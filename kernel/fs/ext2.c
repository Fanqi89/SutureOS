/* SutureOS: real ext2 (rev 0 / rev 1) READ-ONLY filesystem driver
 *
 * Ported/adapted from Cinux libs/ext2 + kernel/fs/ext2_*.cpp (MIT):
 *   C:\Users\fanqi\Desktop\自研操作系统\Cinux\libs\ext2
 * Adapted to SutureOS: blk_read() block layer, heap scratch buffers, console
 * logging with the [ext2] prefix, a RAM-disk path for in-RAM test images,
 * triple-indirect pointers (Cinux stops at double indirect), on-disk value
 * validation and a built-in guest selftest.
 *
 * READ-ONLY: there is no write path. The VFS adapter maps every mutating call
 * to VFS_EROFS (== VFS_EPERM; kernel/vfs/vfs.h has no dedicated EROFS code).
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
    g_ext2_fs.ram_size = 0;
    return ext2_mount_current_device();
}

int ext2_mount_ramdisk(void) {
    if (!g_ram_image) return ext2_mount_fail("no ramdisk registered");
    ext2_teardown();
    g_ext2_fs.ram = g_ram_image;
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
        ext2_warn1("filesystem is not marked clean; mounting read-only anyway",
                   fs->sb.s_state);
    }

    fs->mounted = 1;
    /* THE mount line (exactly one, as specified) */
    console_printf("[ext2] mount blocksz=%u blocks=%u inodes=%u groups=%u rev=%u\n",
                   (uint64_t)fs->block_size, (uint64_t)fs->blocks_count,
                   (uint64_t)fs->inodes_count, (uint64_t)fs->group_count,
                   (uint64_t)fs->rev_level);
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
