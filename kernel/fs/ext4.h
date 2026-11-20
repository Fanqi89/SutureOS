/* SutureOS: ext4 READ-ONLY filesystem driver (extent-tree volumes)
 *
 * Provenance / licence: written for SutureOS. No third-party source code was
 * copied. The on-disk layout implemented here is the public ext4 specification
 * from the Linux kernel documentation:
 *   https://www.kernel.org/doc/html/latest/filesystems/ext4/
 * (superblock, group descriptors, inodes, extent tree, directory entries).
 * The driver's *shape* (block-layer plumbing, heap scratch buffers, single
 * volume context, error-code enum, VFS adapter layout) intentionally follows
 * kernel/fs/ext2.c / ext2_vfs.c in this same tree so the two drivers stay
 * reviewable side by side; those files were not modified.
 *
 * SCOPE (honest):
 *   - READ-ONLY. Every mutating VFS operation returns VFS_EPERM.
 *   - supported incompatible features: extents (0x0040), filetype (0x0002).
 *     Anything else (64bit, meta_bg, mmp, bigalloc, inline_data, journal device,
 *     needs-recovery, ...) is REFUSED at mount with the offending bits printed
 *     by name - the driver never guesses what an unknown feature means.
 *   - supported read-only-compatible features: sparse_super (0x0001). Unknown
 *     ro_compat bits are accepted (that is exactly what ro_compat means) but
 *     are listed in a mount note.
 *   - directory data must be linear: the compat dir_index (hashed directory)
 *     feature is refused, because a dx_root block would otherwise be silently
 *     misread as an almost-empty linear directory.
 *   - block sizes 1024 / 2048 / 4096, inode sizes 128..block_size,
 *     extent trees up to depth 4, unwritten extents are read as zeroes,
 *     files with a hole (a logical block no extent covers) read as zeroes.
 *   - file size is the low 32 bits of i_size (i_size_high is ignored): files
 *     larger than 4 GiB are not addressable - same limit as the ext2 driver.
 *   - symbolic links are NOT followed: stat reports VFS_NODE_SYMLINK, open of a
 *     symlink fails with EXT4_ENOTSUP. (A fast symlink target is still exposed
 *     through ext4_stat_path()/the driver for reporting, and the image fixture
 *     contains one so the dirent file_type path is exercised.)
 *   - no xattr, no ACL, no encryption, no inline data, no journal replay,
 *     no multi-volume support (one ext4 volume at a time), no write path.
 */
#ifndef STITCH_EXT4_H
#define STITCH_EXT4_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== on-disk constants ==================== */

#define EXT4_SUPER_MAGIC        0xEF53u
#define EXT4_SUPERBLOCK_OFFSET  1024u
#define EXT4_SUPERBLOCK_SIZE    1024u
#define EXT4_SECTOR_SIZE        512u
#define EXT4_INODE_BASE_SIZE    128u
#define EXT4_BLOCK_PTRS         15u
#define EXT4_NAME_MAX           255u
#define EXT4_MAX_GROUPS         1024u
#define EXT4_MAX_BLOCK_SIZE     4096u
#define EXT4_DIR_ENTRY_HDR      8u
#define EXT4_EXTENT_MAGIC       0xF30Au
#define EXT4_EXTENT_MAX_DEPTH   4u
#define EXT4_EXTENT_HDR_SIZE    12u
#define EXT4_EXTENT_ENTRY_SIZE  12u

/* i_flags: the inode body holds an extent tree instead of block pointers */
#define EXT4_EXTENTS_FL         0x00080000u

/* s_feature_incompat */
#define EXT4_FEATURE_INCOMPAT_FILEtype_ 0x0002u   /* (kept internal name sane) */
#define EXT4_FEATURE_INCOMPAT_FILETYPE  0x0002u
#define EXT4_FEATURE_INCOMPAT_EXTENTS   0x0040u
#define EXT4_FEATURE_INCOMPAT_SUPPORTED \
    (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS)

/* s_feature_compat (bits that change the on-disk meaning of data we read) */
#define EXT4_FEATURE_COMPAT_DIR_INDEX   0x0020u
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004u

/* s_feature_ro_compat */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u

/* i_mode file type mask / values */
#define EXT4_S_IFMT     0xF000u
#define EXT4_S_IFSOCK   0xC000u
#define EXT4_S_IFLNK    0xA000u
#define EXT4_S_IFREG    0x8000u
#define EXT4_S_IFBLK    0x6000u
#define EXT4_S_IFDIR    0x4000u
#define EXT4_S_IFCHR    0x2000u
#define EXT4_S_IFIFO    0x1000u

/* directory entry file types (ft_*) - only meaningful with the filetype feature */
#define EXT4_FT_UNKNOWN 0u
#define EXT4_FT_REG     1u
#define EXT4_FT_DIR     2u
#define EXT4_FT_CHRDEV  3u
#define EXT4_FT_BLKDEV  4u
#define EXT4_FT_FIFO    5u
#define EXT4_FT_SOCK    6u
#define EXT4_FT_SYMLINK 7u

/* ==================== error codes ==================== */

typedef enum ext4_err {
    EXT4_OK        = 0,
    EXT4_ERR       = -1,   /* generic failure */
    EXT4_ENOENT    = -2,
    EXT4_ENOTDIR   = -3,
    EXT4_EISDIR    = -4,
    EXT4_EINVAL    = -5,
    EXT4_EIO       = -6,
    EXT4_EROFS     = -7,   /* read-only filesystem */
    EXT4_ENOMEM    = -8,
    EXT4_ENOTSUP   = -9,
    EXT4_EBUSY     = -10,
    EXT4_EEXIST    = -11,
    EXT4_ENOTEMPTY = -12,
    EXT4_ENOSPC    = -13,
    EXT4_ECORRUPT  = -14,  /* on-disk structure failed validation */
} ext4_err_t;

/* ==================== on-disk structures (packed, byte-exact) ==================== */

/* The 256-byte ext4 superblock prefix. Field order/size is the ext4 spec; the
 * driver validates every value it uses (never trusts the disk). */
struct ext4_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
} __attribute__((packed));

/* 32-byte group descriptor (the 64byte variant needs the 64bit feature, refused) */
struct ext4_block_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap;
    uint16_t bg_block_bitmap_csum;
    uint16_t bg_inode_bitmap_csum;
    uint16_t bg_itable_unused;
    uint16_t bg_checksum;
} __attribute__((packed));

/* Inode prefix (128 bytes = the ext2 inode; the 256-byte ext4 inode only adds
 * extra fields after it, which a read-only driver does not need). */
struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT4_BLOCK_PTRS];   /* extent tree root when EXT4_EXTENTS_FL */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

struct ext4_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed));

/* Extent tree: a 12-byte header followed by `eh_entries` 12-byte items.
 * depth 0 items are ext4_extent, depth > 0 items are ext4_extent_idx. */
struct ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed));

struct ext4_extent {
    uint32_t ee_block;      /* first logical block this extent covers */
    uint16_t ee_len;        /* blocks covered; > 32768 => unwritten extent */
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
    uint32_t ei_block;      /* logical block this index node starts at */
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed));

/* Compile-time proof that the packed structs match the on-disk layout. */
_Static_assert(sizeof(struct ext4_superblock) == 256, "ext4 superblock prefix layout");
_Static_assert(sizeof(struct ext4_block_group_desc) == 32, "ext4 group descriptor layout");
_Static_assert(sizeof(struct ext4_inode) == EXT4_INODE_BASE_SIZE, "ext4 inode prefix layout");
_Static_assert(sizeof(struct ext4_dir_entry) == 8, "ext4 dirent header layout");
_Static_assert(sizeof(struct ext4_extent_header) == 12, "ext4 extent header layout");
_Static_assert(sizeof(struct ext4_extent) == 12, "ext4 extent layout");
_Static_assert(sizeof(struct ext4_extent_idx) == 12, "ext4 extent index layout");

/* ==================== volume context (single volume) ==================== */

/* Statistics gathered while walking extent trees; used by the mount-time file
 * probe log and by callers that want to know how deep a tree really is. */
struct ext4_extent_info {
    uint32_t extents;    /* number of leaf extents (depth-0 entries) */
    uint32_t leaves;     /* number of index entries followed (tree nodes) */
    uint32_t depth;      /* maximum depth seen */
    uint32_t blocks;     /* logical blocks covered by initialized extents */
    uint32_t holes;      /* uncovered logical blocks before EOF (sparse) */
};

struct ext4_fs {
    int      drive;              /* block-device number */
    uint32_t start_lba;          /* partition offset, 512-byte sectors */

    /* geometry, derived from the superblock and validated against the device */
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t blocks_count;       /* effective count (clamped to the device) */
    uint32_t blocks_count_raw;
    uint32_t inodes_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t first_data_block;
    uint32_t first_ino;
    uint32_t group_count;
    uint32_t gdt_block;
    uint32_t gdt_entries_per_block;
    uint32_t max_depth_seen;     /* deepest extent tree walked on this volume */

    struct ext4_superblock sb;

    struct ext4_block_group_desc *bgdt;   /* heap, group_count entries */
    uint32_t bgdt_capacity;

    int      mounted;
};

/* ==================== stat / handles ==================== */

struct ext4_stat {
    uint32_t ino;
    uint32_t mode;
    uint32_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t links;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    int      is_dir;
    int      is_file;
    int      is_symlink;
};

struct ext4_file {
    int      used;
    uint32_t ino;
    uint32_t size;
};

struct ext4_dir {
    int      used;
    uint32_t ino;
    uint32_t size;                    /* i_size of the directory inode */
    uint32_t block;                   /* current logical block index */
    uint32_t offset;                  /* byte offset inside that block */
    struct ext4_inode inode;          /* cached at opendir() */
};

/* ==================== lifecycle ==================== */

/* Read the superblock and print one line: either the volume geometry (it IS an
 * ext4 volume this driver can mount) or "[ext4] no ext4 volume on drive N".
 * Never mounts, never allocates beyond one block, returns 1 when a mountable
 * ext4 volume is present and 0 otherwise. Used by the boot probe. */
int ext4_probe(int drive);

/* Mount the ext4 volume on `drive` at `start_lba` read-only.
 * Returns EXT4_OK or a negative EXT4_* error; a failed mount leaves no state. */
int ext4_mount(int drive, uint32_t start_lba);
int ext4_unmount(void);
int ext4_is_mounted(void);
uint32_t ext4_block_size(void);
int ext4_volume_drive(void);
uint32_t ext4_max_depth_seen(void);
uint32_t ext4_blocks_count(void);
uint32_t ext4_free_blocks_count(void);

/* ==================== block / inode access ==================== */

int ext4_read_block_raw(uint32_t block, void *buf);
int ext4_read_inode(uint32_t ino, struct ext4_inode *out);

/* Walk the extent tree of an inode and fill `info` (extent/leaf counts, depth,
 * covered blocks, holes). Returns EXT4_OK or a negative error on a corrupt tree. */
int ext4_extent_scan(const struct ext4_inode *in, struct ext4_extent_info *info);

/* ==================== path operations (volume-relative: "/SUBDIR/A.TXT") ==================== */

int ext4_lookup(const char *path, uint32_t *out_ino);
int ext4_stat_path(const char *path, struct ext4_stat *out);

/* Fast symlink target (i_block holds it when i_size <= 60 and there is no
 * EXTENTS_FL). Returns the length or a negative error. Symlinks are not
 * followed; this only exposes the stored target text. */
int ext4_readlink(const char *path, char *buf, uint32_t cap);

/* Read `size` bytes at `offset` from inode `ino`. Returns bytes read (>= 0,
 * short only at EOF) or a negative ext4_err_t. Holes read as zeroes. */
int ext4_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size);

int ext4_open(const char *path, struct ext4_file **out);
int ext4_file_read(struct ext4_file *f, void *buf, uint32_t size, uint32_t offset);
int ext4_close(struct ext4_file *f);

/* Directory iteration: readdir() returns EXT4_OK per entry and EXT4_ENOENT at
 * the end; "." and ".." are skipped (same convention as the ext2/FAT32 drivers). */
int ext4_opendir(const char *path, struct ext4_dir **out);
int ext4_readdir(struct ext4_dir *d, char *name, uint32_t name_cap,
                 uint32_t *name_len, int *is_dir);
int ext4_closedir(struct ext4_dir *d);

/* ==================== write API (all refused) ==================== */

/* Every mutating entry point exists so callers get a precise error instead of a
 * missing symbol, and every one of them returns EXT4_EROFS. */
int ext4_write_refused(const char *what);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_EXT4_H */
