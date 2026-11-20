/* SutureOS: ext4 READ-ONLY filesystem driver - extent-tree volumes
 *
 * Provenance: written for SutureOS; no third-party source code was copied. The
 * on-disk format implemented here is the public ext4 specification published
 * with the Linux kernel documentation (Documentation/filesystems/ext4/):
 * superblock, group descriptors, inodes, extent trees and directory entries.
 * The driver plumbing (block layer, heap scratch buffers, single-volume context,
 * error enum, VFS adapter shape) deliberately mirrors kernel/fs/ext2.c so both
 * drivers can be reviewed together; ext2.c itself was NOT modified.
 *
 * Why read-only: the port list's file-system gap is "read a real ext4 volume".
 * A write path would need bitmap/GDT allocation, extent split/merge and journal
 * recovery decisions - all out of scope, so every mutating VFS entry point
 * returns EPERM instead of pretending.
 *
 * What is proven by scripts/ext4_acceptance.py + scripts/verify_ext4_read.py:
 *   - superblock at byte offset 1024, geometry cross-checked against the device
 *   - group descriptor table walk (1 group in the fixture, N groups in code)
 *   - inode table indexing with 256-byte inodes
 *   - EXTENT TREES: depth-0 leaves, depth-1 index -> two leaf blocks
 *     (the fixture's /BIG.BIN has 384 one-block extents, which cannot fit in one
 *     leaf block, so the depth-1 path is really traversed)
 *   - directory reads with the filetype dirent format
 *   - sparse files (a logical block no extent covers reads as zeroes)
 *   - write-operations-refused behaviour
 */
#include "ext4.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "block.h"

/* ==================== volume state (single ext4 volume) ==================== */

static struct ext4_fs g_ext4;

#define EXT4_MAX_OPEN_FILES 8
#define EXT4_MAX_OPEN_DIRS  8
static struct ext4_file g_files[EXT4_MAX_OPEN_FILES];
static struct ext4_dir  g_dirs[EXT4_MAX_OPEN_DIRS];

/* Warnings are budgeted so a corrupt image cannot flood the serial log. */
#define EXT4_WARN_BUDGET 32u
static uint32_t g_warn_left = EXT4_WARN_BUDGET;
static int g_ro_note_done = 0;

static int  ext4_read_inode_raw(uint32_t ino, struct ext4_inode *out);
static int  ext4_bmap(const struct ext4_inode *in, uint64_t logical,
                      uint32_t *out_phys, uint8_t *scratch);
static int  ext4_resolve(const char *path, uint32_t *out_ino,
                         struct ext4_inode *out_inode);
static void ext4_teardown(void);
static int  ext4_mount_device(void);

/* ==================== small helpers ==================== */

static uint32_t ext4_rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t ext4_rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t ext4_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void ext4_warn1(const char *msg, uint64_t a) {
    if (g_warn_left == 0) return;
    g_warn_left--;
    console_printf("[ext4] WARNING: %s: %u\n", msg, a);
    if (g_warn_left == 0) console_puts("[ext4] WARNING: further warnings suppressed\n");
}

static void ext4_warn2(const char *msg, uint64_t a, uint64_t b) {
    if (g_warn_left == 0) return;
    g_warn_left--;
    console_printf("[ext4] WARNING: %s: %u %u\n", msg, a, b);
    if (g_warn_left == 0) console_puts("[ext4] WARNING: further warnings suppressed\n");
}

/* One log line per rejected feature bit: the port rule is "print the exact bit,
 * never guess what an unknown feature means". */
struct ext4_feature_name {
    uint32_t bit;
    const char *name;
};

static const struct ext4_feature_name g_incompat_names[] = {
    { 0x0001u, "compression" },
    { 0x0002u, "filetype" },
    { 0x0004u, "recover" },
    { 0x0008u, "journal_dev" },
    { 0x0010u, "meta_bg" },
    { 0x0040u, "extents" },
    { 0x0080u, "64bit" },
    { 0x0100u, "mmp" },
    { 0x0200u, "bigalloc" },
    { 0x0400u, "ea_inode" },
    { 0x1000u, "dirdata" },
    { 0x2000u, "csum_seed" },
    { 0x4000u, "largedir" },
    { 0x8000u, "inline_data" },
    { 0x10000u, "encrypt" },
    { 0x20000u, "casefold" },
};

static const struct ext4_feature_name g_compat_names[] = {
    { 0x0001u, "dir_prealloc" },
    { 0x0002u, "imagic_inodes" },
    { 0x0004u, "has_journal" },
    { 0x0008u, "ext_attr" },
    { 0x0010u, "resize_inode" },
    { 0x0020u, "dir_index" },
    { 0x0040u, "lazy_bg" },
    { 0x0080u, "exclude_inode" },
    { 0x0100u, "exclude_bitmap" },
    { 0x0200u, "sparse_super2" },
    { 0x0400u, "fast_commit" },
    { 0x0800u, "stable_inodes" },
    { 0x1000u, "orphan_file" },
};

static const struct ext4_feature_name g_ro_compat_names[] = {
    { 0x0001u, "sparse_super" },
    { 0x0002u, "large_file" },
    { 0x0004u, "btree_dir" },
    { 0x0008u, "huge_file" },
    { 0x0010u, "gdt_csum" },
    { 0x0020u, "dir_nlink" },
    { 0x0040u, "extra_isize" },
    { 0x0100u, "quota" },
    { 0x0200u, "bigalloc" },
    { 0x0400u, "metadata_csum" },
    { 0x0800u, "replica" },
    { 0x1000u, "read-only" },
    { 0x2000u, "project" },
};

static const char *ext4_feature_name(const struct ext4_feature_name *tab,
                                    uint32_t tab_len, uint32_t bit) {
    for (uint32_t i = 0; i < tab_len; i++) {
        if (tab[i].bit == bit) return tab[i].name;
    }
    return "unknown";
}

/* Print every set bit of `bits` with its documented name, one line each, in
 * ascending bit order (so two runs produce identical logs). */
static void ext4_print_feature_bits(const char *word, const char *verdict,
                                    const struct ext4_feature_name *tab,
                                    uint32_t tab_len, uint32_t bits) {
    for (uint32_t b = 0; b < 32; b++) {
        uint32_t bit = 1u << b;
        if (!(bits & bit)) continue;
        console_printf("[ext4] %s feature 0x%x (%s) %s\n", word,
                       (uint64_t)bit, ext4_feature_name(tab, tab_len, bit), verdict);
    }
}

/* ==================== device I/O ==================== */

static int ext4_dev_read_block(uint32_t block, void *buf) {
    struct ext4_fs *fs = &g_ext4;
    if (!buf) return EXT4_EINVAL;
    if (block >= fs->blocks_count) {
        ext4_warn2("block read out of range", block, fs->blocks_count);
        return EXT4_EINVAL;
    }
    {
        uint64_t lba = (uint64_t)fs->start_lba + (uint64_t)block * fs->sectors_per_block;
        if (blk_read(fs->drive, lba, fs->sectors_per_block, buf) != 0) {
            ext4_warn1("block read failed at lba", lba);
            return EXT4_EIO;
        }
    }
    return EXT4_OK;
}

/* The ext4 superblock always starts at byte offset 1024. */
static int ext4_read_superblock_bytes(int drive, uint32_t start_lba, uint8_t *out) {
    if (!out) return EXT4_EINVAL;
    if (blk_read(drive, (uint64_t)start_lba + 2, 2, out) != 0) return EXT4_EIO;
    return EXT4_OK;
}

/* ==================== lifecycle ==================== */

static void ext4_teardown(void) {
    struct ext4_fs *fs = &g_ext4;
    if (fs->bgdt) {
        lemis_heap_free(fs->bgdt);
        fs->bgdt = NULL;
    }
    fs->bgdt_capacity = 0;
    fs->mounted = 0;
    for (int i = 0; i < EXT4_MAX_OPEN_FILES; i++) g_files[i].used = 0;
    for (int i = 0; i < EXT4_MAX_OPEN_DIRS; i++) g_dirs[i].used = 0;
}

static int ext4_mount_fail(const char *reason) {
    console_printf("[ext4] mount failed: %s\n", reason);
    ext4_teardown();
    return EXT4_ERR;
}

static int ext4_mount_fail2(const char *reason, uint32_t a, uint32_t b) {
    char buf[112];
    snprintf(buf, sizeof(buf), reason, (unsigned)a, (unsigned)b);
    return ext4_mount_fail(buf);
}

int ext4_is_mounted(void) { return g_ext4.mounted; }
uint32_t ext4_block_size(void) { return g_ext4.block_size; }
int ext4_volume_drive(void) { return g_ext4.drive; }
uint32_t ext4_max_depth_seen(void) { return g_ext4.max_depth_seen; }
uint32_t ext4_blocks_count(void) { return g_ext4.blocks_count; }
uint32_t ext4_free_blocks_count(void) { return g_ext4.sb.s_free_blocks_count; }

int ext4_write_refused(const char *what) {
    if (!g_ro_note_done) {
        g_ro_note_done = 1;
        console_printf("[ext4] write rejected: the ext4 driver is read-only (%s)\n",
                       what ? what : "write");
    }
    return EXT4_EROFS;
}

/* Print the one-line answer the boot probe needs. Returns 1 when the volume is
 * something this driver could mount (it does not mount it). */
int ext4_probe(int drive) {
    uint8_t *sb_bytes;
    struct ext4_superblock sb;
    uint32_t block_size;
    uint32_t unsupported;
    int r;

    if (drive < 0) return 0;
    if (!blk_find_by_drive(drive)) {
        console_printf("[ext4] no ext4 volume on drive %d (no such block device)\n",
                       (int64_t)drive);
        return 0;
    }
    sb_bytes = (uint8_t *)lemis_heap_alloc(EXT4_SUPERBLOCK_SIZE);
    if (!sb_bytes) {
        console_printf("[ext4] probe drive %d skipped: out of memory\n", (int64_t)drive);
        return 0;
    }
    r = ext4_read_superblock_bytes(drive, 0, sb_bytes);
    if (r != EXT4_OK) {
        lemis_heap_free(sb_bytes);
        console_printf("[ext4] no ext4 volume on drive %d (superblock unreadable)\n",
                       (int64_t)drive);
        return 0;
    }
    memset(&sb, 0, sizeof(sb));
    memcpy(&sb, sb_bytes, sizeof(sb));
    lemis_heap_free(sb_bytes);

    if (sb.s_magic != EXT4_SUPER_MAGIC) {
        console_printf("[ext4] no ext4 volume on drive %d (magic 0x%x)\n",
                       (int64_t)drive, (uint64_t)sb.s_magic);
        return 0;
    }
    if (sb.s_rev_level == 0 || sb.s_log_block_size > 2) {
        console_printf("[ext4] no ext4 volume on drive %d (old/foreign ext2-style "
                       "superblock)\n", (int64_t)drive);
        return 0;
    }
    unsupported = sb.s_feature_incompat & ~EXT4_FEATURE_INCOMPAT_SUPPORTED;
    if (unsupported || !(sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        console_printf("[ext4] no mountable ext4 volume on drive %d "
                       "(incompat=0x%x)\n", (int64_t)drive,
                       (uint64_t)sb.s_feature_incompat);
        ext4_print_feature_bits("incompat", "is NOT supported", g_incompat_names,
                               sizeof(g_incompat_names) / sizeof(g_incompat_names[0]),
                               unsupported);
        return 0;
    }
    block_size = 1024u << sb.s_log_block_size;
    if (sb.s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX) {
        /* The probe must not advertise a volume that mount would refuse. */
        console_printf("[ext4] no mountable ext4 volume on drive %d "
                       "(hashed directories / dir_index are not supported)\n",
                       (int64_t)drive);
        return 0;
    }
    console_printf("[ext4] ext4 volume on drive %d: blocksz=%u blocks=%u inodes=%u "
                   "(read-only; mount with: mount ext4 %d /mnt/e4)\n",
                   (int64_t)drive, (uint64_t)block_size,
                   (uint64_t)sb.s_blocks_count, (uint64_t)sb.s_inodes_count,
                   (int64_t)drive);
    return 1;
}

int ext4_mount(int drive, uint32_t start_lba) {
    if (g_ext4.mounted) {
        console_puts("[ext4] mount refused: the ext4 driver already has a volume "
                     "(single-volume driver, unmount first)\n");
        return EXT4_EBUSY;
    }
    memset(&g_ext4, 0, sizeof(g_ext4));
    ext4_teardown();
    g_ext4.drive = drive;
    g_ext4.start_lba = start_lba;
    int r = ext4_mount_device();
    if (r != EXT4_OK) return r;
    return EXT4_OK;
}

int ext4_unmount(void) {
    if (!g_ext4.mounted) return EXT4_EBUSY;
    console_printf("[ext4] unmounted drive=%d\n", (int64_t)g_ext4.drive);
    ext4_teardown();
    g_ro_note_done = 0;
    return EXT4_OK;
}

/* Forward declarations for the mount-time probe (defined with the read path). */
static int ext4_probe_files(int emit_log);

static int ext4_mount_device(void) {
    struct ext4_fs *fs = &g_ext4;
    uint8_t *sb_bytes;
    int r;

    g_warn_left = EXT4_WARN_BUDGET;
    g_ro_note_done = 0;

    sb_bytes = (uint8_t *)lemis_heap_alloc(EXT4_SUPERBLOCK_SIZE);
    if (!sb_bytes) return ext4_mount_fail("out of memory reading the superblock");
    r = ext4_read_superblock_bytes(fs->drive, fs->start_lba, sb_bytes);
    if (r != EXT4_OK) {
        lemis_heap_free(sb_bytes);
        return ext4_mount_fail("I/O error reading the superblock at offset 1024 "
                               "(no volume, or the drive is not readable)");
    }
    memset(&fs->sb, 0, sizeof(fs->sb));
    memcpy(&fs->sb, sb_bytes, sizeof(fs->sb));
    lemis_heap_free(sb_bytes);

    /* ---- superblock validation (never trust on-disk values) ---- */
    if (fs->sb.s_magic != EXT4_SUPER_MAGIC) {
        return ext4_mount_fail2("invalid magic 0x%x (want 0xEF53)",
                                fs->sb.s_magic, 0);
    }
    if (fs->sb.s_log_block_size > 2) {
        return ext4_mount_fail2("unsupported block size 2^%u (max 4096)",
                                fs->sb.s_log_block_size, 0);
    }
    fs->block_size = 1024u << fs->sb.s_log_block_size;
    fs->sectors_per_block = fs->block_size / EXT4_SECTOR_SIZE;

    if (fs->sb.s_blocks_count == 0) return ext4_mount_fail("s_blocks_count is 0");
    if (fs->sb.s_inodes_count == 0) return ext4_mount_fail("s_inodes_count is 0");
    if (fs->sb.s_blocks_per_group == 0) return ext4_mount_fail("s_blocks_per_group is 0");
    if (fs->sb.s_inodes_per_group == 0) return ext4_mount_fail("s_inodes_per_group is 0");
    if (fs->sb.s_rev_level == 0) {
        return ext4_mount_fail("superblock revision 0 (no feature fields): this is an "
                               "ext2-style volume, not ext4 (use the ext2 driver)");
    }
    if (fs->sb.s_rev_level > 1) {
        return ext4_mount_fail2("unsupported revision %u", fs->sb.s_rev_level, 0);
    }

    {
        uint32_t expect = (fs->block_size == 1024) ? 1u : 0u;
        if (fs->sb.s_first_data_block != expect) {
            return ext4_mount_fail2("s_first_data_block is %u, expected %u",
                                    fs->sb.s_first_data_block, expect);
        }
    }
    fs->first_data_block = fs->sb.s_first_data_block;

    fs->inode_size = fs->sb.s_inode_size;
    if (fs->inode_size < EXT4_INODE_BASE_SIZE ||
        fs->inode_size > fs->block_size ||
        (fs->block_size % fs->inode_size) != 0) {
        return ext4_mount_fail2("bad inode size %u for block size",
                                fs->inode_size, fs->block_size);
    }
    fs->desc_size = fs->sb.s_desc_size;
    if (fs->desc_size != 0 && fs->desc_size != 32) {
        return ext4_mount_fail2("unsupported group descriptor size %u "
                                "(only the 32-byte form is supported)", fs->desc_size, 0);
    }

    /* ---- features: refuse precisely, never guess ---- */
    {
        uint32_t incompat = fs->sb.s_feature_incompat;
        uint32_t unsupported = incompat & ~EXT4_FEATURE_INCOMPAT_SUPPORTED;
        if (unsupported) {
            console_printf("[ext4] incompatible feature bits 0x%x are not supported "
                           "by this driver\n", (uint64_t)unsupported);
            ext4_print_feature_bits("incompat", "is NOT supported", g_incompat_names,
                                    sizeof(g_incompat_names) / sizeof(g_incompat_names[0]),
                                    unsupported);
            return ext4_mount_fail2("unsupported incompat features 0x%x "
                                    "(listed bit by bit above)", unsupported, 0);
        }
        if (!(incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
            return ext4_mount_fail("the extents feature is not set: this volume uses "
                                   "classic block pointers (ext2), use the ext2 driver");
        }
        if (!(incompat & EXT4_FEATURE_INCOMPAT_FILETYPE)) {
            console_puts("[ext4] note: filetype feature is off; dirent file types "
                         "come from the inode mode instead\n");
        }
    }
    {
        uint32_t compat = fs->sb.s_feature_compat;
        if (compat & EXT4_FEATURE_COMPAT_DIR_INDEX) {
            return ext4_mount_fail("hashed directories (dir_index) are not supported: "
                                   "a linear reader would silently misread the dx_root "
                                   "block, so the volume is refused instead");
        }
        if (compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
            console_puts("[ext4] note: volume has a journal (has_journal); a read-only "
                         "mount ignores it, the volume must be cleanly unmounted\n");
        }
        {
            uint32_t rest = compat & ~(EXT4_FEATURE_COMPAT_DIR_INDEX |
                                       EXT4_FEATURE_COMPAT_HAS_JOURNAL);
            if (rest) {
                console_printf("[ext4] note: compat feature bits 0x%x are ignored "
                               "(safe for a read-only mount)\n", (uint64_t)rest);
                ext4_print_feature_bits("compat", "is ignored (read-only mount)",
                                        g_compat_names,
                                        sizeof(g_compat_names) / sizeof(g_compat_names[0]),
                                        rest);
            }
        }
        {
            uint32_t ro = fs->sb.s_feature_ro_compat;
            uint32_t rest = ro & ~EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER;
            if (ro & 0x0400u) {
                /* metadata_csum is ro_compat, so a read-only mount is legal, but
                 * the driver does not verify inode/bitmap/descriptor checksums:
                 * say so instead of implying integrity checking. */
                console_puts("[ext4] note: metadata_csum is set; this driver does "
                             "NOT verify checksums (bit-rot would go unnoticed)\n");
            }
            if (rest) {
                ext4_print_feature_bits("ro_compat", "is not interpreted (read-only)",
                                        g_ro_compat_names,
                                        sizeof(g_ro_compat_names) /
                                        sizeof(g_ro_compat_names[0]), rest);
            }
        }
    }

    /* ---- cap the block count by the real device size ---- */
    {
        uint64_t dev_blocks;
        uint64_t sectors = 0;
        if (blk_get_info(fs->drive, &sectors, NULL) != 0 || sectors == 0) {
            ext4_warn1("device size unknown, trusting s_blocks_count on drive",
                       (uint64_t)fs->drive);
            dev_blocks = fs->sb.s_blocks_count;
        } else if (sectors <= fs->start_lba) {
            return ext4_mount_fail("device is smaller than the partition offset");
        } else {
            dev_blocks = (sectors - fs->start_lba) / fs->sectors_per_block;
        }
        fs->blocks_count_raw = fs->sb.s_blocks_count;
        if ((uint64_t)fs->sb.s_blocks_count > dev_blocks) {
            ext4_warn2("s_blocks_count exceeds the device, clamping to",
                       fs->sb.s_blocks_count, dev_blocks);
            fs->blocks_count = (uint32_t)dev_blocks;
        } else {
            fs->blocks_count = fs->sb.s_blocks_count;
        }
        if (fs->blocks_count == 0) return ext4_mount_fail("no usable blocks on the device");
    }

    /* ---- group count + descriptor table ---- */
    {
        uint64_t groups = ((uint64_t)fs->blocks_count + fs->sb.s_blocks_per_group - 1) /
                          fs->sb.s_blocks_per_group;
        uint64_t ino_groups = ((uint64_t)fs->sb.s_inodes_count +
                               fs->sb.s_inodes_per_group - 1) / fs->sb.s_inodes_per_group;
        if (ino_groups > groups) groups = ino_groups;
        if (groups == 0 || groups > EXT4_MAX_GROUPS) {
            return ext4_mount_fail2("unsupported group count %u (max %u)",
                                    (uint32_t)groups, EXT4_MAX_GROUPS);
        }
        fs->group_count = (uint32_t)groups;
    }
    fs->blocks_per_group = fs->sb.s_blocks_per_group;
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    fs->inodes_count = fs->sb.s_inodes_count;
    fs->first_ino = fs->sb.s_first_ino;
    if (fs->first_ino < 11u || fs->first_ino > fs->inodes_count) fs->first_ino = 11u;

    {
        uint32_t entries_per_block = fs->block_size / (uint32_t)sizeof(struct ext4_block_group_desc);
        uint32_t gdt_blocks = (fs->group_count + entries_per_block - 1) / entries_per_block;
        uint32_t gdt_block = fs->first_data_block + 1;
        uint8_t *buf;
        struct ext4_block_group_desc *table;

        fs->bgdt = (struct ext4_block_group_desc *)
            lemis_heap_alloc((uintptr_t)fs->group_count * sizeof(struct ext4_block_group_desc));
        if (!fs->bgdt) return ext4_mount_fail("out of memory for the group descriptor table");
        fs->bgdt_capacity = fs->group_count;
        fs->gdt_entries_per_block = entries_per_block;
        fs->gdt_block = gdt_block;
        table = fs->bgdt;
        memset(table, 0, (uintptr_t)fs->group_count * sizeof(*table));

        buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
        if (!buf) return ext4_mount_fail("out of memory for the group descriptor buffer");

        for (uint32_t i = 0; i < gdt_blocks; i++) {
            uint32_t first = i * entries_per_block;
            uint32_t n;
            if (first >= fs->group_count) break;
            n = ext4_min_u32(entries_per_block, fs->group_count - first);
            r = ext4_dev_read_block(gdt_block + i, buf);
            if (r != EXT4_OK) {
                lemis_heap_free(buf);
                return ext4_mount_fail("I/O error reading the group descriptor table");
            }
            memcpy(&table[first], buf, (uintptr_t)n * sizeof(struct ext4_block_group_desc));
        }
        lemis_heap_free(buf);
    }

    /* ---- per-group sanity ---- */
    {
        uint32_t table_blocks =
            (fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
        for (uint32_t g = 0; g < fs->group_count; g++) {
            struct ext4_block_group_desc *gd = &fs->bgdt[g];
            if (gd->bg_block_bitmap == 0 || gd->bg_block_bitmap >= fs->blocks_count) {
                if (g == 0) return ext4_mount_fail2("group 0 block bitmap %u is out of range",
                                                    gd->bg_block_bitmap, fs->blocks_count);
                ext4_warn2("group block bitmap out of range", g, gd->bg_block_bitmap);
            }
            if (gd->bg_inode_bitmap == 0 || gd->bg_inode_bitmap >= fs->blocks_count) {
                if (g == 0) return ext4_mount_fail2("group 0 inode bitmap %u is out of range",
                                                    gd->bg_inode_bitmap, fs->blocks_count);
                ext4_warn2("group inode bitmap out of range", g, gd->bg_inode_bitmap);
            }
            if (gd->bg_inode_table == 0 || gd->bg_inode_table >= fs->blocks_count ||
                (uint64_t)gd->bg_inode_table + table_blocks > fs->blocks_count) {
                if (g == 0) return ext4_mount_fail2("group 0 inode table %u is out of range",
                                                    gd->bg_inode_table, fs->blocks_count);
                ext4_warn2("group inode table out of range, group skipped", g,
                           gd->bg_inode_table);
            }
        }
    }

    /* ---- the root inode must exist, be a directory and use an extent tree ---- */
    {
        struct ext4_inode root;
        if (ext4_read_inode_raw(2, &root) != EXT4_OK) {
            return ext4_mount_fail("cannot read the root inode (inode 2)");
        }
        if ((root.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            return ext4_mount_fail2("root inode 2 is not a directory (mode 0x%x)",
                                    root.i_mode, 0);
        }
        if ((root.i_flags & EXT4_EXTENTS_FL) == 0) {
            return ext4_mount_fail("root inode 2 has no extents flag: the volume is not "
                                   "using extent trees, so it is not the format this "
                                   "driver implements");
        }
    }

    if ((fs->sb.s_state & 0x0001u) == 0 || (fs->sb.s_state & 0x0002u) != 0) {
        ext4_warn1("filesystem is not marked clean; reading it anyway", fs->sb.s_state);
    }

    fs->mounted = 1;

    /* Learn the deepest extent tree first so the mount line can report it, then
     * emit the per-file extent evidence. Both passes only read metadata blocks. */
    fs->max_depth_seen = 0;
    (void)ext4_probe_files(0);
    console_printf("[ext4] mount drive=%d blocksz=%u blocks=%u inodes=%u groups=%u "
                   "extent_depth_max=%u (read-only)\n",
                   (int64_t)fs->drive, (uint64_t)fs->block_size,
                   (uint64_t)fs->blocks_count, (uint64_t)fs->inodes_count,
                   (uint64_t)fs->group_count, (uint64_t)fs->max_depth_seen);
    (void)ext4_probe_files(1);
    console_puts("[ext4] read-only: open/read/stat/readdir only; create/write/"
                 "truncate/mkdir/unlink/rename return EPERM\n");
    return EXT4_OK;
}

/* ==================== blocks / inodes ==================== */

int ext4_read_block_raw(uint32_t block, void *buf) {
    if (!buf) return EXT4_EINVAL;
    if (!g_ext4.mounted) return EXT4_EBUSY;
    return ext4_dev_read_block(block, buf);
}

static int ext4_read_inode_raw(uint32_t ino, struct ext4_inode *out) {
    struct ext4_fs *fs = &g_ext4;
    uint32_t group, index, byte_off, blk, off;
    uint8_t *buf;
    int r;

    if (!out) return EXT4_EINVAL;
    if (ino == 0 || ino > fs->inodes_count) {
        ext4_warn2("inode number out of range", ino, fs->inodes_count);
        return EXT4_EINVAL;
    }
    group = (ino - 1) / fs->inodes_per_group;
    if (group >= fs->group_count) {
        ext4_warn2("inode maps past the last block group", ino, group);
        return EXT4_EINVAL;
    }
    index = (ino - 1) % fs->inodes_per_group;
    byte_off = index * fs->inode_size;
    blk = fs->bgdt[group].bg_inode_table + byte_off / fs->block_size;
    off = byte_off % fs->block_size;
    if (blk >= fs->blocks_count) {
        ext4_warn2("inode table block out of range for inode", ino, blk);
        return EXT4_EINVAL;
    }
    if (off + EXT4_INODE_BASE_SIZE > fs->block_size) {
        /* cannot happen while block_size % inode_size == 0 (checked at mount) */
        ext4_warn2("inode crosses a block boundary", ino, off);
        return EXT4_EIO;
    }

    buf = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!buf) return EXT4_ENOMEM;
    r = ext4_dev_read_block(blk, buf);
    if (r == EXT4_OK) {
        uint32_t copy = fs->inode_size < sizeof(*out) ? fs->inode_size : (uint32_t)sizeof(*out);
        memset(out, 0, sizeof(*out));
        memcpy(out, buf + off, copy);
    }
    lemis_heap_free(buf);
    return r;
}

int ext4_read_inode(uint32_t ino, struct ext4_inode *out) {
    if (!g_ext4.mounted) return EXT4_EBUSY;
    return ext4_read_inode_raw(ino, out);
}

/* Validate an extent-tree header sitting in `node` (node_bytes long).
 * `is_root` tells the caller how many entries fit in an inode's i_block. */
static int ext4_extent_check_header(const uint8_t *node, uint32_t node_bytes,
                                    int is_root, uint16_t *out_entries,
                                    uint16_t *out_max, uint16_t *out_depth) {
    uint16_t magic, entries, max, depth;
    uint32_t cap;

    if (node_bytes < EXT4_EXTENT_HDR_SIZE) return EXT4_ECORRUPT;
    magic = ext4_rd16(node);
    entries = ext4_rd16(node + 2);
    max = ext4_rd16(node + 4);
    depth = ext4_rd16(node + 6);
    cap = (node_bytes - EXT4_EXTENT_HDR_SIZE) / EXT4_EXTENT_ENTRY_SIZE;
    if (is_root && cap > 4) cap = 4;
    if (magic != EXT4_EXTENT_MAGIC) {
        ext4_warn1("extent header magic is not 0xF30A (0x", magic);
        return EXT4_ECORRUPT;
    }
    if (entries > max || max > cap) {
        ext4_warn2("extent header entries/max out of bounds", entries, max);
        return EXT4_ECORRUPT;
    }
    if (depth > EXT4_EXTENT_MAX_DEPTH) {
        ext4_warn1("extent tree deeper than 4 levels", depth);
        return EXT4_ECORRUPT;
    }
    if (out_entries) *out_entries = entries;
    if (out_max) *out_max = max;
    if (out_depth) *out_depth = depth;
    return EXT4_OK;
}

/* Map one logical file block to a physical block through the extent tree.
 * *out_phys == 0 means "hole" (or an unwritten extent): read as zeroes. */
static int ext4_bmap(const struct ext4_inode *in, uint64_t logical,
                     uint32_t *out_phys, uint8_t *scratch) {
    struct ext4_fs *fs = &g_ext4;
    uint8_t root[60];
    const uint8_t *node;
    uint32_t node_bytes;
    uint16_t entries, max, depth;
    uint32_t depth_field;
    int level = 0;
    int r;

    if (!out_phys) return EXT4_EINVAL;
    *out_phys = 0;
    if ((in->i_flags & EXT4_EXTENTS_FL) == 0) {
        /* A zero-length file may legally have no extent header at all. */
        if (in->i_size == 0) return EXT4_OK;
        ext4_warn1("inode without the extents flag (classic block pointers)", in->i_mode);
        return EXT4_ENOTSUP;
    }
    memcpy(root, in->i_block, sizeof(root));
    node = root;
    node_bytes = sizeof(root);
    r = ext4_extent_check_header(node, node_bytes, 1, &entries, &max, &depth);
    if (r != EXT4_OK) return r;
    if (entries == 0 && depth == 0) return EXT4_OK;      /* empty file */
    depth_field = depth;

    for (;;) {
        if (depth_field == 0) {
            /* Leaf: the extents are sorted by logical block. A linear scan is
             * fine - a leaf holds at most 340 entries - and it also validates
             * that every entry is well formed. */
            for (uint32_t i = 0; i < entries; i++) {
                const uint8_t *e = node + EXT4_EXTENT_HDR_SIZE +
                                   i * EXT4_EXTENT_ENTRY_SIZE;
                uint32_t ee_block = ext4_rd32(e);
                uint16_t ee_len = ext4_rd16(e + 4);
                uint32_t len = (uint32_t)(ee_len & 0x7FFFu);
                uint64_t start = ((uint64_t)ext4_rd16(e + 6) << 32) | ext4_rd32(e + 8);
                if (len == 0) {
                    ext4_warn1("extent with length 0 at logical block", ee_block);
                    return EXT4_ECORRUPT;
                }
                if (logical < ee_block) continue;
                if (logical >= (uint64_t)ee_block + len) continue;
                if (ee_len > 32768u) return EXT4_OK;    /* unwritten extent => zeroes */
                {
                    uint64_t phys = start + (logical - ee_block);
                    if (phys == 0 || phys >= fs->blocks_count) {
                        ext4_warn2("extent maps outside the volume", phys,
                                   fs->blocks_count);
                        return EXT4_ECORRUPT;
                    }
                    *out_phys = (uint32_t)phys;
                }
                return EXT4_OK;
            }
            return EXT4_OK;                             /* hole */
        }

        /* Index node: the last entry whose ei_block <= logical points at the
         * child node that must contain it. */
        {
            int found = -1;
            uint64_t leaf = 0;
            for (uint32_t i = 0; i < entries; i++) {
                const uint8_t *e = node + EXT4_EXTENT_HDR_SIZE +
                                   i * EXT4_EXTENT_ENTRY_SIZE;
                uint32_t ei_block = ext4_rd32(e);
                if (ei_block <= logical) found = (int)i;
                else break;
            }
            if (found < 0) return EXT4_OK;              /* before the first child: hole */
            {
                const uint8_t *e = node + EXT4_EXTENT_HDR_SIZE +
                                   (uint32_t)found * EXT4_EXTENT_ENTRY_SIZE;
                leaf = ((uint64_t)ext4_rd16(e + 8) << 32) | ext4_rd32(e + 4);
            }
            if (leaf == 0 || leaf >= fs->blocks_count) {
                ext4_warn2("extent index points outside the volume", leaf,
                           fs->blocks_count);
                return EXT4_ECORRUPT;
            }
            if (level >= (int)EXT4_EXTENT_MAX_DEPTH) return EXT4_ECORRUPT;
            r = ext4_dev_read_block((uint32_t)leaf, scratch);
            if (r != EXT4_OK) return r;
            node = scratch;
            node_bytes = fs->block_size;
            r = ext4_extent_check_header(node, node_bytes, 0, &entries, &max, &depth);
            if (r != EXT4_OK) return r;
            if (depth + 1 != depth_field) {
                ext4_warn2("extent tree depth does not decrease", depth, depth_field);
                return EXT4_ECORRUPT;
            }
            depth_field = depth;
            level++;
        }
    }
}

/* Walk a whole extent tree and collect counts/depth. Bounded: at most
 * EXT4_EXTENT_SCAN_NODES index nodes are followed, so a corrupt or cyclic tree
 * cannot loop forever. */
#define EXT4_EXTENT_SCAN_NODES 512u

struct ext4_scan_ctx {
    struct ext4_extent_info *info;
    uint8_t *scratch;
    uint32_t nodes;
    uint32_t root_depth;
};

static int ext4_extent_walk(const uint8_t *node, uint32_t node_bytes, int is_root,
                            uint32_t expect_depth, struct ext4_scan_ctx *ctx) {
    struct ext4_fs *fs = &g_ext4;
    uint16_t entries, max, depth;
    int r = ext4_extent_check_header(node, node_bytes, is_root, &entries, &max, &depth);

    if (r != EXT4_OK) return r;
    if (depth != expect_depth) {
        ext4_warn2("extent tree depth mismatch while scanning", depth, expect_depth);
        return EXT4_ECORRUPT;
    }
    if (ctx->info->depth < depth) ctx->info->depth = depth;
    if (depth == 0) {
        ctx->info->extents += entries;
        for (uint32_t i = 0; i < entries; i++) {
            const uint8_t *e = node + EXT4_EXTENT_HDR_SIZE +
                               i * EXT4_EXTENT_ENTRY_SIZE;
            uint16_t ee_len = ext4_rd16(e + 4);
            ctx->info->blocks += (uint32_t)(ee_len & 0x7FFFu);
        }
        return EXT4_OK;
    }
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = node + EXT4_EXTENT_HDR_SIZE + i * EXT4_EXTENT_ENTRY_SIZE;
        uint64_t leaf = ((uint64_t)ext4_rd16(e + 8) << 32) | ext4_rd32(e + 4);
        if (ctx->nodes >= EXT4_EXTENT_SCAN_NODES) {
            ext4_warn1("extent tree too large to scan (nodes)", ctx->nodes);
            return EXT4_ECORRUPT;
        }
        if (leaf == 0 || leaf >= fs->blocks_count) {
            ext4_warn2("extent index points outside the volume", leaf, fs->blocks_count);
            return EXT4_ECORRUPT;
        }
        r = ext4_dev_read_block((uint32_t)leaf, ctx->scratch);
        if (r != EXT4_OK) return r;
        ctx->nodes++;
        ctx->info->leaves++;
        r = ext4_extent_walk(ctx->scratch, fs->block_size, 0, depth - 1, ctx);
        if (r != EXT4_OK) return r;
    }
    return EXT4_OK;
}

int ext4_extent_scan(const struct ext4_inode *in, struct ext4_extent_info *info) {
    struct ext4_fs *fs = &g_ext4;
    struct ext4_scan_ctx ctx;
    uint8_t root[60];
    uint8_t *scratch;
    uint16_t entries, max, depth;
    int r;

    if (!in || !info) return EXT4_EINVAL;
    if (!fs->mounted) return EXT4_EBUSY;
    memset(info, 0, sizeof(*info));
    if ((in->i_flags & EXT4_EXTENTS_FL) == 0) {
        if (in->i_size == 0) return EXT4_OK;
        return EXT4_ENOTSUP;
    }
    memcpy(root, in->i_block, sizeof(root));
    r = ext4_extent_check_header(root, sizeof(root), 1, &entries, &max, &depth);
    if (r != EXT4_OK) return r;
    if (entries == 0 && depth == 0) {
        info->holes = (in->i_size + fs->block_size - 1) / fs->block_size;
        return EXT4_OK;
    }
    scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scratch) return EXT4_ENOMEM;
    memset(&ctx, 0, sizeof(ctx));
    ctx.info = info;
    ctx.scratch = scratch;
    r = ext4_extent_walk(root, sizeof(root), 1, depth, &ctx);
    lemis_heap_free(scratch);
    if (r != EXT4_OK) return r;
    {
        uint32_t total = (in->i_size + fs->block_size - 1) / fs->block_size;
        info->holes = (total > info->blocks) ? (total - info->blocks) : 0;
    }
    if (fs->max_depth_seen < info->depth) fs->max_depth_seen = info->depth;
    return EXT4_OK;
}

/* Read `size` bytes at `offset` out of an inode; holes are zero-filled. */
static int ext4_inode_read(const struct ext4_inode *in, void *buf, uint32_t offset,
                           uint32_t size, uint8_t *scratch) {
    struct ext4_fs *fs = &g_ext4;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t file_size = in->i_size;
    uint32_t done = 0;

    if (!buf && size != 0) return EXT4_EINVAL;
    if (offset >= file_size) return 0;
    if (size > file_size - offset) size = file_size - offset;

    while (done < size) {
        uint64_t abs = (uint64_t)offset + done;
        uint64_t logical = abs / fs->block_size;
        uint32_t boff = (uint32_t)(abs % fs->block_size);
        uint32_t chunk = fs->block_size - boff;
        uint32_t phys = 0;
        int r;
        if (chunk > size - done) chunk = size - done;

        r = ext4_bmap(in, logical, &phys, scratch);
        if (r != EXT4_OK) return r;
        if (phys == 0) {
            memset(dst + done, 0, chunk);            /* hole / unwritten extent */
        } else {
            if (ext4_dev_read_block(phys, scratch) != EXT4_OK) return EXT4_EIO;
            memcpy(dst + done, scratch + boff, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

/* ==================== directories / path resolution ==================== */

/* Walk every block of a directory looking for `name`. */
static int ext4_dir_find(const struct ext4_inode *dir, const char *name,
                         uint32_t name_len, uint32_t *out_ino, uint8_t *out_type) {
    struct ext4_fs *fs = &g_ext4;
    uint32_t blocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    uint32_t has_ftype = (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;
    uint8_t *scan = (uint8_t *)lemis_heap_alloc(fs->block_size);
    uint8_t *data = (uint8_t *)lemis_heap_alloc(fs->block_size);

    if (!scan || !data) {
        if (scan) lemis_heap_free(scan);
        if (data) lemis_heap_free(data);
        return EXT4_ENOMEM;
    }
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t phys = 0;
        uint32_t pos = 0;
        int r = ext4_bmap(dir, b, &phys, scan);
        if (r != EXT4_OK) {
            lemis_heap_free(scan);
            lemis_heap_free(data);
            return r;
        }
        if (phys == 0) continue;                     /* hole inside a directory */
        if (ext4_dev_read_block(phys, data) != EXT4_OK) {
            lemis_heap_free(scan);
            lemis_heap_free(data);
            return EXT4_EIO;
        }
        while (pos + EXT4_DIR_ENTRY_HDR <= fs->block_size) {
            uint32_t eino = ext4_rd32(data + pos);
            uint32_t rec = ext4_rd16(data + pos + 4);
            uint32_t nlen = ext4_rd16(data + pos + 6);
            /* With filetype the name_len field is one byte and byte 7 is the
             * file type; without it name_len is the full 16-bit value. */
            if (!has_ftype) nlen &= 0xFFFFu; else nlen &= 0xFFu;
            if (rec < EXT4_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > fs->block_size - pos || nlen > rec - EXT4_DIR_ENTRY_HDR) {
                ext4_warn2("corrupt directory entry (rec_len/name_len)", rec, pos);
                break;
            }
            if (eino != 0 && nlen == name_len &&
                memcmp(data + pos + EXT4_DIR_ENTRY_HDR, name, name_len) == 0) {
                if (out_ino) *out_ino = eino;
                if (out_type) *out_type = has_ftype ? data[pos + 7] : EXT4_FT_UNKNOWN;
                lemis_heap_free(scan);
                lemis_heap_free(data);
                return EXT4_OK;
            }
            pos += rec;
        }
    }
    lemis_heap_free(scan);
    lemis_heap_free(data);
    return EXT4_ENOENT;
}

/* Resolve a volume-relative path ("/", "/HELLO.TXT", "/SUBDIR/../HELLO.TXT"). */
static int ext4_resolve(const char *path, uint32_t *out_ino,
                        struct ext4_inode *out_inode) {
    struct ext4_inode ino;
    uint32_t cur = 2;
    const char *p;
    int r;

    if (!path) return EXT4_EINVAL;
    r = ext4_read_inode_raw(2, &ino);
    if (r != EXT4_OK) return EXT4_EIO;

    p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[EXT4_NAME_MAX + 1];
        uint32_t len = 0;
        uint32_t next = 0;

        while (*p && *p != '/') {
            if (len >= EXT4_NAME_MAX) {
                ext4_warn1("path component longer than 255 bytes at offset",
                           (uint64_t)(p - path));
                return EXT4_EINVAL;
            }
            comp[len++] = *p++;
        }
        while (*p == '/') p++;
        if (len == 0) continue;
        comp[len] = '\0';

        if ((ino.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return EXT4_ENOTDIR;
        r = ext4_dir_find(&ino, comp, len, &next, NULL);
        if (r != EXT4_OK) return EXT4_ENOENT;
        r = ext4_read_inode_raw(next, &ino);
        if (r != EXT4_OK) return EXT4_EIO;
        cur = next;
    }

    if (out_ino) *out_ino = cur;
    if (out_inode) *out_inode = ino;

    /* A trailing slash demands a directory ("/SUBDIR/" ok, "/HELLO.TXT/" not). */
    {
        size_t plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/' &&
            (ino.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            return EXT4_ENOTDIR;
        }
    }
    return EXT4_OK;
}

/* ==================== public file API ==================== */

int ext4_lookup(const char *path, uint32_t *out_ino) {
    if (!g_ext4.mounted) return EXT4_EBUSY;
    if (!out_ino) return EXT4_EINVAL;
    return ext4_resolve(path, out_ino, NULL);
}

int ext4_stat_path(const char *path, struct ext4_stat *out) {
    uint32_t ino;
    struct ext4_inode in;
    int r;

    if (!g_ext4.mounted) return EXT4_EBUSY;
    if (!out) return EXT4_EINVAL;
    r = ext4_resolve(path, &ino, &in);
    if (r != EXT4_OK) return r;

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
    out->is_dir = ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) ? 1 : 0;
    out->is_file = ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG) ? 1 : 0;
    out->is_symlink = ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) ? 1 : 0;
    if (in.i_size_high != 0) {
        ext4_warn1("file larger than 4 GiB; only the low 32 bits are addressable", ino);
    }
    return EXT4_OK;
}

int ext4_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size) {
    struct ext4_fs *fs = &g_ext4;
    struct ext4_inode in;
    uint8_t *scratch;
    int r;

    if (!fs->mounted) return EXT4_EBUSY;
    if (!buf && size != 0) return EXT4_EINVAL;
    r = ext4_read_inode_raw(ino, &in);
    if (r != EXT4_OK) return r;
    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return EXT4_EISDIR;

    scratch = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scratch) return EXT4_ENOMEM;
    r = ext4_inode_read(&in, buf, offset, size, scratch);
    lemis_heap_free(scratch);
    return r;
}

int ext4_readlink(const char *path, char *buf, uint32_t cap) {
    uint32_t ino;
    struct ext4_inode in;
    uint32_t n;
    int r;

    if (!g_ext4.mounted) return EXT4_EBUSY;
    if (!buf || cap == 0) return EXT4_EINVAL;
    r = ext4_resolve(path, &ino, &in);
    if (r != EXT4_OK) return r;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFLNK) return EXT4_EINVAL;
    n = in.i_size;
    if (n > cap - 1) n = cap - 1;
    if ((in.i_flags & EXT4_EXTENTS_FL) == 0 && in.i_size <= 60) {
        memcpy(buf, in.i_block, n);                  /* fast symlink: target in i_block */
    } else {
        r = ext4_read_file(ino, buf, 0, n);           /* slow symlink: target in a block */
        if (r < 0) return r;
        n = (uint32_t)r;
    }
    buf[n] = '\0';
    return (int)n;
}

int ext4_open(const char *path, struct ext4_file **out) {
    uint32_t ino;
    struct ext4_inode in;
    struct ext4_file *f = NULL;
    int i, r;

    if (!g_ext4.mounted) return EXT4_EBUSY;
    if (!out) return EXT4_EINVAL;
    r = ext4_resolve(path, &ino, &in);
    if (r != EXT4_OK) return r;

    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return EXT4_EISDIR;
    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) return EXT4_ENOTSUP;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG) return EXT4_ENOTSUP;

    for (i = 0; i < EXT4_MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) { f = &g_files[i]; break; }
    }
    if (!f) return EXT4_EBUSY;

    f->used = 1;
    f->ino = ino;
    f->size = in.i_size;
    *out = f;
    return EXT4_OK;
}

int ext4_file_read(struct ext4_file *f, void *buf, uint32_t size, uint32_t offset) {
    if (!f || !f->used) return EXT4_EINVAL;
    if (offset >= f->size) return 0;                   /* EOF */
    if (size > f->size - offset) size = f->size - offset;
    return ext4_read_file(f->ino, buf, offset, size);
}

int ext4_close(struct ext4_file *f) {
    if (!f) return EXT4_EINVAL;
    memset(f, 0, sizeof(*f));
    return EXT4_OK;
}

int ext4_opendir(const char *path, struct ext4_dir **out) {
    uint32_t ino;
    struct ext4_inode in;
    struct ext4_dir *d = NULL;
    int i, r;

    if (!g_ext4.mounted) return EXT4_EBUSY;
    if (!out) return EXT4_EINVAL;
    r = ext4_resolve(path, &ino, &in);
    if (r != EXT4_OK) return r;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return EXT4_ENOTDIR;

    for (i = 0; i < EXT4_MAX_OPEN_DIRS; i++) {
        if (!g_dirs[i].used) { d = &g_dirs[i]; break; }
    }
    if (!d) return EXT4_EBUSY;

    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->ino = ino;
    d->size = in.i_size;
    d->block = 0;
    d->offset = 0;
    d->inode = in;
    *out = d;
    return EXT4_OK;
}

/* Returns EXT4_OK for one entry, EXT4_ENOENT at the end (FAT32/ext2 convention).
 * "." and ".." are skipped. Names longer than name_cap-1 are truncated. */
int ext4_readdir(struct ext4_dir *d, char *name, uint32_t name_cap,
                 uint32_t *name_len, int *is_dir) {
    struct ext4_fs *fs = &g_ext4;
    uint32_t has_ftype;
    uint8_t *scan;
    uint8_t *data;
    int rc = EXT4_ENOENT;

    if (!d || !d->used || !name || name_cap == 0) return EXT4_EINVAL;
    if (!fs->mounted) return EXT4_EBUSY;
    has_ftype = (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;

    scan = (uint8_t *)lemis_heap_alloc(fs->block_size);
    data = (uint8_t *)lemis_heap_alloc(fs->block_size);
    if (!scan || !data) {
        if (scan) lemis_heap_free(scan);
        if (data) lemis_heap_free(data);
        return EXT4_ENOMEM;
    }

    while ((uint64_t)d->block * fs->block_size < d->size) {
        uint32_t phys = 0;
        int block_done = 0;

        rc = ext4_bmap(&d->inode, d->block, &phys, scan);
        if (rc != EXT4_OK) goto out;
        rc = EXT4_ENOENT;
        if (phys == 0) {                               /* hole: skip the block */
            d->block++;
            d->offset = 0;
            continue;
        }
        if (ext4_dev_read_block(phys, data) != EXT4_OK) { rc = EXT4_EIO; goto out; }

        while (d->offset + EXT4_DIR_ENTRY_HDR <= fs->block_size) {
            uint32_t eino = ext4_rd32(data + d->offset);
            uint32_t rec = ext4_rd16(data + d->offset + 4);
            uint32_t nlen = ext4_rd16(data + d->offset + 6);
            uint8_t ftype = data[d->offset + 7];
            uint32_t copy;
            const uint8_t *nm = data + d->offset + EXT4_DIR_ENTRY_HDR;

            if (!has_ftype) nlen &= 0xFFFFu; else nlen &= 0xFFu;
            if (rec < EXT4_DIR_ENTRY_HDR || (rec & 3u) != 0 ||
                rec > fs->block_size - d->offset || nlen > rec - EXT4_DIR_ENTRY_HDR) {
                ext4_warn2("corrupt directory entry (rec_len/name_len)", rec, d->offset);
                block_done = 1;
                break;
            }
            d->offset += rec;
            if (eino == 0) continue;
            if (nlen == 1 && nm[0] == '.') continue;
            if (nlen == 2 && nm[0] == '.' && nm[1] == '.') continue;

            copy = ext4_min_u32(nlen, name_cap - 1);
            memcpy(name, nm, copy);
            name[copy] = '\0';
            if (name_len) *name_len = copy;
            if (is_dir) {
                if (has_ftype) {
                    *is_dir = (ftype == EXT4_FT_DIR) ? 1 : 0;
                } else {
                    struct ext4_inode child;
                    if (ext4_read_inode_raw(eino, &child) != EXT4_OK) {
                        rc = EXT4_EIO;
                        goto out;
                    }
                    *is_dir = ((child.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) ? 1 : 0;
                }
            }
            rc = EXT4_OK;
            goto out;
        }
        if (block_done || d->offset + EXT4_DIR_ENTRY_HDR > fs->block_size) {
            d->block++;
            d->offset = 0;
        }
    }

out:
    if (scan) lemis_heap_free(scan);
    if (data) lemis_heap_free(data);
    return rc;
}

int ext4_closedir(struct ext4_dir *d) {
    if (!d) return EXT4_EINVAL;
    memset(d, 0, sizeof(*d));
    return EXT4_OK;
}

/* ==================== mount-time evidence (per-file extent counts) ==========
 * Reads only metadata: for every regular file found (root level and one level of
 * subdirectories) it walks the extent tree and logs
 *   [ext4] file <NAME> size=<n> extents=<n> depth=<n> leaves=<n>
 * and for every directory
 *   [ext4] dir <NAME> size=<n> extents=<n> depth=<n>
 * `emit_log == 0` only collects the deepest tree (for the mount line).
 * Bounded: at most EXT4_PROBE_MAX_ENTRIES entries per directory and at most
 * EXT4_PROBE_MAX_DIRS subdirectories, so a hostile image cannot spin here.
 */
#define EXT4_PROBE_MAX_ENTRIES 32
#define EXT4_PROBE_MAX_DIRS    8

struct ext4_probe_stat {
    uint32_t files;
    uint32_t dirs;
    uint32_t extents;
    uint32_t max_depth;
};

static int ext4_probe_one(const char *path, uint32_t ino, int emit_log,
                          struct ext4_probe_stat *st) {
    struct ext4_inode in;
    struct ext4_extent_info info;
    int r;

    r = ext4_read_inode_raw(ino, &in);
    if (r != EXT4_OK) return r;

    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
        /* Symlinks are not extent-mapped when they are "fast" (target <= 60
         * bytes lives inside i_block), and this driver does not follow them.
         * Report the stored target instead of pretending it is a data file. */
        char target[64];
        int n = ext4_readlink(path, target, sizeof(target));
        if (emit_log) {
            if (n >= 0) {
                console_printf("[ext4] link %s -> %s (target=%u bytes, fast=%u)\n",
                               path, target, (uint64_t)in.i_size,
                               (uint64_t)((in.i_flags & EXT4_EXTENTS_FL) == 0 ? 1 : 0));
            } else {
                console_printf("[ext4] link %s (target unreadable, err %d)\n",
                               path, (int64_t)n);
            }
        }
        return EXT4_OK;
    }

    r = ext4_extent_scan(&in, &info);
    if (r != EXT4_OK) {
        /* A file whose extent tree does not parse is a real finding: say so. */
        console_printf("[ext4] BAD extent tree for %s (inode %u, err %d)\n",
                       path, (uint64_t)ino, (int64_t)r);
        return r;
    }
    if (info.depth > st->max_depth) st->max_depth = info.depth;
    st->extents += info.extents;
    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        st->dirs++;
        if (emit_log) {
            console_printf("[ext4] dir %s size=%u extents=%u depth=%u\n",
                           path, (uint64_t)in.i_size, (uint64_t)info.extents,
                           (uint64_t)info.depth);
        }
    } else {
        st->files++;
        if (emit_log) {
            console_printf("[ext4] file %s size=%u extents=%u depth=%u leaves=%u\n",
                           path, (uint64_t)in.i_size, (uint64_t)info.extents,
                           (uint64_t)info.depth, (uint64_t)info.leaves);
        }
    }
    return EXT4_OK;
}

/* Probe the root directory and one level of subdirectories. */
static int ext4_probe_files(int emit_log) {
    struct ext4_probe_stat st;
    struct ext4_dir *d = NULL;
    char name[EXT4_NAME_MAX + 1];
    uint32_t nlen = 0;
    int is_dir = 0;
    int entries = 0;
    int subdirs = 0;
    int r;

    memset(&st, 0, sizeof(st));
    r = ext4_opendir("/", &d);
    if (r != EXT4_OK) {
        console_printf("[ext4] cannot open the root directory (err %d)\n", (int64_t)r);
        return r;
    }
    while (entries < EXT4_PROBE_MAX_ENTRIES &&
           ext4_readdir(d, name, sizeof(name), &nlen, &is_dir) == EXT4_OK) {
        uint32_t child_ino = 0;
        entries++;
        if (ext4_resolve(name, &child_ino, NULL) != EXT4_OK) continue;
        (void)ext4_probe_one(name, child_ino, emit_log, &st);
        if (!is_dir || subdirs >= EXT4_PROBE_MAX_DIRS) continue;
        {
            /* one level down: "<SUBDIR>/<FILE>" */
            struct ext4_dir *sub = NULL;
            char sub_path[EXT4_NAME_MAX + 3];
            char cname[EXT4_NAME_MAX + 1];
            uint32_t clen = 0;
            int cdir = 0;
            int centries = 0;

            snprintf(sub_path, sizeof(sub_path), "/%s", name);
            if (ext4_opendir(sub_path, &sub) != EXT4_OK) continue;
            subdirs++;
            while (centries < EXT4_PROBE_MAX_ENTRIES &&
                   ext4_readdir(sub, cname, sizeof(cname), &clen, &cdir) == EXT4_OK) {
                char cpath[2 * EXT4_NAME_MAX + 3];
                uint32_t cino = 0;
                centries++;
                snprintf(cpath, sizeof(cpath), "%s/%s", name, cname);
                if (ext4_resolve(cpath, &cino, NULL) != EXT4_OK) continue;
                (void)ext4_probe_one(cpath, cino, emit_log, &st);
            }
            ext4_closedir(sub);
        }
    }
    ext4_closedir(d);

    if (st.max_depth > g_ext4.max_depth_seen) g_ext4.max_depth_seen = st.max_depth;
    if (emit_log) {
        console_printf("[ext4] probe: files=%u dirs=%u extents=%u max_depth=%u\n",
                       (uint64_t)st.files, (uint64_t)st.dirs, (uint64_t)st.extents,
                       (uint64_t)st.max_depth);
    }
    return EXT4_OK;
}
