/* SutureOS: real ext2 (rev 0 / rev 1) READ-WRITE filesystem driver
 *
 * Read half ported/adapted from Cinux libs/ext2 + kernel/fs/ext2_*.cpp (MIT):
 *   C:\Users\fanqi\Desktop\自研操作系统\Cinux\libs\ext2
 * Adapted to SutureOS: blk_read() block layer, heap scratch buffers, console
 * logging with the [ext2] prefix, a RAM-disk path for in-RAM test images,
 * triple-indirect pointers (Cinux stops at double indirect), on-disk value
 * validation and a built-in guest selftest.
 *
 * Write half (added 2026-11 for the B3 port batch) written for SutureOS after
 * reading two Apache-2.0 implementations as algorithm references - no code was
 * copied, the on-disk layout is the ext2 specification itself:
 *   - XJ380    driver/fs/ext4/ext4.cpp  (Apache-2.0)
 *     C:\Users\fanqi\Desktop\自研操作系统\XJ380\driver\fs\ext4\ext4.cpp
 *   - Deshab-OS CODE/dsk/ext4.c         (Apache-2.0)
 *     C:\Users\fanqi\Desktop\自研操作系统\Deshab-OS\CODE\dsk\ext4.c
 * SutureOS specific write design:
 *   - single-volume context, no cache: every mutation writes the inode and then
 *     flushes the superblock + group descriptor table (ext2_sync()), so the
 *     free counters on disk always match the bitmaps;
 *   - the free counters are *recomputed from the bitmaps* before every flush,
 *     therefore a bug in the incremental accounting can never leave a bad
 *     superblock behind (slow but honest);
 *   - write support covers direct blocks, single indirect and double indirect;
 *     triple indirect writes fail with EXT2_ENOTSUP, and so does truncating a
 *     file that actually uses the triple-indirect pointer;
 *   - backup superblocks / backup group descriptor tables are NOT updated (only
 *     the primary copy at block 1.. and the primary GDT); e2fsck would have to
 *     rebuild them from the primary copy.
 *
 * ---------------------------------------------------------------------------
 * TEST IMAGE CONTENT RULES (must stay in sync with scripts/mkfs_ext2_testimg.py
 * and scripts/verify_ext2_read.py; ext2_selftest() re-derives them here):
 *
 *   /HELLO.TXT     : "Hello from SutureOS ext2!\n"
 *                    "Second line: read via the real ext2 driver.\n"   (70 B)
 *   /SUB/NESTED.TXT: "Nested file: SUB/NESTED.TXT reached through ext2 "
 *                    "directory traversal.\n"                          (70 B)
 *   /BIG.BIN       : byte i == (i * 31 + 7) & 0xFF                 (64 KiB)
 *   /SPARSE.BIN    : byte i == (i * 17 + 3) & 0xFF, except every byte of
 *                    logical block 1 (a HOLE: i_block[1] == 0) == 0
 *                    (3 logical blocks: 3 KiB at 1 KiB blocks)
 *   /DOUBLE.BIN    : byte i == (i * 29 + 11) & 0xFF               (300 KiB)
 *
 * WRITE SELF-TEST ARTIFACT (sync with scripts/verify_ext2_write.py):
 *   /WRT_KEEP.BIN  : byte i == (i * 41 + 5) & 0xFF. Always at least 13 logical
 *                    blocks (12 direct + one single-indirect block). When the
 *                    volume has enough free blocks the artifact grows to
 *                    12 + ptrs_per_block + 8 blocks so it also exercises the
 *                    double-indirect write path (276 KiB @1 KiB blocks); the
 *                    block count is printed on the [ext2] artifact line.
 * ---------------------------------------------------------------------------
 */
#include "ext2.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "block.h"

/* ==================== volume state ==================== */

struct ext2_fs g_ext2_fs = {0};

/* The registered RAM image survives mount/unmount; it belongs to the caller. */
static const uint8_t *g_ram_image = NULL;
static uint32_t g_ram_image_size = 0;

/* Warnings are budgeted so a corrupt image cannot flood the serial log. */
#define EXT2_WARN_BUDGET 32u
static uint32_t g_warn_left = EXT2_WARN_BUDGET;

#define EXT2_MAX_OPEN_FILES 8
#define EXT2_MAX_OPEN_DIRS  8
static struct ext2_file g_files[EXT2_MAX_OPEN_FILES];
static struct ext2_dir  g_dirs[EXT2_MAX_OPEN_DIRS];

static int  ext2_mount_current_device(void);
static int  ext2_read_inode_raw(uint32_t ino, struct ext2_inode *out);
static uint32_t ext2_bmap(const struct ext2_inode *in, uint64_t logical, uint8_t *scratch);
static int  ext2_resolve(const char *path, uint32_t *out_ino, struct ext2_inode *out_inode);
static void ext2_teardown(void);
static int  ext2_dev_write_block(uint32_t block, const void *buf);
static int  ext2_write_inode_raw(uint32_t ino, const struct ext2_inode *in);
static int  ext2_free_inode_raw(uint32_t ino);
static int  ext2_bmap_alloc(struct ext2_inode *in, uint64_t logical, int create,
                            uint32_t *out_phys, uint32_t *new_blocks, uint8_t *tab);
static int  ext2_truncate_blocks(struct ext2_inode *in, uint32_t keep_blocks,
                                 uint32_t *freed);
static int  ext2_write_data(uint32_t ino, struct ext2_inode *in, const void *buf,
                            uint32_t offset, uint32_t size);
static int  ext2_dir_add_entry(struct ext2_inode *dir, const char *name,
                               uint32_t name_len, uint32_t child_ino, uint8_t ftype);
static int  ext2_dir_remove_entry(struct ext2_inode *dir, const char *name,
                                  uint32_t name_len, uint32_t *out_ino,
                                  uint8_t *out_ftype);
static int  ext2_dir_set_dotdot(struct ext2_inode *dir, uint32_t new_parent);
static int  ext2_dir_is_empty(const struct ext2_inode *dir);
static int  ext2_split_path(const char *path, uint32_t *out_parent_ino,
                            struct ext2_inode *out_parent, char *leaf,
                            uint32_t leaf_cap, uint32_t *out_leaf_len);
static int  ext2_create_or_mkdir(const char *path, uint32_t mode, int is_dir,
                                 uint32_t *out_ino);

/* ==================== small helpers ==================== */

static uint32_t ext2_rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t ext2_rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static void ext2_wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void ext2_wr16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}

/* Read/write a big-endian-free 32-bit value inside a pointer table buffer. */
static uint32_t ext2_tbl_get(const uint8_t *tab, uint64_t index) {
    return ext2_rd32(tab + index * 4u);
}

static void ext2_tbl_set(uint8_t *tab, uint64_t index, uint32_t value) {
    ext2_wr32(tab + index * 4u, value);
}

static uint32_t ext2_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void ext2_warn1(const char *msg, uint64_t a) {
    if (g_warn_left == 0) return;
    g_warn_left--;
    console_printf("[ext2] WARNING: %s: %u\n", msg, a);
    if (g_warn_left == 0) console_puts("[ext2] WARNING: further warnings suppressed\n");
}

static void ext2_warn2(const char *msg, uint64_t a, uint64_t b) {
    if (g_warn_left == 0) return;
    g_warn_left--;
    console_printf("[ext2] WARNING: %s: %u %u\n", msg, a, b);
    if (g_warn_left == 0) console_puts("[ext2] WARNING: further warnings suppressed\n");
}

/* ==================== CRC-32 (IEEE 802.3, == Python zlib.crc32) ==================== */

static uint32_t ext2_crc_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

uint32_t ext2_crc32(const void *data, uint32_t len) {
    return ext2_crc_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/* ==================== device I/O ==================== */

/* One whole filesystem block. The RAM path ignores start_lba (the image IS the
 * volume); the block path reads sector-aligned ranges from the block layer. */
static int ext2_dev_read_block(uint32_t block, void *buf) {
    struct ext2_fs *fs = &g_ext2_fs;
    if (!buf) return EXT2_EINVAL;
    if (fs->ram) {
        uint64_t off = (uint64_t)block * fs->block_size;
        if (off + fs->block_size > fs->ram_size) {
            ext2_warn2("ramdisk read past the end of the image", off, fs->ram_size);
            return EXT2_EIO;
        }
        memcpy(buf, fs->ram + off, fs->block_size);
        return EXT2_OK;
    }
    {
        uint64_t lba = (uint64_t)fs->start_lba + (uint64_t)block * fs->sectors_per_block;
        if (blk_read(fs->drive, lba, fs->sectors_per_block, buf) != 0) {
            ext2_warn1("block read failed at lba", lba);
            return EXT2_EIO;
        }
    }
    return EXT2_OK;
}

/* The superblock always starts at byte offset 1024. */
static int ext2_read_superblock_bytes(uint8_t *out) {
    struct ext2_fs *fs = &g_ext2_fs;
    if (fs->ram) {
        if (fs->ram_size < EXT2_SUPERBLOCK_OFFSET + EXT2_SUPERBLOCK_SIZE) return EXT2_EIO;
        memcpy(out, fs->ram + EXT2_SUPERBLOCK_OFFSET, EXT2_SUPERBLOCK_SIZE);
        return EXT2_OK;
    }
    if (blk_read(fs->drive, (uint64_t)fs->start_lba + 2, 2, out) != 0) return EXT2_EIO;
    return EXT2_OK;
}

/* ==================== lifecycle ==================== */

static void ext2_teardown(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    if (fs->bgdt) {
        lemis_heap_free(fs->bgdt);
        fs->bgdt = NULL;
    }
    fs->bgdt_capacity = 0;
    fs->mounted = 0;
    fs->ram = NULL;
    fs->ram_w = NULL;
    fs->ram_size = 0;
}

static int ext2_mount_fail(const char *reason) {
    console_printf("[ext2] mount failed: %s\n", reason);
    ext2_teardown();
    return EXT2_ERR;
}

/* Fail with a formatted reason (short buffer: static text + one or two
 * numbers). Arguments stay 32-bit so both the kernel vsnprintf and a host
 * snprintf read them correctly for %u / %x. */
static int ext2_mount_fail2(const char *reason, uint32_t a, uint32_t b) {
    char buf[96];
    snprintf(buf, sizeof(buf), reason, (unsigned)a, (unsigned)b);
    return ext2_mount_fail(buf);
}

int ext2_register_ramdisk(const void *image, uint32_t size) {
    if (!image || size < (EXT2_SUPERBLOCK_OFFSET + EXT2_SUPERBLOCK_SIZE)) {
        console_puts("[ext2] register_ramdisk failed: image is NULL or too small\n");
        return EXT2_EINVAL;
    }
    g_ram_image = (const uint8_t *)image;
    g_ram_image_size = size;
    return EXT2_OK;
}

int ext2_ramdisk_registered(void) {
    return g_ram_image != NULL;
}

int ext2_mount(int drive, uint32_t start_lba) {
    ext2_teardown();
    g_ext2_fs.drive = drive;
    g_ext2_fs.start_lba = start_lba;
    g_ext2_fs.ram = NULL;
    g_ext2_fs.ram_w = NULL;
    g_ext2_fs.ram_size = 0;
    return ext2_mount_current_device();
}

/* A RAM image is writable through the const-cast below: the caller handed us a
 * live buffer and keeps it alive, so the write path may update it in place.
 * ext2_register_ramdisk() is documented as "the caller keeps the buffer alive";
 * a caller that needs a read-only RAM volume can simply never mount it rw. */
int ext2_mount_ramdisk(void) {
    if (!g_ram_image) return ext2_mount_fail("no ramdisk registered");
    ext2_teardown();
    g_ext2_fs.ram = g_ram_image;
    g_ext2_fs.ram_w = (uint8_t *)g_ram_image;
    g_ext2_fs.ram_size = g_ram_image_size;
    g_ext2_fs.drive = -1;
    g_ext2_fs.start_lba = 0;
    return ext2_mount_current_device();
}

int ext2_unmount(void) {
    ext2_teardown();
    return EXT2_OK;
}

int ext2_is_mounted(void) { return g_ext2_fs.mounted; }
uint32_t ext2_block_size(void) { return g_ext2_fs.block_size; }

static int ext2_mount_current_device(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *sb_bytes;
    int r;

    g_warn_left = EXT2_WARN_BUDGET;

    sb_bytes = (uint8_t *)lemis_heap_alloc(EXT2_SUPERBLOCK_SIZE);
    if (!sb_bytes) return ext2_mount_fail("out of memory reading the superblock");
    r = ext2_read_superblock_bytes(sb_bytes);
    if (r != EXT2_OK) {
        lemis_heap_free(sb_bytes);
        return ext2_mount_fail("I/O error reading the superblock at offset 1024");
    }
    memset(&fs->sb, 0, sizeof(fs->sb));
    memcpy(&fs->sb, sb_bytes, sizeof(fs->sb));
    lemis_heap_free(sb_bytes);

    /* ---- superblock validation (never trust on-disk values) ---- */
    if (fs->sb.s_magic != EXT2_SUPER_MAGIC) {
        return ext2_mount_fail2("invalid magic 0x%x (want 0xEF53)", fs->sb.s_magic, 0);
    }
    if (fs->sb.s_log_block_size > 2) {
        return ext2_mount_fail2("unsupported block size 2^%u (max 4096)",
                                fs->sb.s_log_block_size, 0);
    }
    fs->block_size = 1024u << fs->sb.s_log_block_size;
    fs->sectors_per_block = fs->block_size / EXT2_SECTOR_SIZE;
    fs->ptrs_per_block = fs->block_size / 4u;

    if (fs->sb.s_inodes_per_group == 0) return ext2_mount_fail("s_inodes_per_group is 0");
    if (fs->sb.s_blocks_per_group == 0) return ext2_mount_fail("s_blocks_per_group is 0");
    if (fs->sb.s_blocks_count == 0) return ext2_mount_fail("s_blocks_count is 0");
    if (fs->sb.s_inodes_count == 0) return ext2_mount_fail("s_inodes_count is 0");
    if (fs->sb.s_rev_level > 1) {
        return ext2_mount_fail2("unsupported revision %u", fs->sb.s_rev_level, 0);
    }

    fs->first_data_block = fs->sb.s_first_data_block;
    {
        uint32_t expect = (fs->block_size == 1024) ? 1u : 0u;
        if (fs->first_data_block != expect) {
            return ext2_mount_fail2("s_first_data_block is %u, expected %u",
                                    fs->first_data_block, expect);
        }
    }

    fs->rev_level = fs->sb.s_rev_level;
    fs->inode_size = (fs->rev_level == 0) ? EXT2_INODE_SIZE_REV0 : fs->sb.s_inode_size;
    if (fs->inode_size < EXT2_INODE_SIZE_REV0 ||
        fs->inode_size > fs->block_size ||
        (fs->block_size % fs->inode_size) != 0) {
        return ext2_mount_fail2("bad inode size %u for block size", fs->inode_size,
                                fs->block_size);
    }

    /* First non-reserved inode: s_first_ino is a revision-1 field, revision 0
     * always reserves 1..10 (ext2 convention, same as mke2fs). */
    fs->first_ino = (fs->rev_level == 0) ? 11u : fs->sb.s_first_ino;
    if (fs->first_ino < 11u || fs->first_ino > fs->inodes_count) fs->first_ino = 11u;

    if ((fs->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_UNSUPPORTED) != 0) {
        return ext2_mount_fail2("unsupported incompat features 0x%x (extents/64bit/"
                                "journal/meta_bg/mmp)", fs->sb.s_feature_incompat, 0);
    }

    /* ---- cap the block count by the real device size ---- */
    {
        uint64_t dev_blocks;
        if (fs->ram) {
            dev_blocks = (uint64_t)fs->ram_size / fs->block_size;
        } else {
            uint64_t sectors = 0;
            if (blk_get_info(fs->drive, &sectors, NULL) != 0 || sectors == 0) {
                ext2_warn1("device size unknown, trusting s_blocks_count on drive",
                           (uint64_t)fs->drive);
                dev_blocks = fs->sb.s_blocks_count;
            } else if (sectors <= fs->start_lba) {
                return ext2_mount_fail("device is smaller than the partition offset");
            } else {
                dev_blocks = (sectors - fs->start_lba) / fs->sectors_per_block;
            }
        }
        fs->blocks_count_raw = fs->sb.s_blocks_count;
        if ((uint64_t)fs->sb.s_blocks_count > dev_blocks) {
            ext2_warn2("s_blocks_count exceeds the device, clamping to",
                       fs->sb.s_blocks_count, dev_blocks);
            fs->blocks_count = (uint32_t)dev_blocks;
        } else {
            fs->blocks_count = fs->sb.s_blocks_count;
        }
        if (fs->blocks_count == 0) return ext2_mount_fail("no usable blocks on the device");
    }

    /* ---- group count ---- */
    {
        uint64_t groups = ((uint64_t)fs->blocks_count + fs->sb.s_blocks_per_group - 1) /
                          fs->sb.s_blocks_per_group;
        uint64_t ino_groups = ((uint64_t)fs->sb.s_inodes_count + fs->sb.s_inodes_per_group - 1) /
                              fs->sb.s_inodes_per_group;
        if (ino_groups > groups) groups = ino_groups;
        if (groups == 0 || groups > EXT2_MAX_GROUPS) {
            return ext2_mount_fail2("unsupported group count %u (max %u)", (uint32_t)groups,
                                    EXT2_MAX_GROUPS);
        }
        fs->group_count = (uint32_t)groups;
    }

    fs->blocks_per_group = fs->sb.s_blocks_per_group;
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    fs->inodes_count = fs->sb.s_inodes_count;

    {
        uint64_t p = fs->ptrs_per_block;
        fs->max_logical = (uint32_t)(EXT2_DIRECT_BLOCKS + p + p * p + p * p * p);
    }

    /* ---- block group descriptor table ---- */
    {
        uint32_t i;
        uint32_t entries_per_block = fs->block_size / (uint32_t)sizeof(struct ext2_block_group_desc);
        uint32_t gdt_blocks = (fs->group_count + entries_per_block - 1) / entries_per_block;
        uint32_t gdt_block = fs->first_data_block + 1;
        uint8_t *buf;
        struct ext2_block_group_desc *table;

        fs->bgdt = (struct ext2_block_group_desc *)
            lemis_heap_alloc((uintptr_t)fs->group_count * sizeof(struct ext2_block_group_desc));
        if (!fs->bgdt) return ext2_mount_fail("out of memory for the group descriptor table");
        fs->bgdt_capacity = fs->group_count;
        fs->gdt_entries_per_block = entries_per_block;
        fs->gdt_block = gdt_block;
        table = fs->bgdt;
        memset(table, 0, (uintptr_t)fs->group_count * sizeof(*table));

        buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
        if (!buf) return ext2_mount_fail("out of memory for the group descriptor buffer");

        for (i = 0; i < gdt_blocks; i++) {
            uint32_t first = i * entries_per_block;
            uint32_t n;
            if (first >= fs->group_count) break;
            n = ext2_min_u32(entries_per_block, fs->group_count - first);
            r = ext2_dev_read_block(gdt_block + i, buf);
            if (r != EXT2_OK) {
                lemis_heap_free(buf);
                return ext2_mount_fail("I/O error reading the group descriptor table");
            }
            memcpy(&table[first], buf, (uintptr_t)n * sizeof(struct ext2_block_group_desc));
        }
        lemis_heap_free(buf);
    }

    /* ---- per-group sanity ---- */
    {
        uint32_t g;
        uint32_t table_blocks =
            (fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
        for (g = 0; g < fs->group_count; g++) {
            struct ext2_block_group_desc *gd = &fs->bgdt[g];
            if (gd->bg_inode_table == 0 || gd->bg_inode_table >= fs->blocks_count ||
                (uint64_t)gd->bg_inode_table + table_blocks > fs->blocks_count) {
                if (g == 0) {
                    return ext2_mount_fail2("group 0 inode table %u is out of range",
                                            gd->bg_inode_table, fs->blocks_count);
                }
                ext2_warn2("group inode table out of range, group skipped", g,
                           gd->bg_inode_table);
            }
            if (gd->bg_block_bitmap >= fs->blocks_count) {
                ext2_warn2("group block bitmap out of range", g, gd->bg_block_bitmap);
            }
            if (gd->bg_inode_bitmap >= fs->blocks_count) {
                ext2_warn2("group inode bitmap out of range", g, gd->bg_inode_bitmap);
            }
        }
    }

    /* ---- the root inode must exist and be a directory ---- */
    {
        struct ext2_inode root;
        if (ext2_read_inode_raw(2, &root) != EXT2_OK) {
            return ext2_mount_fail("cannot read the root inode (inode 2)");
        }
        if ((root.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
            return ext2_mount_fail2("root inode 2 is not a directory (mode 0x%x)",
                                    root.i_mode, 0);
        }
    }

    if ((fs->sb.s_state & EXT2_VALID_FS) == 0 || (fs->sb.s_state & EXT2_ERROR_FS) != 0) {
        ext2_warn1("filesystem is not marked clean; mounting read-write anyway",
                   fs->sb.s_state);
    }

    fs->mounted = 1;
    /* THE mount line (exactly one, as specified) */
    console_printf("[ext2] mount blocksz=%u blocks=%u inodes=%u groups=%u rev=%u\n",
                   (uint64_t)fs->block_size, (uint64_t)fs->blocks_count,
                   (uint64_t)fs->inodes_count, (uint64_t)fs->group_count,
                   (uint64_t)fs->rev_level);
    /* Honest scope note, printed once per mount: only the primary superblock and
     * the primary group descriptor table are written back. The backup copies
     * (blocks 8193/16385/..., present when the volume has >1 group) are left
     * untouched and would have to be regenerated by e2fsck from the primary. */
    console_puts("[ext2] read-write: primary superblock/GDT flushed on every "
                 "write, backup copies not updated\n");
    return EXT2_OK;
}

/* ==================== blocks / inodes ==================== */

int ext2_read_block_raw(uint32_t block, void *buf) {
    if (!buf) return EXT2_EINVAL;
    if (!g_ext2_fs.mounted) return EXT2_EBUSY;
    if (block >= g_ext2_fs.blocks_count) {
        ext2_warn2("block read out of range", block, g_ext2_fs.blocks_count);
        return EXT2_EINVAL;
    }
    return ext2_dev_read_block(block, buf);
}

static int ext2_read_inode_raw(uint32_t ino, struct ext2_inode *out) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t group, index, byte_off, blk, off;
    uint8_t *buf;
    int r;

    if (!out) return EXT2_EINVAL;
    if (ino == 0 || ino > fs->inodes_count) {
        ext2_warn2("inode number out of range", ino, fs->inodes_count);
        return EXT2_EINVAL;
    }
    group = (ino - 1) / fs->inodes_per_group;
    if (group >= fs->group_count) {
        ext2_warn2("inode maps past the last block group", ino, group);
        return EXT2_EINVAL;
    }
    index = (ino - 1) % fs->inodes_per_group;
    byte_off = index * fs->inode_size;
    blk = fs->bgdt[group].bg_inode_table + byte_off / fs->block_size;
    off = byte_off % fs->block_size;
    if (blk >= fs->blocks_count) {
        ext2_warn2("inode table block out of range for inode", ino, blk);
        return EXT2_EINVAL;
    }
    if (off + fs->inode_size > fs->block_size) {
        /* cannot happen while block_size % inode_size == 0 (checked at mount) */
        ext2_warn2("inode crosses a block boundary", ino, off);
        return EXT2_EIO;
    }

    buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!buf) return EXT2_ENOMEM;
    r = ext2_dev_read_block(blk, buf);
    if (r == EXT2_OK) {
        uint32_t copy = fs->inode_size < sizeof(*out) ? fs->inode_size : (uint32_t)sizeof(*out);
        memset(out, 0, sizeof(*out));
        memcpy(out, buf + off, copy);
    }
    lemis_heap_free(buf);
    return r;
}

int ext2_read_inode(uint32_t ino, struct ext2_inode *out) {
    if (!g_ext2_fs.mounted) return EXT2_EBUSY;
    return ext2_read_inode_raw(ino, out);
}

/* Validate a data-block pointer: 0 is a hole, anything >= blocks_count is
 * treated as a hole after a warning (never fault, never read block 0). */
static uint32_t ext2_check_ptr(uint32_t blk) {
    if (blk == 0) return 0;
    if (blk >= g_ext2_fs.blocks_count) {
        ext2_warn2("data block pointer out of range, treating as a hole", blk,
                   g_ext2_fs.blocks_count);
        return 0;
    }
    return blk;
}

/* Map a logical file block to a physical block (0 = hole / unmapped).
 * Depth is bounded: direct, one table, two tables, three tables - never
 * recursive, so a corrupt pointer chain cannot loop. */
static uint32_t ext2_bmap(const struct ext2_inode *in, uint64_t logical, uint8_t *scratch) {
    struct ext2_fs *fs = &g_ext2_fs;
    const uint64_t ptrs = fs->ptrs_per_block;
    uint64_t idx;
    uint32_t cur;

    if (logical < EXT2_DIRECT_BLOCKS) {
        return ext2_check_ptr(in->i_block[logical]);
    }

    if (logical < EXT2_DIRECT_BLOCKS + ptrs) {                 /* single indirect */
        idx = logical - EXT2_DIRECT_BLOCKS;
        cur = ext2_check_ptr(in->i_block[EXT2_INDIRECT_BLOCK]);
        if (cur == 0) return 0;
        if (ext2_dev_read_block(cur, scratch) != EXT2_OK) return 0;
        return ext2_check_ptr(ext2_rd32(scratch + idx * 4u));
    }

    if (logical < EXT2_DIRECT_BLOCKS + ptrs + ptrs * ptrs) {    /* double indirect */
        idx = logical - EXT2_DIRECT_BLOCKS - ptrs;
        cur = ext2_check_ptr(in->i_block[EXT2_DOUBLE_INDIRECT]);
        if (cur == 0) return 0;
        if (ext2_dev_read_block(cur, scratch) != EXT2_OK) return 0;
        cur = ext2_check_ptr(ext2_rd32(scratch + (idx / ptrs) * 4u));
        if (cur == 0) return 0;
        if (ext2_dev_read_block(cur, scratch) != EXT2_OK) return 0;
        return ext2_check_ptr(ext2_rd32(scratch + (idx % ptrs) * 4u));
    }

    if (logical < (uint64_t)EXT2_DIRECT_BLOCKS + ptrs + ptrs * ptrs + ptrs * ptrs * ptrs) {
        /* triple indirect: exactly three table reads */
        uint64_t rem = logical - EXT2_DIRECT_BLOCKS - ptrs - ptrs * ptrs;
        cur = ext2_check_ptr(in->i_block[EXT2_TRIPLE_INDIRECT]);
        if (cur == 0) return 0;
        if (ext2_dev_read_block(cur, scratch) != EXT2_OK) return 0;
        cur = ext2_check_ptr(ext2_rd32(scratch + (rem / (ptrs * ptrs)) * 4u));
        if (cur == 0) return 0;
        if (ext2_dev_read_block(cur, scratch) != EXT2_OK) return 0;
        cur = ext2_check_ptr(ext2_rd32(scratch + ((rem / ptrs) % ptrs) * 4u));
        if (cur == 0) return 0;
        if (ext2_dev_read_block(cur, scratch) != EXT2_OK) return 0;
        return ext2_check_ptr(ext2_rd32(scratch + (rem % ptrs) * 4u));
    }

    ext2_warn2("logical block beyond the triple-indirect range", logical, fs->max_logical);
    return 0;
}

/* Read `size` bytes at `offset` out of an inode. Holes are zero-filled (block 0
 * is never read). scratch must hold one filesystem block. */
static int ext2_inode_read(const struct ext2_inode *in, void *buf, uint32_t offset,
                           uint32_t size, uint8_t *scratch) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t file_size = in->i_size;
    uint32_t done = 0;

    if (!buf && size != 0) return EXT2_EINVAL;
    if (offset >= file_size) return 0;
    if (size > file_size - offset) size = file_size - offset;

    while (done < size) {
        uint64_t abs = (uint64_t)offset + done;
        uint64_t logical = abs / fs->block_size;
        uint32_t boff = (uint32_t)(abs % fs->block_size);
        uint32_t chunk = fs->block_size - boff;
        uint32_t phys;
        if (chunk > size - done) chunk = size - done;

        phys = ext2_bmap(in, logical, scratch);
        if (phys == 0) {
            memset(dst + done, 0, chunk);            /* sparse hole */
        } else {
            if (ext2_dev_read_block(phys, scratch) != EXT2_OK) return EXT2_EIO;
            memcpy(dst + done, scratch + boff, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

/* ==================== directories / path resolution ==================== */

/* Walk every block of a directory looking for `name`.
 * Returns EXT2_OK, EXT2_ENOENT (not found) or a negative error. */
static int ext2_dir_find(const struct ext2_inode *dir, const char *name, uint32_t name_len,
                         uint32_t *out_ino, uint8_t *out_type) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t blocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    uint32_t has_ftype = (fs->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    uint8_t *scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    uint32_t b;

    if (!scratch) return EXT2_ENOMEM;

    for (b = 0; b < blocks; b++) {
        uint32_t phys = ext2_bmap(dir, b, scratch);
        uint32_t pos = 0;
        if (phys == 0) continue;                     /* hole inside a directory */
        if (ext2_dev_read_block(phys, scratch) != EXT2_OK) {
            lemis_heap_free(scratch);
            return EXT2_EIO;
        }
        while (pos + EXT2_DIR_ENTRY_HDR <= fs->block_size) {
            uint32_t eino = ext2_rd32(scratch + pos);
            uint32_t rec = ext2_rd16(scratch + pos + 4);
            uint32_t nlen = ext2_rd16(scratch + pos + 6);
            if (!has_ftype) nlen &= 0xFFFFu; else nlen &= 0xFFu;
            if (rec < EXT2_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > fs->block_size - pos || nlen > rec - EXT2_DIR_ENTRY_HDR) {
                ext2_warn2("corrupt directory entry (rec_len/name_len)", rec, pos);
                break;                               /* stop scanning this block */
            }
            if (eino != 0 && nlen == name_len &&
                memcmp(scratch + pos + EXT2_DIR_ENTRY_HDR, name, name_len) == 0) {
                if (out_ino) *out_ino = eino;
                if (out_type) *out_type = has_ftype ? scratch[pos + 7] : EXT2_FT_UNKNOWN;
                lemis_heap_free(scratch);
                return EXT2_OK;
            }
            pos += rec;
        }
    }
    lemis_heap_free(scratch);
    return EXT2_ENOENT;
}

/* Resolve a volume-relative path ("/", "/HELLO.TXT", "/SUB/../HELLO.TXT").
 * "." and ".." are resolved through the real directory entries. */
static int ext2_resolve(const char *path, uint32_t *out_ino, struct ext2_inode *out_inode) {
    struct ext2_inode ino;
    uint32_t cur = 2;
    const char *p;
    int r;

    if (!path) return EXT2_EINVAL;
    r = ext2_read_inode_raw(2, &ino);
    if (r != EXT2_OK) return EXT2_EIO;

    p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[EXT2_NAME_MAX + 1];
        uint32_t len = 0;
        uint32_t next = 0;

        while (*p && *p != '/') {
            if (len >= EXT2_NAME_MAX) {
                ext2_warn1("path component longer than 255 bytes at offset",
                           (uint64_t)(p - path));
                return EXT2_EINVAL;
            }
            comp[len++] = *p++;
        }
        while (*p == '/') p++;
        if (len == 0) continue;
        comp[len] = '\0';

        if ((ino.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return EXT2_ENOTDIR;
        r = ext2_dir_find(&ino, comp, len, &next, NULL);
        if (r != EXT2_OK) return EXT2_ENOENT;
        r = ext2_read_inode_raw(next, &ino);
        if (r != EXT2_OK) return EXT2_EIO;
        cur = next;
    }

    if (out_ino) *out_ino = cur;
    if (out_inode) *out_inode = ino;

    /* A trailing slash demands a directory ("/SUB/" ok, "/HELLO.TXT/" not). */
    {
        size_t plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/' &&
            (ino.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
            return EXT2_ENOTDIR;
        }
    }
    return EXT2_OK;
}

/* ==================== public file API ==================== */

int ext2_lookup(const char *path, uint32_t *out_ino) {
    if (!g_ext2_fs.mounted) return EXT2_EBUSY;
    if (!out_ino) return EXT2_EINVAL;
    return ext2_resolve(path, out_ino, NULL);
}

int ext2_stat(const char *path, struct ext2_stat *out) {
    uint32_t ino;
    struct ext2_inode in;
    int r;

    if (!g_ext2_fs.mounted) return EXT2_EBUSY;
    if (!out) return EXT2_EINVAL;
    r = ext2_resolve(path, &ino, &in);
    if (r != EXT2_OK) return r;

    memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->mode = in.i_mode;
    out->size = in.i_size;
    out->uid = in.i_uid;
    out->gid = in.i_gid;
    out->links = in.i_links_count;
    out->atime = in.i_atime;
    out->mtime = in.i_mtime;
    out->ctime = in.i_ctime;
    out->is_dir = ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;
    out->is_file = ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) ? 1 : 0;
    out->is_symlink = ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) ? 1 : 0;
    if (out->is_file && in.i_dir_acl != 0) {
        ext2_warn1("file larger than 4 GiB; only the low 32 bits are addressable", ino);
    }
    return EXT2_OK;
}

int ext2_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode in;
    uint8_t *scratch;
    int r;

    if (!fs->mounted) return EXT2_EBUSY;
    if (!buf && size != 0) return EXT2_EINVAL;
    r = ext2_read_inode_raw(ino, &in);
    if (r != EXT2_OK) return r;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_EISDIR;

    scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scratch) return EXT2_ENOMEM;
    r = ext2_inode_read(&in, buf, offset, size, scratch);
    lemis_heap_free(scratch);
    return r;
}

int ext2_open(const char *path, struct ext2_file **out) {
    uint32_t ino;
    struct ext2_inode in;
    struct ext2_file *f = NULL;
    int i, r;

    if (!g_ext2_fs.mounted) return EXT2_EBUSY;
    if (!out) return EXT2_EINVAL;
    r = ext2_resolve(path, &ino, &in);
    if (r != EXT2_OK) return r;

    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_EISDIR;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return EXT2_ENOTSUP;

    for (i = 0; i < EXT2_MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) { f = &g_files[i]; break; }
    }
    if (!f) return EXT2_EBUSY;

    f->used = 1;
    f->ino = ino;
    f->size = in.i_size;
    *out = f;
    return EXT2_OK;
}

int ext2_file_read(struct ext2_file *f, void *buf, uint32_t size, uint32_t offset) {
    if (!f || !f->used) return EXT2_EINVAL;
    if (offset >= f->size) return 0;                       /* EOF */
    if (size > f->size - offset) size = f->size - offset;
    return ext2_read_file(f->ino, buf, offset, size);
}

int ext2_close(struct ext2_file *f) {
    if (!f) return EXT2_EINVAL;
    memset(f, 0, sizeof(*f));
    return EXT2_OK;
}

int ext2_opendir(const char *path, struct ext2_dir **out) {
    uint32_t ino;
    struct ext2_inode in;
    struct ext2_dir *d = NULL;
    int i, r;

    if (!g_ext2_fs.mounted) return EXT2_EBUSY;
    if (!out) return EXT2_EINVAL;
    r = ext2_resolve(path, &ino, &in);
    if (r != EXT2_OK) return r;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return EXT2_ENOTDIR;

    for (i = 0; i < EXT2_MAX_OPEN_DIRS; i++) {
        if (!g_dirs[i].used) { d = &g_dirs[i]; break; }
    }
    if (!d) return EXT2_EBUSY;

    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->ino = ino;
    d->size = in.i_size;
    d->block = 0;
    d->offset = 0;
    d->inode = in;
    *out = d;
    return EXT2_OK;
}

/* Returns EXT2_OK for one entry, EXT2_ENOENT at the end (FAT32 convention).
 * "." and ".." are skipped. Names longer than name_cap-1 are truncated. */
int ext2_readdir(struct ext2_dir *d, char *name, uint32_t name_cap,
                 uint32_t *name_len, int *is_dir) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t has_ftype = (fs->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    uint8_t *scratch;
    int rc = EXT2_ENOENT;

    if (!d || !d->used || !name || name_cap == 0) return EXT2_EINVAL;
    if (!fs->mounted) return EXT2_EBUSY;

    scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scratch) return EXT2_ENOMEM;

    while ((uint64_t)d->block * fs->block_size < d->size) {
        uint32_t phys = ext2_bmap(&d->inode, d->block, scratch);
        int block_done = 0;

        if (phys == 0) {                                  /* hole: skip the block */
            d->block++;
            d->offset = 0;
            continue;
        }
        if (ext2_dev_read_block(phys, scratch) != EXT2_OK) { rc = EXT2_EIO; break; }

        while (d->offset + EXT2_DIR_ENTRY_HDR <= fs->block_size) {
            uint32_t eino = ext2_rd32(scratch + d->offset);
            uint32_t rec = ext2_rd16(scratch + d->offset + 4);
            uint32_t nlen = ext2_rd16(scratch + d->offset + 6);
            uint8_t ftype = scratch[d->offset + 7];
            uint32_t copy;
            const uint8_t *nm = scratch + d->offset + EXT2_DIR_ENTRY_HDR;

            if (!has_ftype) nlen &= 0xFFFFu; else nlen &= 0xFFu;
            if (rec < EXT2_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > fs->block_size - d->offset || nlen > rec - EXT2_DIR_ENTRY_HDR) {
                ext2_warn2("corrupt directory entry (rec_len/name_len)", rec, d->offset);
                block_done = 1;
                break;
            }
            d->offset += rec;
            if (eino == 0) continue;
            if (nlen == 1 && nm[0] == '.') continue;
            if (nlen == 2 && nm[0] == '.' && nm[1] == '.') continue;

            copy = ext2_min_u32(nlen, name_cap - 1);
            memcpy(name, nm, copy);
            name[copy] = '\0';
            if (name_len) *name_len = copy;
            if (is_dir) {
                if (has_ftype) {
                    *is_dir = (ftype == EXT2_FT_DIR) ? 1 : 0;
                } else {
                    struct ext2_inode child;
                    if (ext2_read_inode_raw(eino, &child) != EXT2_OK) {
                        rc = EXT2_EIO;
                        goto out;
                    }
                    *is_dir = ((child.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;
                }
            }
            rc = EXT2_OK;
            goto out;
        }
        if (block_done || d->offset + EXT2_DIR_ENTRY_HDR > fs->block_size) {
            d->block++;
            d->offset = 0;
        }
    }

out:
    lemis_heap_free(scratch);
    return rc;
}

int ext2_closedir(struct ext2_dir *d) {
    if (!d) return EXT2_EINVAL;
    memset(d, 0, sizeof(*d));
    return EXT2_OK;
}

/* =========================================================================
 * WRITE PATH - bitmap allocation and metadata flushing
 *
 * Design note: the free counters are never trusted as a source of truth. Every
 * mutating operation ends in ext2_sync(), which recounts the free blocks and
 * free inodes straight from the bitmaps and writes those numbers into the
 * superblock and the group descriptor table. The volume is therefore always
 * self-consistent on disk, at the cost of one bitmap scan per operation.
 * ========================================================================= */

/* One whole filesystem block. Mirrors ext2_dev_read_block(). */
static int ext2_dev_write_block(uint32_t block, const void *buf) {
    struct ext2_fs *fs = &g_ext2_fs;
    if (!buf) return EXT2_EINVAL;
    if (fs->ram) {
        uint64_t off = (uint64_t)block * fs->block_size;
        if (!fs->ram_w) {
            ext2_warn1("volume is a read-only RAM image, cannot write block", block);
            return EXT2_EROFS;
        }
        if (off + fs->block_size > fs->ram_size) {
            ext2_warn2("ramdisk write past the end of the image", off, fs->ram_size);
            return EXT2_EIO;
        }
        memcpy(fs->ram_w + off, buf, fs->block_size);
        return EXT2_OK;
    }
    {
        uint64_t lba = (uint64_t)fs->start_lba + (uint64_t)block * fs->sectors_per_block;
        if (blk_write(fs->drive, lba, fs->sectors_per_block, buf) != 0) {
            ext2_warn1("block write failed at lba", lba);
            return EXT2_EIO;
        }
    }
    return EXT2_OK;
}

/* Map a block number to (group, bit inside that group's block bitmap).
 * ext2 bit i of group g is the absolute block first_data_block + g*bpg + i. */
static int ext2_block_bit_of(uint32_t blk, uint32_t *out_group, uint32_t *out_bit) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t rel;
    if (!out_group || !out_bit) return EXT2_EINVAL;
    *out_group = 0;
    *out_bit = 0;
    if (blk < fs->first_data_block || blk >= fs->blocks_count) return EXT2_EINVAL;
    rel = blk - fs->first_data_block;
    *out_group = rel / fs->blocks_per_group;
    *out_bit = rel % fs->blocks_per_group;
    if (*out_group >= fs->group_count) return EXT2_EINVAL;
    return EXT2_OK;
}

/* Map an inode number to (group, bit inside that group's inode bitmap). */
static int ext2_inode_bit_of(uint32_t ino, uint32_t *out_group, uint32_t *out_bit) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t rel;
    if (!out_group || !out_bit) return EXT2_EINVAL;
    *out_group = 0;
    *out_bit = 0;
    if (ino == 0 || ino > fs->inodes_count) return EXT2_EINVAL;
    rel = ino - 1;
    *out_group = rel / fs->inodes_per_group;
    *out_bit = rel % fs->inodes_per_group;
    if (*out_group >= fs->group_count) return EXT2_EINVAL;
    return EXT2_OK;
}

/* How many blocks the given group really owns (the last group can be short). */
static uint32_t ext2_group_block_limit(uint32_t group) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint64_t first = (uint64_t)fs->first_data_block + (uint64_t)group * fs->blocks_per_group;
    uint64_t left = (first >= fs->blocks_count) ? 0 : ((uint64_t)fs->blocks_count - first);
    if (left > fs->blocks_per_group) left = fs->blocks_per_group;
    return (uint32_t)left;
}

/* How many inodes the given group really owns. */
static uint32_t ext2_group_inode_limit(uint32_t group) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint64_t first = (uint64_t)group * fs->inodes_per_group;
    uint64_t left = (first >= fs->inodes_count) ? 0 : ((uint64_t)fs->inodes_count - first);
    if (left > fs->inodes_per_group) left = fs->inodes_per_group;
    return (uint32_t)left;
}

/* Allocate one block (first fit over the real bitmaps, zeroed on return).
 * The new block is written as all-zero because ext2 requires zeroed indirect
 * tables and directory blocks, and a zeroed file tail keeps a short write
 * deterministic. */
static int ext2_alloc_block_raw(uint32_t *out) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *bm;
    uint32_t g;
    int rc = EXT2_ENOSPC;

    if (!out) return EXT2_EINVAL;
    if (!fs->mounted) return EXT2_EBUSY;
    bm = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!bm) return EXT2_ENOMEM;

    for (g = 0; g < fs->group_count && rc == EXT2_ENOSPC; g++) {
        struct ext2_block_group_desc *gd = &fs->bgdt[g];
        uint32_t limit = ext2_group_block_limit(g);
        uint32_t i;

        if (limit == 0 || gd->bg_block_bitmap == 0 ||
            gd->bg_block_bitmap >= fs->blocks_count) {
            continue;
        }
        if (ext2_dev_read_block(gd->bg_block_bitmap, bm) != EXT2_OK) {
            rc = EXT2_EIO;
            break;
        }
        for (i = 0; i < limit; i++) {
            uint32_t blk;
            if (bm[i >> 3] & (uint8_t)(1u << (i & 7u))) continue;
            bm[i >> 3] |= (uint8_t)(1u << (i & 7u));
            if (ext2_dev_write_block(gd->bg_block_bitmap, bm) != EXT2_OK) {
                rc = EXT2_EIO;
                break;
            }
            blk = fs->first_data_block + g * fs->blocks_per_group + i;
            memset(bm, 0, fs->block_size);
            if (ext2_dev_write_block(blk, bm) != EXT2_OK) {
                rc = EXT2_EIO;
                break;
            }
            *out = blk;
            rc = EXT2_OK;
            break;
        }
    }
    lemis_heap_free(bm);
    return rc;
}

/* Return one block to the bitmap. A double free is reported and refused. */
static int ext2_free_block_raw(uint32_t blk) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *bm;
    uint32_t group, bit;
    int r;

    r = ext2_block_bit_of(blk, &group, &bit);
    if (r != EXT2_OK) {
        ext2_warn1("refusing to free an out-of-range block", blk);
        return EXT2_EINVAL;
    }
    bm = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!bm) return EXT2_ENOMEM;
    if (ext2_dev_read_block(fs->bgdt[group].bg_block_bitmap, bm) != EXT2_OK) {
        lemis_heap_free(bm);
        return EXT2_EIO;
    }
    if (!(bm[bit >> 3] & (uint8_t)(1u << (bit & 7u)))) {
        ext2_warn2("double free of block (already clear in the bitmap)", blk, bit);
        lemis_heap_free(bm);
        return EXT2_ERR;
    }
    bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7u));
    r = ext2_dev_write_block(fs->bgdt[group].bg_block_bitmap, bm);
    lemis_heap_free(bm);
    return r;
}

/* Allocate one inode number (bit set in the inode bitmap, inode itself is
 * written by the caller). */
static int ext2_alloc_inode_raw(uint32_t *out) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *bm;
    uint32_t g;
    int rc = EXT2_ENOSPC;

    if (!out) return EXT2_EINVAL;
    if (!fs->mounted) return EXT2_EBUSY;
    bm = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!bm) return EXT2_ENOMEM;

    for (g = 0; g < fs->group_count && rc == EXT2_ENOSPC; g++) {
        struct ext2_block_group_desc *gd = &fs->bgdt[g];
        uint32_t limit = ext2_group_inode_limit(g);
        uint32_t i, first_bit = 0;

        if (limit == 0 || gd->bg_inode_bitmap == 0 ||
            gd->bg_inode_bitmap >= fs->blocks_count) {
            continue;
        }
        /* never hand out a reserved inode (1..first_ino-1) */
        if (g == 0) {
            uint32_t reserved = fs->first_ino - 1u;
            if (reserved >= limit) continue;
            first_bit = reserved;
        }
        if (ext2_dev_read_block(gd->bg_inode_bitmap, bm) != EXT2_OK) {
            rc = EXT2_EIO;
            break;
        }
        for (i = first_bit; i < limit; i++) {
            if (bm[i >> 3] & (uint8_t)(1u << (i & 7u))) continue;
            bm[i >> 3] |= (uint8_t)(1u << (i & 7u));
            if (ext2_dev_write_block(gd->bg_inode_bitmap, bm) != EXT2_OK) {
                rc = EXT2_EIO;
                break;
            }
            *out = g * fs->inodes_per_group + i + 1u;
            rc = EXT2_OK;
            break;
        }
    }
    lemis_heap_free(bm);
    return rc;
}

/* Free one inode: the on-disk inode is zeroed first (so a stale block pointer
 * can never be followed), then the bitmap bit is cleared. */
static int ext2_free_inode_raw(uint32_t ino) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode zero;
    uint8_t *bm;
    uint32_t group, bit;
    int r;

    r = ext2_inode_bit_of(ino, &group, &bit);
    if (r != EXT2_OK) {
        ext2_warn1("refusing to free an out-of-range inode", ino);
        return EXT2_EINVAL;
    }
    memset(&zero, 0, sizeof(zero));
    r = ext2_write_inode_raw(ino, &zero);
    if (r != EXT2_OK) return r;

    bm = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!bm) return EXT2_ENOMEM;
    if (ext2_dev_read_block(fs->bgdt[group].bg_inode_bitmap, bm) != EXT2_OK) {
        lemis_heap_free(bm);
        return EXT2_EIO;
    }
    if (!(bm[bit >> 3] & (uint8_t)(1u << (bit & 7u)))) {
        ext2_warn2("double free of inode (already clear in the bitmap)", ino, bit);
        lemis_heap_free(bm);
        return EXT2_ERR;
    }
    bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7u));
    r = ext2_dev_write_block(fs->bgdt[group].bg_inode_bitmap, bm);
    lemis_heap_free(bm);
    return r;
}

/* Read-modify-write one inode inside its inode table block. */
static int ext2_write_inode_raw(uint32_t ino, const struct ext2_inode *in) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t group, index, byte_off, blk, off, copy;
    uint8_t *buf;
    int r;

    if (!in) return EXT2_EINVAL;
    if (!fs->mounted) return EXT2_EBUSY;
    if (ino == 0 || ino > fs->inodes_count) {
        ext2_warn2("inode write out of range", ino, fs->inodes_count);
        return EXT2_EINVAL;
    }
    group = (ino - 1) / fs->inodes_per_group;
    if (group >= fs->group_count) return EXT2_EINVAL;
    index = (ino - 1) % fs->inodes_per_group;
    byte_off = index * fs->inode_size;
    blk = fs->bgdt[group].bg_inode_table + byte_off / fs->block_size;
    off = byte_off % fs->block_size;
    if (blk >= fs->blocks_count || off + fs->inode_size > fs->block_size) {
        ext2_warn2("inode table block out of range for inode", ino, blk);
        return EXT2_EINVAL;
    }

    buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!buf) return EXT2_ENOMEM;
    r = ext2_dev_read_block(blk, buf);
    if (r == EXT2_OK) {
        copy = fs->inode_size < sizeof(*in) ? fs->inode_size : (uint32_t)sizeof(*in);
        memset(buf + off, 0, copy);
        memcpy(buf + off, in, copy);
        r = ext2_dev_write_block(blk, buf);
    }
    lemis_heap_free(buf);
    return r;
}

/* Write the 1024-byte superblock area (offset 1024) without touching the boot
 * block bytes that share the same filesystem block at 2 KiB / 4 KiB blocks. */
static int ext2_write_sb(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t sb_block = EXT2_SUPERBLOCK_OFFSET / fs->block_size;
    uint32_t off = EXT2_SUPERBLOCK_OFFSET % fs->block_size;
    uint8_t *buf;
    int r;

    buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!buf) return EXT2_ENOMEM;
    r = ext2_dev_read_block(sb_block, buf);
    if (r == EXT2_OK) {
        memcpy(buf + off, &fs->sb, sizeof(fs->sb));
        r = ext2_dev_write_block(sb_block, buf);
    }
    lemis_heap_free(buf);
    return r;
}

/* Write every group descriptor block back (read-modify-write so entries that do
 * not belong to this table - and the slack after the last entry - survive). */
static int ext2_write_bgdt(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t per = fs->gdt_entries_per_block;
    uint32_t blocks, i;
    uint8_t *buf;
    int r;

    if (per == 0 || fs->gdt_block == 0) return EXT2_ERR;
    blocks = (fs->group_count + per - 1) / per;
    buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!buf) return EXT2_ENOMEM;
    for (i = 0; i < blocks; i++) {
        uint32_t first = i * per;
        uint32_t n;
        if (first >= fs->group_count) break;
        n = fs->group_count - first;
        if (n > per) n = per;
        r = ext2_dev_read_block(fs->gdt_block + i, buf);
        if (r != EXT2_OK) break;
        memcpy(buf, &fs->bgdt[first], (uintptr_t)n * sizeof(struct ext2_block_group_desc));
        r = ext2_dev_write_block(fs->gdt_block + i, buf);
        if (r != EXT2_OK) break;
    }
    lemis_heap_free(buf);
    return r;
}

/* Recount free blocks / free inodes from the bitmaps into the cached superblock
 * and group descriptor table (in memory only; ext2_sync() writes them). */
static void ext2_recount(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *bm;
    uint32_t g, total_b = 0, total_i = 0;

    bm = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!bm) return;                       /* keep the cached numbers */

    for (g = 0; g < fs->group_count; g++) {
        struct ext2_block_group_desc *gd = &fs->bgdt[g];
        uint32_t limit_b = ext2_group_block_limit(g);
        uint32_t limit_i = ext2_group_inode_limit(g);
        uint32_t i, free_b = 0, free_i = 0;

        if (gd->bg_block_bitmap != 0 && gd->bg_block_bitmap < fs->blocks_count &&
            ext2_dev_read_block(gd->bg_block_bitmap, bm) == EXT2_OK) {
            for (i = 0; i < limit_b; i++) {
                if (!(bm[i >> 3] & (uint8_t)(1u << (i & 7u)))) free_b++;
            }
        } else {
            free_b = gd->bg_free_blocks_count;
        }

        if (gd->bg_inode_bitmap != 0 && gd->bg_inode_bitmap < fs->blocks_count &&
            ext2_dev_read_block(gd->bg_inode_bitmap, bm) == EXT2_OK) {
            for (i = 0; i < limit_i; i++) {
                if (!(bm[i >> 3] & (uint8_t)(1u << (i & 7u)))) free_i++;
            }
        } else {
            free_i = gd->bg_free_inodes_count;
        }

        gd->bg_free_blocks_count = (uint16_t)free_b;
        gd->bg_free_inodes_count = (uint16_t)free_i;
        total_b += free_b;
        total_i += free_i;
    }

    fs->sb.s_free_blocks_count = total_b;
    fs->sb.s_free_inodes_count = total_i;
    lemis_heap_free(bm);
}

int ext2_count_free(uint32_t *free_blocks, uint32_t *free_inodes) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *bm;
    uint32_t g, total_b = 0, total_i = 0;

    if (!fs->mounted) return EXT2_EBUSY;
    bm = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!bm) return EXT2_ENOMEM;
    for (g = 0; g < fs->group_count; g++) {
        struct ext2_block_group_desc *gd = &fs->bgdt[g];
        uint32_t limit_b = ext2_group_block_limit(g);
        uint32_t limit_i = ext2_group_inode_limit(g);
        uint32_t i;
        if (gd->bg_block_bitmap == 0 || gd->bg_block_bitmap >= fs->blocks_count) {
            lemis_heap_free(bm);
            return EXT2_EIO;
        }
        if (ext2_dev_read_block(gd->bg_block_bitmap, bm) != EXT2_OK) {
            lemis_heap_free(bm);
            return EXT2_EIO;
        }
        for (i = 0; i < limit_b; i++) {
            if (!(bm[i >> 3] & (uint8_t)(1u << (i & 7u)))) total_b++;
        }
        if (gd->bg_inode_bitmap == 0 || gd->bg_inode_bitmap >= fs->blocks_count) {
            lemis_heap_free(bm);
            return EXT2_EIO;
        }
        if (ext2_dev_read_block(gd->bg_inode_bitmap, bm) != EXT2_OK) {
            lemis_heap_free(bm);
            return EXT2_EIO;
        }
        for (i = 0; i < limit_i; i++) {
            if (!(bm[i >> 3] & (uint8_t)(1u << (i & 7u)))) total_i++;
        }
    }
    lemis_heap_free(bm);
    if (free_blocks) *free_blocks = total_b;
    if (free_inodes) *free_inodes = total_i;
    return EXT2_OK;
}

int ext2_bitmaps_consistent(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t meas_b = 0, meas_i = 0;
    int ok = 1;

    if (!fs->mounted) return 0;
    if (ext2_count_free(&meas_b, &meas_i) != EXT2_OK) {
        console_puts("[ext2] bitmaps: cannot read the bitmaps\n");
        return 0;
    }
    if (meas_b != fs->sb.s_free_blocks_count) {
        console_printf("[ext2] bitmaps: free blocks measured=%u superblock=%u\n",
                       (uint64_t)meas_b, (uint64_t)fs->sb.s_free_blocks_count);
        ok = 0;
    }
    if (meas_i != fs->sb.s_free_inodes_count) {
        console_printf("[ext2] bitmaps: free inodes measured=%u superblock=%u\n",
                       (uint64_t)meas_i, (uint64_t)fs->sb.s_free_inodes_count);
        ok = 0;
    }
    {
        uint32_t g, sum_b = 0, sum_i = 0;
        for (g = 0; g < fs->group_count; g++) {
            sum_b += fs->bgdt[g].bg_free_blocks_count;
            sum_i += fs->bgdt[g].bg_free_inodes_count;
        }
        if (sum_b != fs->sb.s_free_blocks_count) {
            console_printf("[ext2] bitmaps: group free block sum=%u superblock=%u\n",
                           (uint64_t)sum_b, (uint64_t)fs->sb.s_free_blocks_count);
            ok = 0;
        }
        if (sum_i != fs->sb.s_free_inodes_count) {
            console_printf("[ext2] bitmaps: group free inode sum=%u superblock=%u\n",
                           (uint64_t)sum_i, (uint64_t)fs->sb.s_free_inodes_count);
            ok = 0;
        }
    }
    return ok;
}

int ext2_sync(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    if (!fs->mounted) return EXT2_EBUSY;
    ext2_recount();                        /* counters := bitmaps */
    if (ext2_write_bgdt() != EXT2_OK) {
        ext2_warn1("cannot flush the group descriptor table (volume", fs->group_count);
        return EXT2_EIO;
    }
    if (ext2_write_sb() != EXT2_OK) {
        console_puts("[ext2] cannot flush the superblock\n");
        return EXT2_EIO;
    }
    if (!fs->ram && fs->drive >= 0) {
        if (blk_flush(fs->drive) != 0) {
            ext2_warn1("device flush failed for drive", (uint64_t)fs->drive);
            return EXT2_EIO;
        }
    }
    return EXT2_OK;
}

/* =========================================================================
 * WRITE PATH - block mapping, data writes and truncate
 * ========================================================================= */

/* Map logical file block -> physical block, allocating the data block and any
 * indirection table that is still missing when `create` is set.
 *   *out_phys    = physical block, or 0 for a hole when create == 0
 *   *new_blocks += data + indirection blocks allocated by this call
 * Triple indirect is refused for writes: EXT2_ENOTSUP (read still handles it).
 * `tab` must hold one filesystem block and is clobbered. */
static int ext2_bmap_alloc(struct ext2_inode *in, uint64_t logical, int create,
                           uint32_t *out_phys, uint32_t *new_blocks, uint8_t *tab) {
    struct ext2_fs *fs = &g_ext2_fs;
    const uint64_t ptrs = fs->ptrs_per_block;
    uint64_t idx;
    uint32_t tbl, data;
    int r;

    if (!in || !out_phys || !tab) return EXT2_EINVAL;
    *out_phys = 0;

    if (logical < EXT2_DIRECT_BLOCKS) {
        if (in->i_block[logical] == 0 && create) {
            r = ext2_alloc_block_raw(&data);
            if (r != EXT2_OK) return r;
            in->i_block[logical] = data;
            if (new_blocks) (*new_blocks)++;
        }
        *out_phys = in->i_block[logical];
        return EXT2_OK;
    }

    if (logical < EXT2_DIRECT_BLOCKS + ptrs) {                    /* single indirect */
        if (in->i_block[EXT2_INDIRECT_BLOCK] == 0) {
            if (!create) return EXT2_OK;
            r = ext2_alloc_block_raw(&tbl);
            if (r != EXT2_OK) return r;
            in->i_block[EXT2_INDIRECT_BLOCK] = tbl;
            if (new_blocks) (*new_blocks)++;
        }
        tbl = in->i_block[EXT2_INDIRECT_BLOCK];
        if (ext2_dev_read_block(tbl, tab) != EXT2_OK) return EXT2_EIO;
        idx = logical - EXT2_DIRECT_BLOCKS;
        data = ext2_tbl_get(tab, idx);
        if (data == 0 && create) {
            r = ext2_alloc_block_raw(&data);
            if (r != EXT2_OK) return r;
            ext2_tbl_set(tab, idx, data);
            if (ext2_dev_write_block(tbl, tab) != EXT2_OK) return EXT2_EIO;
            if (new_blocks) (*new_blocks)++;
        }
        *out_phys = data;
        return EXT2_OK;
    }

    if (logical < EXT2_DIRECT_BLOCKS + ptrs + ptrs * ptrs) {      /* double indirect */
        uint64_t rem = logical - EXT2_DIRECT_BLOCKS - ptrs;
        uint64_t i1 = rem / ptrs;
        uint64_t i2 = rem % ptrs;
        uint32_t child;

        if (in->i_block[EXT2_DOUBLE_INDIRECT] == 0) {
            if (!create) return EXT2_OK;
            r = ext2_alloc_block_raw(&tbl);
            if (r != EXT2_OK) return r;
            in->i_block[EXT2_DOUBLE_INDIRECT] = tbl;
            if (new_blocks) (*new_blocks)++;
        }
        tbl = in->i_block[EXT2_DOUBLE_INDIRECT];
        if (ext2_dev_read_block(tbl, tab) != EXT2_OK) return EXT2_EIO;
        child = ext2_tbl_get(tab, i1);
        if (child == 0) {
            if (!create) return EXT2_OK;
            r = ext2_alloc_block_raw(&child);
            if (r != EXT2_OK) return r;
            ext2_tbl_set(tab, i1, child);
            if (ext2_dev_write_block(tbl, tab) != EXT2_OK) return EXT2_EIO;
            if (new_blocks) (*new_blocks)++;
        }
        if (ext2_dev_read_block(child, tab) != EXT2_OK) return EXT2_EIO;
        data = ext2_tbl_get(tab, i2);
        if (data == 0 && create) {
            r = ext2_alloc_block_raw(&data);
            if (r != EXT2_OK) return r;
            ext2_tbl_set(tab, i2, data);
            if (ext2_dev_write_block(child, tab) != EXT2_OK) return EXT2_EIO;
            if (new_blocks) (*new_blocks)++;
        }
        *out_phys = data;
        return EXT2_OK;
    }

    ext2_warn2("write needs the triple-indirect pointer (unsupported), logical block",
               logical, fs->max_logical);
    return EXT2_ENOTSUP;
}

/* Release every data block at or after logical block `keep` and every indirect
 * table that becomes empty. `in` is updated in memory; the caller writes it.
 * Returns the number of freed blocks in *freed. */
static int ext2_truncate_blocks(struct ext2_inode *in, uint32_t keep_blocks,
                                uint32_t *freed) {
    struct ext2_fs *fs = &g_ext2_fs;
    const uint64_t ptrs = fs->ptrs_per_block;
    const uint64_t single_end = EXT2_DIRECT_BLOCKS + ptrs;
    const uint64_t double_end = single_end + ptrs * ptrs;
    uint8_t *tab, *tab2;
    uint32_t i;
    int r = EXT2_OK;

    if (!in) return EXT2_EINVAL;
    if (freed) *freed = 0;
    if (in->i_block[EXT2_TRIPLE_INDIRECT] != 0) {
        ext2_warn1("refusing to shrink a file that uses the triple-indirect pointer",
                   in->i_block[EXT2_TRIPLE_INDIRECT]);
        return EXT2_ENOTSUP;
    }

    tab = (uint8_t *)lemis_heap_alloc(fs->block_size);
    tab2 = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!tab || !tab2) {
        if (tab) lemis_heap_free(tab);
        if (tab2) lemis_heap_free(tab2);
        return EXT2_ENOMEM;
    }

    /* ---- direct blocks ---- */
    for (i = keep_blocks; i < EXT2_DIRECT_BLOCKS; i++) {
        if (in->i_block[i] == 0) continue;
        r = ext2_free_block_raw(in->i_block[i]);
        if (r != EXT2_OK) goto out;
        in->i_block[i] = 0;
        if (freed) (*freed)++;
    }

    /* ---- single indirect ---- */
    if (in->i_block[EXT2_INDIRECT_BLOCK] != 0) {
        uint32_t tbl = in->i_block[EXT2_INDIRECT_BLOCK];
        int modified = 0, kept = 0;
        r = ext2_dev_read_block(tbl, tab);
        if (r != EXT2_OK) goto out;
        for (i = 0; i < ptrs; i++) {
            uint32_t d = ext2_tbl_get(tab, i);
            uint64_t logical = EXT2_DIRECT_BLOCKS + i;
            if (d == 0) continue;
            if (logical < (uint64_t)keep_blocks) { kept = 1; continue; }
            r = ext2_free_block_raw(d);
            if (r != EXT2_OK) goto out;
            ext2_tbl_set(tab, i, 0);
            modified = 1;
            if (freed) (*freed)++;
        }
        if (kept) {
            if (modified) {
                r = ext2_dev_write_block(tbl, tab);
                if (r != EXT2_OK) goto out;
            }
        } else {
            r = ext2_free_block_raw(tbl);
            if (r != EXT2_OK) goto out;
            in->i_block[EXT2_INDIRECT_BLOCK] = 0;
            if (freed) (*freed)++;
        }
    }

    /* ---- double indirect ---- */
    if (in->i_block[EXT2_DOUBLE_INDIRECT] != 0) {
        uint32_t outer = in->i_block[EXT2_DOUBLE_INDIRECT];
        int outer_modified = 0;
        r = ext2_dev_read_block(outer, tab);
        if (r != EXT2_OK) goto out;
        for (i = 0; i < ptrs; i++) {
            uint32_t child = ext2_tbl_get(tab, i);
            int child_modified = 0;
            uint64_t j;
            if (child == 0) continue;
            r = ext2_dev_read_block(child, tab2);
            if (r != EXT2_OK) goto out;
            for (j = 0; j < ptrs; j++) {
                uint32_t d = ext2_tbl_get(tab2, j);
                uint64_t logical = single_end + (uint64_t)i * ptrs + j;
                if (d == 0) continue;
                if (logical < (uint64_t)keep_blocks) continue;
                r = ext2_free_block_raw(d);
                if (r != EXT2_OK) goto out;
                ext2_tbl_set(tab2, j, 0);
                child_modified = 1;
                if (freed) (*freed)++;
            }
            if (child_modified) {
                /* the child table is empty when nothing at or after `keep`
                 * belongs to it and everything below keep was already absent */
                int has_kept = 0;
                for (j = 0; j < ptrs; j++) {
                    if (ext2_tbl_get(tab2, j) != 0) { has_kept = 1; break; }
                }
                if (has_kept) {
                    r = ext2_dev_write_block(child, tab2);
                    if (r != EXT2_OK) goto out;
                } else {
                    r = ext2_free_block_raw(child);
                    if (r != EXT2_OK) goto out;
                    ext2_tbl_set(tab, i, 0);
                    outer_modified = 1;
                    if (freed) (*freed)++;
                }
            }
        }
        if (outer_modified) {
            r = ext2_dev_write_block(outer, tab);
            if (r != EXT2_OK) goto out;
        }
        if (keep_blocks <= single_end) {
            r = ext2_free_block_raw(outer);
            if (r != EXT2_OK) goto out;
            in->i_block[EXT2_DOUBLE_INDIRECT] = 0;
            if (freed) (*freed)++;
        }
    }
    (void)double_end;

out:
    lemis_heap_free(tab);
    lemis_heap_free(tab2);
    return r;
}

/* Write `size` bytes at `offset` into inode `in`, growing the file as needed.
 * Zero-fills any newly allocated block region (no sparse writes). */
static int ext2_write_data(uint32_t ino, struct ext2_inode *in, const void *buf,
                           uint32_t offset, uint32_t size) {
    struct ext2_fs *fs = &g_ext2_fs;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bs = fs->block_size;
    uint32_t done = 0, new_blocks = 0;
    uint8_t *wbuf, *tab;
    int r = EXT2_OK;

    if (!in) return EXT2_EINVAL;
    if (!src && size != 0) return EXT2_EINVAL;
    if (size == 0) return 0;
    if ((uint64_t)offset + size > 0xFFFFFFFFu) return EXT2_EINVAL;

    wbuf = (uint8_t *)lemis_heap_alloc(bs);
    tab = (uint8_t *)lemis_heap_alloc(bs);
    if (!wbuf || !tab) {
        if (wbuf) lemis_heap_free(wbuf);
        if (tab) lemis_heap_free(tab);
        return EXT2_ENOMEM;
    }

    while (done < size) {
        uint64_t abs = (uint64_t)offset + done;
        uint64_t logical = abs / bs;
        uint32_t boff = (uint32_t)(abs % bs);
        uint32_t chunk = bs - boff;
        uint32_t phys = 0;

        if (chunk > size - done) chunk = size - done;

        r = ext2_bmap_alloc(in, logical, 1, &phys, &new_blocks, tab);
        if (r != EXT2_OK) break;
        if (phys == 0) { r = EXT2_EIO; break; }

        if (boff == 0 && chunk == bs) {
            memcpy(wbuf, src + done, bs);
        } else {
            /* partial block: keep the bytes outside the written range */
            r = ext2_dev_read_block(phys, wbuf);
            if (r != EXT2_OK) break;
            memcpy(wbuf + boff, src + done, chunk);
        }
        r = ext2_dev_write_block(phys, wbuf);
        if (r != EXT2_OK) break;
        done += chunk;
    }

    if (done > 0) {
        if ((uint64_t)offset + done > in->i_size) in->i_size = offset + done;
        in->i_blocks += new_blocks * (bs / EXT2_SECTOR_SIZE);
    }
    if (done > 0 || size == 0) {
        int wr = ext2_write_inode_raw(ino, in);
        if (r == EXT2_OK) r = wr;
    }
    lemis_heap_free(wbuf);
    lemis_heap_free(tab);
    if (r != EXT2_OK) return r;
    if (ext2_sync() != EXT2_OK) return EXT2_EIO;
    return (int)done;
}

int ext2_write_at(uint32_t ino, const void *buf, uint32_t offset, uint32_t size) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode in;
    int r;

    if (!fs->mounted) return EXT2_EBUSY;
    r = ext2_read_inode_raw(ino, &in);
    if (r != EXT2_OK) return r;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_EISDIR;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return EXT2_ENOTSUP;
    return ext2_write_data(ino, &in, buf, offset, size);
}

int ext2_file_write(struct ext2_file *f, const void *buf, uint32_t size, uint32_t offset) {
    struct ext2_inode in;
    int r;

    if (!f || !f->used) return EXT2_EINVAL;
    if (f->append) offset = f->size;
    r = ext2_read_inode_raw(f->ino, &in);
    if (r != EXT2_OK) return r;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return EXT2_ENOTSUP;
    r = ext2_write_data(f->ino, &in, buf, offset, size);
    if (r >= 0) f->size = in.i_size;
    return r;
}

int ext2_truncate(uint32_t ino, uint32_t new_size) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode in;
    uint32_t freed = 0, bs, keep_blocks;
    int r;

    if (!fs->mounted) return EXT2_EBUSY;
    r = ext2_read_inode_raw(ino, &in);
    if (r != EXT2_OK) return r;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_EISDIR;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return EXT2_ENOTSUP;
    bs = fs->block_size;

    if (new_size == in.i_size) return EXT2_OK;

    if (new_size < in.i_size) {
        keep_blocks = (new_size + bs - 1) / bs;
        r = ext2_truncate_blocks(&in, keep_blocks, &freed);
        if (r != EXT2_OK) return r;
        in.i_size = new_size;
        in.i_blocks -= freed * (bs / EXT2_SECTOR_SIZE);
        r = ext2_write_inode_raw(ino, &in);
        if (r != EXT2_OK) return r;
        if (ext2_sync() != EXT2_OK) return EXT2_EIO;
        return EXT2_OK;
    }

    /* grow: zero-fill up to the new length (ext2 has no file-system level
     * sparse-write API here; the read path still understands holes) */
    {
        uint8_t *zero = (uint8_t *)lemis_heap_alloc(bs);
        uint32_t pos = in.i_size;
        if (!zero) return EXT2_ENOMEM;
        memset(zero, 0, bs);
        while (pos < new_size) {
            uint32_t n = bs - (pos % bs);
            if (n > new_size - pos) n = new_size - pos;
            r = ext2_write_data(ino, &in, zero, pos, n);
            if (r < 0) break;
            pos += (uint32_t)r;
        }
        lemis_heap_free(zero);
        return (r < 0) ? r : EXT2_OK;
    }
}

int ext2_file_open(const char *path, int create, int trunc, int append,
                   struct ext2_file **out) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t ino = 0;
    struct ext2_inode in;
    struct ext2_file *f = NULL;
    int i, r;

    if (!fs->mounted) return EXT2_EBUSY;
    if (!out) return EXT2_EINVAL;

    r = ext2_resolve(path, &ino, &in);
    if (r == EXT2_ENOENT && create) {
        uint32_t made = 0;
        r = ext2_create(path, 0644, &made);
        if (r != EXT2_OK) return r;
        ino = made;
        r = ext2_read_inode_raw(ino, &in);
    }
    if (r != EXT2_OK) return r;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_EISDIR;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return EXT2_ENOTSUP;

    if (trunc) {
        r = ext2_truncate(ino, 0);
        if (r != EXT2_OK) return r;
        in.i_size = 0;
    }

    for (i = 0; i < EXT2_MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) { f = &g_files[i]; break; }
    }
    if (!f) return EXT2_EBUSY;
    f->used = 1;
    f->ino = ino;
    f->size = in.i_size;
    f->append = append ? 1 : 0;
    *out = f;
    return EXT2_OK;
}

/* =========================================================================
 * WRITE PATH - directory entries, path splitting, create/mkdir/rmdir/unlink/
 * rename
 * ========================================================================= */

/* Minimum rec_len for a directory entry with this name (4-byte aligned). */
static uint32_t ext2_dirent_need(uint32_t name_len) {
    return (EXT2_DIR_ENTRY_HDR + name_len + 3u) & ~3u;
}

/* The file-type byte is only interpreted when the volume advertises
 * EXT2_FEATURE_INCOMPAT_FILETYPE; otherwise it stays 0 (the high byte of a
 * 16-bit name length, which is always 0 because names stop at 255 bytes). */
static uint8_t ext2_dirent_ftype(uint8_t ftype) {
    return (g_ext2_fs.sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE)
               ? ftype : 0u;
}

/* Serialize one directory entry at `off` inside a directory block. */
static void ext2_dirent_put(uint8_t *blk, uint32_t off, uint32_t rec_len,
                            uint32_t ino, const char *name, uint32_t name_len,
                            uint8_t ftype) {
    memset(blk + off, 0, rec_len);
    ext2_wr32(blk + off, ino);
    ext2_wr16(blk + off + 4, (uint16_t)rec_len);
    blk[off + 6] = (uint8_t)name_len;
    blk[off + 7] = ext2_dirent_ftype(ftype);
    if (name && name_len) memcpy(blk + off + EXT2_DIR_ENTRY_HDR, name, name_len);
}

/* Insert (name -> child_ino) into directory `dir`. Reuses a free record, splits
 * the slack of the last record, or appends a fresh directory block. Updates
 * dir->i_size / dir->i_blocks in memory (the caller flushes the inode). */
static int ext2_dir_add_entry(struct ext2_inode *dir, const char *name,
                              uint32_t name_len, uint32_t child_ino, uint8_t ftype) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t bs = fs->block_size;
    uint32_t need, b, phys = 0;
    uint8_t *blk, *tab;
    int r = EXT2_ENOSPC;

    if (!dir || !name || name_len == 0 || name_len > EXT2_NAME_MAX) return EXT2_EINVAL;
    need = ext2_dirent_need(name_len);
    if (need > bs) return EXT2_EINVAL;

    blk = (uint8_t *)lemis_heap_alloc(bs);
    tab = (uint8_t *)lemis_heap_alloc(bs);
    if (!blk || !tab) {
        if (blk) lemis_heap_free(blk);
        if (tab) lemis_heap_free(tab);
        return EXT2_ENOMEM;
    }

    for (b = 0; (uint64_t)b * bs < dir->i_size; b++) {
        uint32_t pos = 0;
        phys = ext2_bmap(dir, b, tab);
        if (phys == 0) continue;
        if (ext2_dev_read_block(phys, blk) != EXT2_OK) { r = EXT2_EIO; goto out; }
        while (pos + EXT2_DIR_ENTRY_HDR <= bs) {
            uint32_t eino = ext2_rd32(blk + pos);
            uint32_t rec = ext2_rd16(blk + pos + 4);
            uint32_t nlen = blk[pos + 6];
            if (rec < EXT2_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > bs - pos || nlen > rec - EXT2_DIR_ENTRY_HDR) {
                break;                       /* corrupt block: stop scanning it */
            }
            if (eino == 0 && rec >= need) {
                ext2_dirent_put(blk, pos, rec, child_ino, name, name_len, ftype);
                r = ext2_dev_write_block(phys, blk);
                goto out;
            }
            if (eino != 0 && pos + rec == bs) {
                uint32_t min = ext2_dirent_need(nlen);
                if (rec >= min + need) {
                    ext2_wr16(blk + pos + 4, (uint16_t)min);
                    ext2_dirent_put(blk, pos + min, rec - min, child_ino, name,
                                    name_len, ftype);
                    r = ext2_dev_write_block(phys, blk);
                    goto out;
                }
            }
            pos += rec;
        }
    }

    /* no usable slack: extend the directory with a new block */
    {
        uint64_t logical = (uint64_t)dir->i_size / bs;
        uint32_t new_blocks = 0;
        r = ext2_bmap_alloc(dir, logical, 1, &phys, &new_blocks, tab);
        if (r != EXT2_OK) goto out;
        if (phys == 0) { r = EXT2_EIO; goto out; }
        ext2_dirent_put(blk, 0, bs, child_ino, name, name_len, ftype);
        r = ext2_dev_write_block(phys, blk);
        if (r != EXT2_OK) goto out;
        dir->i_size = (uint32_t)((logical + 1u) * bs);
        dir->i_blocks += new_blocks * (bs / EXT2_SECTOR_SIZE);
    }

out:
    lemis_heap_free(blk);
    lemis_heap_free(tab);
    return r;
}

/* Remove one entry from directory `dir` (merging its record into the previous
 * entry of the same block when there is one). */
static int ext2_dir_remove_entry(struct ext2_inode *dir, const char *name,
                                 uint32_t name_len, uint32_t *out_ino,
                                 uint8_t *out_ftype) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t bs = fs->block_size;
    uint32_t b;
    uint8_t *blk, *tab;
    int r = EXT2_ENOENT;

    if (!dir || !name || name_len == 0) return EXT2_EINVAL;
    blk = (uint8_t *)lemis_heap_alloc(bs);
    tab = (uint8_t *)lemis_heap_alloc(bs);
    if (!blk || !tab) {
        if (blk) lemis_heap_free(blk);
        if (tab) lemis_heap_free(tab);
        return EXT2_ENOMEM;
    }

    for (b = 0; (uint64_t)b * bs < dir->i_size; b++) {
        uint32_t phys = ext2_bmap(dir, b, tab);
        uint32_t pos = 0, prev = 0;
        int have_prev = 0;
        if (phys == 0) continue;
        if (ext2_dev_read_block(phys, blk) != EXT2_OK) { r = EXT2_EIO; goto out; }
        while (pos + EXT2_DIR_ENTRY_HDR <= bs) {
            uint32_t eino = ext2_rd32(blk + pos);
            uint32_t rec = ext2_rd16(blk + pos + 4);
            uint32_t nlen = blk[pos + 6];
            if (rec < EXT2_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > bs - pos || nlen > rec - EXT2_DIR_ENTRY_HDR) {
                break;
            }
            if (eino != 0 && nlen == name_len &&
                memcmp(blk + pos + EXT2_DIR_ENTRY_HDR, name, name_len) == 0) {
                if (out_ino) *out_ino = eino;
                if (out_ftype) *out_ftype = blk[pos + 7];
                if (have_prev) {
                    ext2_wr16(blk + prev + 4, (uint16_t)(ext2_rd16(blk + prev + 4) + rec));
                } else {
                    ext2_wr32(blk + pos, 0);     /* first entry: keep rec_len */
                }
                r = ext2_dev_write_block(phys, blk);
                goto out;
            }
            prev = pos;
            have_prev = 1;
            pos += rec;
        }
    }

out:
    lemis_heap_free(blk);
    lemis_heap_free(tab);
    return r;
}

/* Point the ".." entry of `dir` at `new_parent` (used by directory renames). */
static int ext2_dir_set_dotdot(struct ext2_inode *dir, uint32_t new_parent) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t bs = fs->block_size;
    uint32_t b;
    uint8_t *blk, *tab;
    int r = EXT2_ENOENT;

    blk = (uint8_t *)lemis_heap_alloc(bs);
    tab = (uint8_t *)lemis_heap_alloc(bs);
    if (!blk || !tab) {
        if (blk) lemis_heap_free(blk);
        if (tab) lemis_heap_free(tab);
        return EXT2_ENOMEM;
    }
    for (b = 0; (uint64_t)b * bs < dir->i_size; b++) {
        uint32_t phys = ext2_bmap(dir, b, tab);
        uint32_t pos = 0;
        if (phys == 0) continue;
        if (ext2_dev_read_block(phys, blk) != EXT2_OK) { r = EXT2_EIO; goto out; }
        while (pos + EXT2_DIR_ENTRY_HDR <= bs) {
            uint32_t rec = ext2_rd16(blk + pos + 4);
            uint32_t nlen = blk[pos + 6];
            if (rec < EXT2_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > bs - pos || nlen > rec - EXT2_DIR_ENTRY_HDR) {
                break;
            }
            if (nlen == 2 && blk[pos + EXT2_DIR_ENTRY_HDR] == '.' &&
                blk[pos + EXT2_DIR_ENTRY_HDR + 1] == '.') {
                ext2_wr32(blk + pos, new_parent);
                r = ext2_dev_write_block(phys, blk);
                goto out;
            }
            pos += rec;
        }
    }
out:
    lemis_heap_free(blk);
    lemis_heap_free(tab);
    return r;
}

/* 1 = empty (only "." and ".."), 0 = holds other entries, < 0 = error. */
static int ext2_dir_is_empty(const struct ext2_inode *dir) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t bs = fs->block_size;
    uint32_t b;
    uint8_t *blk, *tab;
    int r = 1;

    blk = (uint8_t *)lemis_heap_alloc(bs);
    tab = (uint8_t *)lemis_heap_alloc(bs);
    if (!blk || !tab) {
        if (blk) lemis_heap_free(blk);
        if (tab) lemis_heap_free(tab);
        return EXT2_ENOMEM;
    }
    for (b = 0; (uint64_t)b * bs < dir->i_size && r == 1; b++) {
        uint32_t phys = ext2_bmap(dir, b, tab);
        uint32_t pos = 0;
        if (phys == 0) continue;
        if (ext2_dev_read_block(phys, blk) != EXT2_OK) { r = EXT2_EIO; break; }
        while (pos + EXT2_DIR_ENTRY_HDR <= bs) {
            uint32_t eino = ext2_rd32(blk + pos);
            uint32_t rec = ext2_rd16(blk + pos + 4);
            uint32_t nlen = blk[pos + 6];
            const uint8_t *nm = blk + pos + EXT2_DIR_ENTRY_HDR;
            if (rec < EXT2_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > bs - pos || nlen > rec - EXT2_DIR_ENTRY_HDR) {
                break;
            }
            if (eino != 0) {
                if (nlen == 1 && nm[0] == '.') { /* "." */ }
                else if (nlen == 2 && nm[0] == '.' && nm[1] == '.') { /* ".." */ }
                else { r = 0; break; }
            }
            pos += rec;
        }
    }
    lemis_heap_free(blk);
    lemis_heap_free(tab);
    return r;
}

/* Split a volume-relative path into (parent directory, final component).
 * Trailing slashes are dropped; "." and ".." as the final component are
 * refused (they can never name a new entry). */
#define EXT2_PATH_MAX 320

static int ext2_split_path(const char *path, uint32_t *out_parent_ino,
                           struct ext2_inode *out_parent, char *leaf,
                           uint32_t leaf_cap, uint32_t *out_leaf_len) {
    char parent[EXT2_PATH_MAX];
    size_t n, cut;
    uint32_t len;
    int r;

    if (!path || !leaf || leaf_cap == 0) return EXT2_EINVAL;
    n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (n == 1 && path[0] == '/') return EXT2_EINVAL;      /* the root has no name */
    if (n == 0) return EXT2_EINVAL;

    cut = n;
    while (cut > 0 && path[cut - 1] != '/') cut--;
    len = (uint32_t)(n - cut);
    if (len == 0 || len > EXT2_NAME_MAX) return EXT2_EINVAL;
    if (leaf_cap <= len) return EXT2_EINVAL;
    memcpy(leaf, path + cut, len);
    leaf[len] = '\0';
    if ((len == 1 && leaf[0] == '.') ||
        (len == 2 && leaf[0] == '.' && leaf[1] == '.')) {
        return EXT2_EINVAL;
    }

    if (cut == 0) {
        strcpy(parent, "/");
    } else {
        if (cut >= sizeof(parent)) return EXT2_EINVAL;
        memcpy(parent, path, cut);
        parent[cut] = '\0';
    }
    r = ext2_resolve(parent, out_parent_ino, out_parent);
    if (r != EXT2_OK) return r;
    if ((out_parent->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return EXT2_ENOTDIR;
    if (out_leaf_len) *out_leaf_len = len;
    return EXT2_OK;
}

/* Shared body of ext2_create() / ext2_mkdir(). */
static int ext2_create_or_mkdir(const char *path, uint32_t mode, int is_dir,
                                uint32_t *out_ino) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode parent, in;
    char leaf[EXT2_NAME_MAX + 1];
    uint32_t parent_ino = 0, leaf_len = 0, ino = 0, bs = fs->block_size;
    uint32_t dir_block = 0;
    int r;

    if (!fs->mounted) return EXT2_EBUSY;
    r = ext2_split_path(path, &parent_ino, &parent, leaf, sizeof(leaf), &leaf_len);
    if (r != EXT2_OK) return r;
    if (ext2_dir_find(&parent, leaf, leaf_len, NULL, NULL) == EXT2_OK) return EXT2_EEXIST;

    r = ext2_alloc_inode_raw(&ino);
    if (r != EXT2_OK) return r;

    memset(&in, 0, sizeof(in));
    if (is_dir) {
        r = ext2_alloc_block_raw(&dir_block);
        if (r != EXT2_OK) { ext2_free_inode_raw(ino); return r; }
        in.i_mode = (uint16_t)(EXT2_S_IFDIR | (mode & 0x0FFFu));
        in.i_links_count = 2;
        in.i_size = bs;
        in.i_blocks = bs / EXT2_SECTOR_SIZE;
        in.i_block[0] = dir_block;
    } else {
        in.i_mode = (uint16_t)(EXT2_S_IFREG | (mode & 0x0FFFu));
        in.i_links_count = 1;
    }

    if (is_dir) {
        /* build the new directory block: "." (12 bytes) then ".." (the rest) */
        uint8_t *dblk = (uint8_t *)lemis_heap_alloc(bs);
        if (!dblk) {
            ext2_free_block_raw(dir_block);
            ext2_free_inode_raw(ino);
            return EXT2_ENOMEM;
        }
        ext2_dirent_put(dblk, 0, ext2_dirent_need(1), ino, ".", 1, EXT2_FT_DIR);
        ext2_dirent_put(dblk, ext2_dirent_need(1), bs - ext2_dirent_need(1),
                        parent_ino, "..", 2, EXT2_FT_DIR);
        r = ext2_dev_write_block(dir_block, dblk);
        lemis_heap_free(dblk);
        if (r != EXT2_OK) {
            ext2_free_block_raw(dir_block);
            ext2_free_inode_raw(ino);
            return r;
        }
    }

    r = ext2_write_inode_raw(ino, &in);
    if (r != EXT2_OK) { ext2_free_inode_raw(ino); return r; }

    r = ext2_dir_add_entry(&parent, leaf, leaf_len, ino,
                           is_dir ? EXT2_FT_DIR : EXT2_FT_REG);
    if (r != EXT2_OK) { ext2_free_inode_raw(ino); return r; }

    if (is_dir) {
        uint32_t pgroup = (parent_ino - 1u) / fs->inodes_per_group;
        parent.i_links_count++;
        if (pgroup < fs->group_count) fs->bgdt[pgroup].bg_used_dirs_count++;
    }
    r = ext2_write_inode_raw(parent_ino, &parent);
    if (r != EXT2_OK) return r;
    r = ext2_sync();
    if (r != EXT2_OK) return r;
    if (out_ino) *out_ino = ino;
    return EXT2_OK;
}

int ext2_create(const char *path, uint32_t mode, uint32_t *out_ino) {
    return ext2_create_or_mkdir(path, mode, 0, out_ino);
}

int ext2_mkdir(const char *path, uint32_t mode) {
    return ext2_create_or_mkdir(path, mode, 1, NULL);
}

/* Release every block of an inode (used by unlink/rmdir/target replacement). */
static int ext2_release_inode_blocks(struct ext2_inode *in) {
    uint32_t freed = 0;
    int r = ext2_truncate_blocks(in, 0, &freed);
    if (r != EXT2_OK) return r;
    in->i_size = 0;
    if (in->i_blocks >= freed * (g_ext2_fs.block_size / EXT2_SECTOR_SIZE)) {
        in->i_blocks -= freed * (g_ext2_fs.block_size / EXT2_SECTOR_SIZE);
    } else {
        in->i_blocks = 0;
    }
    return EXT2_OK;
}

/* Remove the directory entry `name` from `parent` and drop one link from the
 * child inode. With `remove_inode` set (rename over an existing directory) the
 * inode and its blocks are always released, otherwise the inode survives while
 * other hard links still reference it. */
static int ext2_remove_entry_and_unlink(uint32_t parent_ino, struct ext2_inode *parent,
                                        const char *name, uint32_t name_len,
                                        uint32_t child_ino, struct ext2_inode *child,
                                        int remove_inode) {
    struct ext2_fs *fs = &g_ext2_fs;
    int r;

    (void)parent_ino;
    r = ext2_dir_remove_entry(parent, name, name_len, NULL, NULL);
    if (r != EXT2_OK) return r;

    if (!remove_inode && child->i_links_count > 1) {
        child->i_links_count--;
        r = ext2_write_inode_raw(child_ino, child);
        return r;
    }

    /* last link: free the data blocks, then the inode itself */
    r = ext2_release_inode_blocks(child);
    if (r != EXT2_OK) return r;
    child->i_links_count = 0;
    r = ext2_write_inode_raw(child_ino, child);
    if (r != EXT2_OK) return r;
    r = ext2_free_inode_raw(child_ino);
    if (r != EXT2_OK) return r;
    if ((child->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        uint32_t group = (child_ino - 1u) / fs->inodes_per_group;
        if (group < fs->group_count && fs->bgdt[group].bg_used_dirs_count > 0) {
            fs->bgdt[group].bg_used_dirs_count--;
        }
    }
    return EXT2_OK;
}

int ext2_unlink(const char *path) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode parent, child;
    char leaf[EXT2_NAME_MAX + 1];
    uint32_t parent_ino = 0, leaf_len = 0, child_ino = 0;
    int r;

    if (!fs->mounted) return EXT2_EBUSY;
    r = ext2_split_path(path, &parent_ino, &parent, leaf, sizeof(leaf), &leaf_len);
    if (r != EXT2_OK) return r;
    r = ext2_dir_find(&parent, leaf, leaf_len, &child_ino, NULL);
    if (r != EXT2_OK) return (r == EXT2_ENOENT) ? EXT2_ENOENT : r;
    r = ext2_read_inode_raw(child_ino, &child);
    if (r != EXT2_OK) return r;
    if ((child.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_EISDIR;

    r = ext2_remove_entry_and_unlink(parent_ino, &parent, leaf, leaf_len, child_ino, &child,
                                     0);
    if (r != EXT2_OK) return r;
    return ext2_sync();
}

int ext2_rmdir(const char *path) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode parent, child;
    char leaf[EXT2_NAME_MAX + 1];
    uint32_t parent_ino = 0, leaf_len = 0, child_ino = 0, group;
    int r, empty;

    if (!fs->mounted) return EXT2_EBUSY;
    r = ext2_split_path(path, &parent_ino, &parent, leaf, sizeof(leaf), &leaf_len);
    if (r != EXT2_OK) return r;
    r = ext2_dir_find(&parent, leaf, leaf_len, &child_ino, NULL);
    if (r != EXT2_OK) return (r == EXT2_ENOENT) ? EXT2_ENOENT : r;
    if (child_ino == 2) return EXT2_EBUSY;                  /* never remove '/' */
    r = ext2_read_inode_raw(child_ino, &child);
    if (r != EXT2_OK) return r;
    if ((child.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return EXT2_ENOTDIR;

    empty = ext2_dir_is_empty(&child);
    if (empty < 0) return empty;
    if (!empty) return EXT2_ENOTEMPTY;

    /* drop the entry first, then release the child (blocks + inode) */
    r = ext2_dir_remove_entry(&parent, leaf, leaf_len, NULL, NULL);
    if (r != EXT2_OK) return r;
    r = ext2_release_inode_blocks(&child);
    if (r != EXT2_OK) return r;
    child.i_links_count = 0;
    r = ext2_write_inode_raw(child_ino, &child);
    if (r != EXT2_OK) return r;
    r = ext2_free_inode_raw(child_ino);
    if (r != EXT2_OK) return r;
    group = (child_ino - 1u) / fs->inodes_per_group;
    if (group < fs->group_count && fs->bgdt[group].bg_used_dirs_count > 0) {
        fs->bgdt[group].bg_used_dirs_count--;
    }

    if (parent.i_links_count > 0) parent.i_links_count--;
    r = ext2_write_inode_raw(parent_ino, &parent);
    if (r != EXT2_OK) return r;
    return ext2_sync();
}

/* Is `ancestor` the directory `dir_ino` or one of its parents? 1 = yes,
 * 0 = no, < 0 = cannot tell (corrupt ".." chain). */
static int ext2_is_ancestor(uint32_t ancestor, uint32_t dir_ino) {
    uint32_t cur = dir_ino, depth = 0;

    while (depth++ < 256u) {
        struct ext2_inode in;
        uint32_t up = 0;
        if (cur == ancestor) return 1;
        if (cur == 2) return 0;                       /* reached the root */
        if (ext2_read_inode_raw(cur, &in) != EXT2_OK) return -1;
        if (ext2_dir_find(&in, "..", 2, &up, NULL) != EXT2_OK) return -1;
        if (up == 0 || up == cur) return 0;
        cur = up;
    }
    return -1;
}

int ext2_rename(const char *oldpath, const char *newpath) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode old_parent, new_parent_in, src_in, dst_in;
    struct ext2_inode *pn;
    char leaf_old[EXT2_NAME_MAX + 1], leaf_new[EXT2_NAME_MAX + 1];
    uint32_t old_parent_ino = 0, new_parent_ino = 0, lo = 0, ln = 0;
    uint32_t src_ino = 0, dst_ino = 0, src_group;
    int r, src_is_dir, same_parent;

    if (!fs->mounted) return EXT2_EBUSY;
    if (!oldpath || !newpath) return EXT2_EINVAL;
    if (strcmp(oldpath, newpath) == 0) return EXT2_OK;

    r = ext2_split_path(oldpath, &old_parent_ino, &old_parent, leaf_old,
                        sizeof(leaf_old), &lo);
    if (r != EXT2_OK) return r;
    r = ext2_dir_find(&old_parent, leaf_old, lo, &src_ino, NULL);
    if (r != EXT2_OK) return (r == EXT2_ENOENT) ? EXT2_ENOENT : r;
    r = ext2_read_inode_raw(src_ino, &src_in);
    if (r != EXT2_OK) return r;
    src_is_dir = ((src_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;

    r = ext2_split_path(newpath, &new_parent_ino, &new_parent_in, leaf_new,
                        sizeof(leaf_new), &ln);
    if (r != EXT2_OK) return r;

    same_parent = (new_parent_ino == old_parent_ino) ? 1 : 0;
    pn = same_parent ? &old_parent : &new_parent_in;

    /* moving a directory inside itself (or below itself) is invalid */
    if (src_is_dir) {
        int anc = ext2_is_ancestor(src_ino, new_parent_ino);
        if (anc < 0) return EXT2_ERR;
        if (anc) return EXT2_EINVAL;
    }

    /* same parent and same name: nothing to do */
    if (same_parent && lo == ln && memcmp(leaf_old, leaf_new, lo) == 0) return EXT2_OK;

    /* existing destination? POSIX says: replace it. */
    r = ext2_dir_find(pn, leaf_new, ln, &dst_ino, NULL);
    if (r == EXT2_OK) {
        if (dst_ino == src_ino) return EXT2_OK;
        r = ext2_read_inode_raw(dst_ino, &dst_in);
        if (r != EXT2_OK) return r;
        if (src_is_dir && (dst_in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
            return EXT2_ENOTDIR;
        }
        if (!src_is_dir && (dst_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            return EXT2_EISDIR;
        }
        if ((dst_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            int empty = ext2_dir_is_empty(&dst_in);
            if (empty < 0) return empty;
            if (!empty) return EXT2_ENOTEMPTY;
        }
        r = ext2_remove_entry_and_unlink(new_parent_ino, pn, leaf_new, ln, dst_ino,
                                         &dst_in,
                                         ((dst_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR));
        if (r != EXT2_OK) return r;
        if ((dst_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR && pn->i_links_count > 0) {
            pn->i_links_count--;             /* the removed dir's ".." is gone */
        }
    } else if (r != EXT2_ENOENT) {
        return r;
    }

    /* remove the old name, add the new one */
    r = ext2_dir_remove_entry(&old_parent, leaf_old, lo, NULL, NULL);
    if (r != EXT2_OK) return r;
    r = ext2_dir_add_entry(pn, leaf_new, ln, src_ino,
                           src_is_dir ? EXT2_FT_DIR : EXT2_FT_REG);
    if (r != EXT2_OK) return r;

    if (src_is_dir) {
        if (old_parent.i_links_count > 0) old_parent.i_links_count--;
        pn->i_links_count++;
        if (same_parent) {
            /* op == np: the -- and ++ cancel out, keep it explicit */
            old_parent.i_links_count = pn->i_links_count;
        }
        r = ext2_dir_set_dotdot(&src_in, new_parent_ino);
        if (r != EXT2_OK) return r;
    }

    if (!same_parent) {
        r = ext2_write_inode_raw(old_parent_ino, &old_parent);
        if (r != EXT2_OK) return r;
    }
    r = ext2_write_inode_raw(new_parent_ino, pn);
    if (r != EXT2_OK) return r;

    src_group = (src_ino - 1u) / fs->inodes_per_group;
    if (src_is_dir && src_group < fs->group_count) {
        /* used_dirs_count follows the inode, so moving it changes nothing */
    }
    return ext2_sync();
}

/* ==================== guest selftest ==================== */

/* Content rules - MUST match scripts/mkfs_ext2_testimg.py (see the file header
 * comment for the full table). */
static const char EXT2_ST_HELLO[] =
    "Hello from SutureOS ext2!\nSecond line: read via the real ext2 driver.\n";
static const char EXT2_ST_NESTED[] =
    "Nested file: SUB/NESTED.TXT reached through ext2 directory traversal.\n";
#define EXT2_ST_HELLO_SIZE   ((uint32_t)sizeof(EXT2_ST_HELLO) - 1u)   /* 70 */
#define EXT2_ST_NESTED_SIZE  ((uint32_t)sizeof(EXT2_ST_NESTED) - 1u)  /* 70 */
#define EXT2_ST_BIG_SIZE     (64u * 1024u)      /* byte i = (i*31 + 7) & 0xFF */
#define EXT2_ST_DOUBLE_SIZE  (300u * 1024u)     /* byte i = (i*29 + 11) & 0xFF */
#define EXT2_ST_SPARSE_BLOCKS 3u                /* logical block 1 is a hole */

enum ext2_st_kind {
    EXT2_ST_TEXT = 0,
    EXT2_ST_BIG,
    EXT2_ST_DOUBLE,
    EXT2_ST_SPARSE,
};

static uint8_t ext2_st_expected(int kind, uint32_t off, const char *text) {
    switch (kind) {
        case EXT2_ST_TEXT:   return (uint8_t)text[off];
        case EXT2_ST_BIG:    return (uint8_t)((off * 31u + 7u) & 0xFFu);
        case EXT2_ST_DOUBLE: return (uint8_t)((off * 29u + 11u) & 0xFFu);
        case EXT2_ST_SPARSE:
            if (off / g_ext2_fs.block_size == 1u) return 0;   /* the hole */
            return (uint8_t)((off * 17u + 3u) & 0xFFu);
        default:             return 0;
    }
}

static int ext2_selftest_fail(const char *reason) {
    console_printf("[ext2] selftest FAIL (%s)\n", reason);
    return EXT2_ERR;
}

static int ext2_selftest_file(const char *path, int kind, uint32_t expect_size,
                              const char *text, uint8_t *scratch, uint32_t *out_crc) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_stat st;
    uint32_t offset = 0;
    uint32_t crc = 0xFFFFFFFFu;
    int r;

    r = ext2_stat(path, &st);
    if (r != EXT2_OK) return ext2_selftest_fail("cannot stat a test file");
    if (!st.is_file) return ext2_selftest_fail("a test file is not a regular file");
    if (st.size != expect_size) {
        console_printf("[ext2] selftest detail: %s size=%u expected=%u\n", path,
                       (uint64_t)st.size, (uint64_t)expect_size);
        return ext2_selftest_fail("unexpected test file size");
    }

    while (offset < expect_size) {
        uint32_t n = fs->block_size;
        uint32_t i;
        int got;
        if (n > expect_size - offset) n = expect_size - offset;
        got = ext2_read_file(st.ino, scratch, offset, n);
        if (got != (int)n) return ext2_selftest_fail("short read from a test file");
        for (i = 0; i < n; i++) {
            if (scratch[i] != ext2_st_expected(kind, offset + i, text)) {
                console_printf("[ext2] selftest detail: %s mismatch at offset %u\n", path,
                               (uint64_t)(offset + i));
                return ext2_selftest_fail("test file content mismatch");
            }
        }
        crc = ext2_crc_update(crc, scratch, n);
        offset += n;
    }
    if (out_crc) *out_crc = crc ^ 0xFFFFFFFFu;
    return EXT2_OK;
}

static const char *const EXT2_ST_ROOT_NAMES[] = {
    "HELLO.TXT", "BIG.BIN", "SPARSE.BIN", "DOUBLE.BIN", "SUB"
};
#define EXT2_ST_ROOT_NAME_COUNT 5

static int ext2_selftest_readdir(const char *path, const char *const *want, uint32_t want_n,
                                 int *seen) {
    struct ext2_dir *d = NULL;
    char name[128];
    int r;

    r = ext2_opendir(path, &d);
    if (r != EXT2_OK) return ext2_selftest_fail("cannot open a test directory");

    console_printf("[ext2] selftest readdir %s =", path);
    for (;;) {
        uint32_t len = 0;
        int is_dir = 0;
        r = ext2_readdir(d, name, sizeof(name), &len, &is_dir);
        if (r == EXT2_ENOENT) break;
        if (r != EXT2_OK) {
            ext2_closedir(d);
            return ext2_selftest_fail("readdir failed");
        }
        console_printf(" %s%s", name, is_dir ? "/" : "");
        if (want) {
            uint32_t i;
            for (i = 0; i < want_n; i++) {
                if (strcmp(name, want[i]) == 0) seen[i] = 1;
            }
        }
    }
    console_puts("\n");
    ext2_closedir(d);
    return EXT2_OK;
}

int ext2_selftest(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint8_t *scratch;
    uint32_t crc_hello = 0, crc_big = 0, crc_nested = 0, crc_sparse = 0, crc_double = 0;
    uint32_t sparse_size, i;
    int seen[EXT2_ST_ROOT_NAME_COUNT] = {0, 0, 0, 0, 0};
    struct ext2_inode sparse_in;
    struct ext2_stat st;
    int r;

    if (!fs->mounted) {
        if (!g_ram_image) {
            return ext2_selftest_fail("no ramdisk registered and no ext2 volume mounted");
        }
        if (ext2_mount_ramdisk() != EXT2_OK) {
            return ext2_selftest_fail("mounting the registered ramdisk failed");
        }
    }

    scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scratch) return ext2_selftest_fail("out of memory");

    r = ext2_selftest_file("/HELLO.TXT", EXT2_ST_TEXT, EXT2_ST_HELLO_SIZE, EXT2_ST_HELLO,
                           scratch, &crc_hello);
    if (r != EXT2_OK) goto fail;
    r = ext2_selftest_file("/SUB/NESTED.TXT", EXT2_ST_TEXT, EXT2_ST_NESTED_SIZE,
                           EXT2_ST_NESTED, scratch, &crc_nested);
    if (r != EXT2_OK) goto fail;
    r = ext2_selftest_file("/BIG.BIN", EXT2_ST_BIG, EXT2_ST_BIG_SIZE, NULL, scratch, &crc_big);
    if (r != EXT2_OK) goto fail;
    sparse_size = EXT2_ST_SPARSE_BLOCKS * fs->block_size;
    r = ext2_selftest_file("/SPARSE.BIN", EXT2_ST_SPARSE, sparse_size, NULL, scratch,
                           &crc_sparse);
    if (r != EXT2_OK) goto fail;
    r = ext2_selftest_file("/DOUBLE.BIN", EXT2_ST_DOUBLE, EXT2_ST_DOUBLE_SIZE, NULL, scratch,
                           &crc_double);
    if (r != EXT2_OK) goto fail;

    /* the sparse file's middle logical block must be an unmapped hole */
    {
        uint32_t ino = 0;
        if (ext2_lookup("/SPARSE.BIN", &ino) != EXT2_OK) {
            ext2_selftest_fail("cannot look up SPARSE.BIN");
            goto fail;
        }
        if (ext2_read_inode(ino, &sparse_in) != EXT2_OK) {
            ext2_selftest_fail("cannot read the SPARSE.BIN inode");
            goto fail;
        }
        if (sparse_in.i_block[1] != 0) {
            ext2_selftest_fail("SPARSE.BIN logical block 1 is not a hole");
            goto fail;
        }
    }

    /* nested lookup + "." / ".." resolution through real directory entries */
    {
        uint32_t a = 0, b = 0;
        if (ext2_lookup("/HELLO.TXT", &a) != EXT2_OK ||
            ext2_lookup("/SUB/../HELLO.TXT", &b) != EXT2_OK || a != b) {
            ext2_selftest_fail("'..' path resolution failed");
            goto fail;
        }
        if (ext2_stat("/SUB", &st) != EXT2_OK || !st.is_dir) {
            ext2_selftest_fail("/SUB is not a directory");
            goto fail;
        }
        if (ext2_stat("/", &st) != EXT2_OK || !st.is_dir) {
            ext2_selftest_fail("'/' is not a directory");
            goto fail;
        }
    }

    if (ext2_selftest_readdir("/", EXT2_ST_ROOT_NAMES, EXT2_ST_ROOT_NAME_COUNT, seen) != EXT2_OK) {
        goto fail;
    }
    for (i = 0; i < EXT2_ST_ROOT_NAME_COUNT; i++) {
        if (!seen[i]) {
            ext2_selftest_fail("the root directory listing is incomplete");
            goto fail;
        }
    }
    if (ext2_selftest_readdir("/SUB", NULL, 0, NULL) != EXT2_OK) goto fail;

    console_printf("[ext2] selftest sizes hello=%u nested=%u big=%u sparse=%u double=%u\n",
                   (uint64_t)EXT2_ST_HELLO_SIZE, (uint64_t)EXT2_ST_NESTED_SIZE,
                   (uint64_t)EXT2_ST_BIG_SIZE, (uint64_t)sparse_size,
                   (uint64_t)EXT2_ST_DOUBLE_SIZE);
    console_printf("[ext2] selftest PASS (hello=0x%08x big=0x%08x nested=0x%08x "
                   "sparse=0x%08x double=0x%08x)\n",
                   (uint64_t)crc_hello, (uint64_t)crc_big, (uint64_t)crc_nested,
                   (uint64_t)crc_sparse, (uint64_t)crc_double);
    lemis_heap_free(scratch);
    return EXT2_OK;

fail:
    lemis_heap_free(scratch);
    return EXT2_ERR;
}

/* ==================== write self-test ==================== */

static int ext2_wst_fail(const char *reason) {
    console_printf("[ext2] write self-test FAIL (%s)\n", reason);
    return EXT2_ERR;
}

/* The write self-test is destructive, so it only runs when the volume really is
 * the generated test image: /HELLO.TXT with exactly the documented 70 bytes. A
 * real user volume therefore never gets touched by the boot self-test. */
static int ext2_is_test_image(void) {
    struct ext2_stat st;
    uint8_t *buf;
    int r, ok = 0;

    if (ext2_stat("/HELLO.TXT", &st) != EXT2_OK) return 0;
    if (!st.is_file || st.size != EXT2_ST_HELLO_SIZE) return 0;
    buf = (uint8_t *)lemis_heap_alloc(g_ext2_fs.block_size);
    if (!buf) return 0;
    r = ext2_read_file(st.ino, buf, 0, EXT2_ST_HELLO_SIZE);
    if (r == (int)EXT2_ST_HELLO_SIZE &&
        memcmp(buf, EXT2_ST_HELLO, EXT2_ST_HELLO_SIZE) == 0) {
        ok = 1;
    }
    lemis_heap_free(buf);
    return ok;
}

/* Write `blocks` blocks of the given pattern starting at offset 0. */
static int ext2_wst_write_pattern(uint32_t ino, uint32_t blocks, uint8_t mul,
                                  uint8_t add, uint8_t *buf) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t bs = fs->block_size, i, k;
    int r = EXT2_OK;

    for (i = 0; i < blocks; i++) {
        for (k = 0; k < bs; k++) {
            buf[k] = (uint8_t)((((uint32_t)i * bs + k) * mul + add) & 0xFFu);
        }
        r = ext2_write_at(ino, buf, i * bs, bs);
        if (r != (int)bs) return (r < 0) ? r : EXT2_EIO;
    }
    return EXT2_OK;
}

/* Byte-for-byte check of [from_block*bs, bytes) against the same pattern. */
static int ext2_wst_check_pattern(uint32_t ino, uint32_t from_block, uint32_t bytes,
                                  uint8_t mul, uint8_t add, uint8_t *buf) {
    struct ext2_fs *fs = &g_ext2_fs;
    uint32_t bs = fs->block_size, i, k;
    int r;

    for (i = from_block; (uint64_t)i * bs < bytes; i++) {
        uint32_t off = i * bs;
        uint32_t n = bytes - off;
        if (n > bs) n = bs;
        r = ext2_read_file(ino, buf, off, n);
        if (r != (int)n) return EXT2_EIO;
        for (k = 0; k < n; k++) {
            if (buf[k] != (uint8_t)((((uint32_t)off + k) * mul + add) & 0xFFu)) {
                console_printf("[ext2] write self-test detail: byte %u mismatch\n",
                               (uint64_t)((uint32_t)off + k));
                return EXT2_ERR;
            }
        }
    }
    return EXT2_OK;
}

int ext2_write_selftest(void) {
    struct ext2_fs *fs = &g_ext2_fs;
    struct ext2_inode in;
    struct ext2_stat st;
    uint32_t bs, blocks, total, i, k, ino = 0, keep_ino = 0, artifact_blocks;
    uint8_t *buf = NULL;
    int create_ok = 0, write_ok = 0, readback_ok = 0, indirect_ok = 0;
    int rename_ok = 0, unlink_ok = 0, rmdir_ok = 0, truncate_ok = 0;
    int nonempty_ok = 0, overwrite_ok = 0, triple_ok = 0, triple_tested = 0;
    int consistent;
    int r;

    if (!fs->mounted) {
        console_puts("[ext2] write self-test skipped (no ext2 volume mounted)\n");
        return EXT2_EBUSY;
    }
    if (!ext2_is_test_image()) {
        console_puts("[ext2] write self-test skipped (not the generated test image)\n");
        return EXT2_OK;
    }

    bs = fs->block_size;
    blocks = EXT2_WRITE_BLOCKS;         /* 13 blocks: direct -> single indirect */
    total = blocks * bs;

    /* Clean up leftovers from an interrupted earlier run (the self-test must be
     * repeatable on the same image). Failures are expected and ignored. */
    if (ext2_stat("/WRTDIR", &st) == EXT2_OK) {
        ext2_unlink("/WRTDIR/F13K.BIN");
        ext2_unlink("/WRTDIR/R13K.BIN");
        ext2_rmdir("/WRTDIR");
    }
    ext2_unlink("/M13K.BIN");

    buf = (uint8_t *)lemis_heap_alloc(bs);
    if (!buf) return ext2_wst_fail("out of memory");

    /* ---- 1. mkdir ---- */
    r = ext2_mkdir("/WRTDIR", 0755);
    if (r != EXT2_OK) goto done;
    create_ok = 1;

    /* ---- 2. create the file and write 13 blocks ---- */
    r = ext2_create("/WRTDIR/F13K.BIN", 0644, &ino);
    if (r != EXT2_OK) goto done;
    r = ext2_wst_write_pattern(ino, blocks, 37, 13, buf);
    if (r != EXT2_OK) goto done;
    write_ok = 1;

    /* ---- 3. read it back byte for byte ---- */
    readback_ok = (ext2_wst_check_pattern(ino, 0, total, 37, 13, buf) == EXT2_OK) ? 1 : 0;

    /* ---- 4. the 13th block must live in the single-indirect table ---- */
    if (readback_ok && ext2_read_inode(ino, &in) == EXT2_OK) {
        indirect_ok = (in.i_block[0] != 0 && in.i_block[EXT2_INDIRECT_BLOCK] != 0 &&
                       in.i_blocks > 0) ? 1 : 0;
    }

    /* ---- 5. truncate: shrink to 4 blocks, verify, grow back, verify zeros ----
     * (the PASS line below implies this step; failures print the flags) */
    if (readback_ok) {
        truncate_ok = 1;
        if (ext2_truncate(ino, 4 * bs) != EXT2_OK ||
            ext2_read_inode(ino, &in) != EXT2_OK || in.i_size != 4 * bs ||
            ext2_wst_check_pattern(ino, 0, 4 * bs, 37, 13, buf) != EXT2_OK) {
            truncate_ok = 0;
        }
        if (truncate_ok) {
            if (ext2_truncate(ino, total) != EXT2_OK ||
                ext2_read_inode(ino, &in) != EXT2_OK || in.i_size != total) {
                truncate_ok = 0;
            } else {
                for (i = 4; i < blocks && truncate_ok; i++) {
                    r = ext2_read_file(ino, buf, i * bs, bs);
                    if (r != (int)bs) { truncate_ok = 0; break; }
                    for (k = 0; k < bs; k++) {
                        if (buf[k] != 0) { truncate_ok = 0; break; }
                    }
                }
            }
        }
    }

    /* ---- 5b. a write that would need the triple-indirect pointer must be
     *      refused honestly (EXT2_ENOTSUP) instead of silently doing nothing.
     *      Only addressable when the block size keeps that offset in 32 bits. ---- */
    if (readback_ok) {
        uint64_t ptrs = fs->ptrs_per_block;
        uint64_t triple_start = EXT2_DIRECT_BLOCKS + ptrs + ptrs * ptrs;
        uint64_t triple_off = triple_start * bs;
        if (triple_off + 1u <= 0xFFFFFFFFu) {
            triple_tested = 1;
            r = ext2_write_at(ino, buf, (uint32_t)triple_off, 1);
            triple_ok = (r == EXT2_ENOTSUP) ? 1 : 0;
            if (triple_ok) {
                console_printf("[ext2] write self-test triple-indirect write refused "
                               "(ENOTSUP at logical block %u)\n",
                               (uint64_t)triple_start);
            }
        }
    }

    /* ---- 6. rename: same directory, across directories, then the directory
     *      itself (that one exercises the ".." fixup and the parent nlink) ---- */
    if (readback_ok) {
        rename_ok = 1;
        if (ext2_rename("/WRTDIR/F13K.BIN", "/WRTDIR/R13K.BIN") != EXT2_OK ||
            ext2_rename("/WRTDIR/R13K.BIN", "/M13K.BIN") != EXT2_OK ||
            ext2_rename("/WRTDIR", "/WRTDIR2") != EXT2_OK ||
            ext2_rename("/WRTDIR2", "/WRTDIR") != EXT2_OK ||
            ext2_rename("/M13K.BIN", "/WRTDIR/R13K.BIN") != EXT2_OK) {
            rename_ok = 0;
        }
        /* the content must have survived every move */
        if (rename_ok &&
            (ext2_stat("/WRTDIR/R13K.BIN", &st) != EXT2_OK || !st.is_file ||
             st.size != total)) {
            rename_ok = 0;
        }
    }

    /* ---- 6b. POSIX details: rmdir must refuse a non-empty directory and
     *      rename over an existing file must replace it (target deleted) ---- */
    if (rename_ok) {
        uint32_t a_ino = 0, b_ino = 0;
        if (ext2_create("/WRTDIR/A.TXT", 0644, &a_ino) == EXT2_OK) {
            nonempty_ok = (ext2_rmdir("/WRTDIR") == EXT2_ENOTEMPTY) ? 1 : 0;
            if (ext2_create("/WRTDIR/B.TXT", 0644, &b_ino) == EXT2_OK) {
                overwrite_ok = 1;
                memset(buf, 0xA5, bs);
                if (ext2_write_at(a_ino, buf, 0, bs) != (int)bs) overwrite_ok = 0;
                memset(buf, 0x5A, bs);
                if (overwrite_ok && ext2_write_at(b_ino, buf, 0, bs) != (int)bs) {
                    overwrite_ok = 0;
                }
                if (overwrite_ok) {
                    memset(buf, 0xA5, bs);
                    if (ext2_write_at(a_ino, buf, 0, bs) != (int)bs) overwrite_ok = 0;
                }
                if (overwrite_ok &&
                    (ext2_rename("/WRTDIR/A.TXT", "/WRTDIR/B.TXT") != EXT2_OK ||
                     ext2_stat("/WRTDIR/B.TXT", &st) != EXT2_OK || st.size != bs)) {
                    overwrite_ok = 0;
                }
                if (overwrite_ok) {
                    memset(buf, 0, bs);
                    if (ext2_read_file(st.ino, buf, 0, bs) != (int)bs) {
                        overwrite_ok = 0;
                    } else {
                        for (k = 0; k < bs; k++) {
                            if (buf[k] != 0xA5) { overwrite_ok = 0; break; }
                        }
                    }
                }
                /* A.TXT must be gone (its name was moved onto B.TXT) */
                if (overwrite_ok && ext2_stat("/WRTDIR/A.TXT", &st) != EXT2_ENOENT) {
                    overwrite_ok = 0;
                }
                ext2_unlink("/WRTDIR/B.TXT");
            }
        }
    }

    /* ---- 7. unlink the file, rmdir the directory ---- */
    if (rename_ok) unlink_ok = (ext2_unlink("/WRTDIR/R13K.BIN") == EXT2_OK) ? 1 : 0;
    if (unlink_ok) {
        rmdir_ok = (ext2_rmdir("/WRTDIR") == EXT2_OK &&
                    ext2_stat("/WRTDIR", &st) == EXT2_ENOENT) ? 1 : 0;
    }

    /* ---- 8. persistent artifact: byte-exact material for the host-side
     *      verifier (scripts/verify_ext2_write.py).
     *      13 blocks always (12 direct + single indirect). When the volume has
     *      enough free blocks the artifact grows past the single-indirect range
     *      so the double-indirect write path is covered as well and can be
     *      verified byte for byte on the host. ---- */
    artifact_blocks = blocks;
    {
        uint32_t ptrs = fs->ptrs_per_block;
        uint32_t want = EXT2_DIRECT_BLOCKS + ptrs + 8u;
        uint32_t free_b = 0, free_i = 0;
        if (ext2_count_free(&free_b, &free_i) == EXT2_OK && free_b > want + 8u) {
            artifact_blocks = want;
        }
    }
    if (rmdir_ok) {
        if (ext2_stat(EXT2_KEEP_NAME, &st) == EXT2_OK) ext2_unlink(EXT2_KEEP_NAME);
        if (ext2_create(EXT2_KEEP_NAME, 0644, &keep_ino) == EXT2_OK) {
            r = ext2_wst_write_pattern(keep_ino, artifact_blocks, 41, 5, buf);
            if (r == EXT2_OK) {
                console_printf("[ext2] write self-test artifact %s blocks=%u "
                               "indirect=%s\n", EXT2_KEEP_NAME,
                               (uint64_t)artifact_blocks,
                               (artifact_blocks > EXT2_DIRECT_BLOCKS + fs->ptrs_per_block)
                                   ? "double" : "single");
            }
        }
    }

    /* ---- 9. the superblock / group descriptor counters must match the
     *      bitmaps that the operations above just changed ---- */
    consistent = ext2_bitmaps_consistent();

done:
    lemis_heap_free(buf);

    /* The required summary line is printed verbatim; the extra coverage above
     * (truncate, rmdir non-empty refusal, rename-over-existing, honest
     * triple-indirect refusal) must pass for it to be printed at all. */
    if (create_ok && write_ok && readback_ok && indirect_ok && rename_ok &&
        unlink_ok && rmdir_ok && truncate_ok && nonempty_ok && overwrite_ok &&
        (triple_ok || !triple_tested)) {
        console_puts("[ext2] write self-test PASS (create=1 write=1 readback=1 "
                     "indirect=1 rename=1 unlink=1 rmdir=1)\n");
        if (consistent) {
            console_puts("[ext2] bitmaps consistent (free blocks/inodes)\n");
            return EXT2_OK;
        }
        console_puts("[ext2] bitmaps INCONSISTENT (free blocks/inodes)\n");
        return EXT2_ERR;
    }
    console_printf("[ext2] write self-test FAIL (create=%u write=%u readback=%u "
                   "indirect=%u rename=%u unlink=%u rmdir=%u truncate=%u "
                   "nonempty=%u overwrite=%u triple=%u tested=%u)\n",
                   (uint64_t)create_ok, (uint64_t)write_ok, (uint64_t)readback_ok,
                   (uint64_t)indirect_ok, (uint64_t)rename_ok, (uint64_t)unlink_ok,
                   (uint64_t)rmdir_ok, (uint64_t)truncate_ok, (uint64_t)nonempty_ok,
                   (uint64_t)overwrite_ok, (uint64_t)triple_ok, (uint64_t)triple_tested);
    return EXT2_ERR;
}
