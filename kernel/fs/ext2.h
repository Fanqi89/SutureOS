/* SutureOS: ext2 (revision 0 / revision 1) READ-ONLY filesystem driver
 *
 * Ported/adapted from: Cinux kernel/fs/ext2_*.cpp + libs/ext2 (MIT)
 *   C:\Users\fanqi\Desktop\自研操作系统\Cinux\libs\ext2
 * Adaptations for SutureOS:
 *   - SutureOS block layer (blk_read from kernel/drivers/block.h) plus a RAM
 *     disk path (ext2_register_ramdisk) so a test image can live in kernel RAM
 *   - no C++/STL/mutex: single-volume context, per-operation heap scratch
 *     (lemis_heap_alloc), console_printf("[ext2] ...") logging
 *   - triple-indirect block pointers (Cinux stops at double indirect)
 *   - sparse holes zero-fill, on-disk value validation, bounded traversal
 *   - read-only: no write/mkdir/unlink path at all
 *
 * Scope (honest):
 *   - ext2 rev0/rev1 with classic block pointers; 1 KiB / 2 KiB / 4 KiB blocks
 *   - extents (EXT4_FEATURE_INCOMPAT_EXTENTS), 64-bit, meta_bg, journal and
 *     MMP filesystems are REJECTED at mount (unsupported incompat features)
 *   - symlink *targets* are not resolved (stat reports a symlink, open fails)
 *   - no write support: write/mkdir/rmdir/unlink/rename all fail read-only
 */
#ifndef STITCH_EXT2_H
#define STITCH_EXT2_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== on-disk constants ==================== */

#define EXT2_SUPER_MAGIC        0xEF53u
#define EXT2_SUPERBLOCK_OFFSET  1024u
#define EXT2_SUPERBLOCK_SIZE    1024u
#define EXT2_SECTOR_SIZE        512u
#define EXT2_INODE_SIZE_REV0    128u
#define EXT2_DIRECT_BLOCKS      12u
#define EXT2_INDIRECT_BLOCK     12u   /* i_block[12]: single indirect */
#define EXT2_DOUBLE_INDIRECT    13u   /* i_block[13]: double indirect */
#define EXT2_TRIPLE_INDIRECT    14u   /* i_block[14]: triple indirect */
#define EXT2_BLOCK_PTRS         15u
#define EXT2_NAME_MAX           255u
#define EXT2_MAX_GROUPS         1024u /* bgdt is heap-allocated (1024*32 = 32 KiB) */
#define EXT2_MAX_BLOCK_SIZE     4096u
#define EXT2_DIR_ENTRY_HDR      8u

#define EXT2_VALID_FS           0x0001u
#define EXT2_ERROR_FS           0x0002u

/* s_feature_incompat bits this driver understands / refuses */
#define EXT2_FEATURE_INCOMPAT_FILETYPE  0x0002u
#define EXT2_FEATURE_INCOMPAT_UNSUPPORTED \
    (0x0004u | 0x0008u | 0x0010u | 0x0040u | 0x0080u | 0x0100u)
/* 0x0004 journal needs recovery, 0x0008 journal device, 0x0010 meta_bg,
 * 0x0040 extents, 0x0080 64-bit, 0x0100 MMP */

/* i_mode file type mask / values */
#define EXT2_S_IFMT     0xF000u
#define EXT2_S_IFSOCK   0xC000u
#define EXT2_S_IFLNK    0xA000u
#define EXT2_S_IFREG    0x8000u
#define EXT2_S_IFBLK    0x6000u
#define EXT2_S_IFDIR    0x4000u
#define EXT2_S_IFCHR    0x2000u
#define EXT2_S_IFIFO    0x1000u

/* directory entry file types (ft_*) */
#define EXT2_FT_UNKNOWN 0u
#define EXT2_FT_REG     1u
#define EXT2_FT_DIR     2u
#define EXT2_FT_CHRDEV  3u
#define EXT2_FT_BLKDEV  4u
#define EXT2_FT_FIFO    5u
#define EXT2_FT_SOCK    6u
#define EXT2_FT_SYMLINK 7u

/* ==================== error codes ==================== */

typedef enum ext2_err {
    EXT2_OK       = 0,
    EXT2_ERR      = -1,   /* generic failure */
    EXT2_ENOENT   = -2,
    EXT2_ENOTDIR  = -3,
    EXT2_EISDIR   = -4,
    EXT2_EINVAL   = -5,
    EXT2_EIO      = -6,
    EXT2_EROFS    = -7,   /* read-only filesystem */
    EXT2_ENOMEM   = -8,
    EXT2_ENOTSUP  = -9,
    EXT2_EBUSY    = -10,
} ext2_err_t;

/* ==================== on-disk structures (packed, byte-exact) ==================== */

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
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
    /* ---- revision 1 fields ---- */
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

struct ext2_block_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
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
    uint32_t i_block[EXT2_BLOCK_PTRS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

/* Directory entry header; the name follows immediately (not NUL terminated). */
struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed));

/* Compile-time proof that the packed structs match the on-disk layout. */
_Static_assert(sizeof(struct ext2_superblock) == 256, "ext2 superblock prefix layout");
_Static_assert(sizeof(struct ext2_block_group_desc) == 32, "ext2 group descriptor layout");
_Static_assert(sizeof(struct ext2_inode) == 128, "ext2 inode layout");
_Static_assert(sizeof(struct ext2_dir_entry) == 8, "ext2 dirent header layout");

/* ==================== volume context ==================== */

struct ext2_fs {
    /* device */
    int      drive;              /* block-device number for the block path */
    uint32_t start_lba;          /* partition offset, 512-byte sectors */
    const uint8_t *ram;          /* RAM image base (ramdisk path), NULL = none */
    uint32_t ram_size;           /* RAM image size in bytes */

    /* geometry (derived from the superblock, validated against the device) */
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t blocks_count;       /* EFFECTIVE count, clamped to the device */
    uint32_t blocks_count_raw;   /* value found in the superblock */
    uint32_t inodes_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t group_count;
    uint32_t ptrs_per_block;     /* block_size / 4 */
    uint32_t max_logical;        /* direct+single+double+triple addressable */
    uint32_t rev_level;          /* 0 or 1 (validated at mount) */

    /* superblock copy */
    struct ext2_superblock sb;

    /* block group descriptor table (heap, group_count entries) */
    struct ext2_block_group_desc *bgdt;
    uint32_t bgdt_capacity;

    int      mounted;
};

extern struct ext2_fs g_ext2_fs;

/* ==================== stat / handles ==================== */

struct ext2_stat {
    uint32_t ino;
    uint32_t mode;      /* full i_mode (type bits + permission bits) */
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

struct ext2_file {
    int      used;
    uint32_t ino;
    uint32_t size;
};

struct ext2_dir {
    int      used;
    uint32_t ino;
    uint32_t size;       /* i_size of the directory inode */
    uint32_t block;      /* current logical block index */
    uint32_t offset;     /* byte offset inside that logical block */
    struct ext2_inode inode;  /* cached at opendir(); the volume is read-only */
};

/* ==================== lifecycle ==================== */

/* Mount the ext2 volume on a block device at start_lba (real partition path). */
int ext2_mount(int drive, uint32_t start_lba);

/* Mount the RAM image previously handed to ext2_register_ramdisk(). */
int ext2_mount_ramdisk(void);

/* Register (not mount) a RAM-resident ext2 image. The caller keeps the buffer
 * alive for as long as the volume stays mounted; nothing is copied. */
int ext2_register_ramdisk(const void *image, uint32_t size);

int ext2_unmount(void);
int ext2_is_mounted(void);
int ext2_ramdisk_registered(void);
uint32_t ext2_block_size(void);

/* ==================== block / inode access ==================== */

/* Read one whole filesystem block (block_size bytes) into buf. */
int ext2_read_block_raw(uint32_t block, void *buf);
int ext2_read_inode(uint32_t ino, struct ext2_inode *out);

/* ==================== path operations (volume-relative, e.g. "/SUB/A.TXT") ==================== */

int ext2_lookup(const char *path, uint32_t *out_ino);
int ext2_stat(const char *path, struct ext2_stat *out);

/* Read `size` bytes at `offset` from inode `ino`. Returns bytes read (>= 0,
 * short only at EOF) or a negative ext2_err_t. Holes are zero-filled. */
int ext2_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size);

/* Open/read/close a regular file. Handles come from a small static pool. */
int ext2_open(const char *path, struct ext2_file **out);
int ext2_file_read(struct ext2_file *f, void *buf, uint32_t size, uint32_t offset);
int ext2_close(struct ext2_file *f);

/* Directory iteration. readdir() returns EXT2_OK per entry and EXT2_ENOENT at
 * the end; "." and ".." are skipped (same convention as the FAT32 driver). */
int ext2_opendir(const char *path, struct ext2_dir **out);
int ext2_readdir(struct ext2_dir *d, char *name, uint32_t name_cap,
                 uint32_t *name_len, int *is_dir);
int ext2_closedir(struct ext2_dir *d);

/* ==================== helpers ==================== */

/* Standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) - same value as
 * Python's zlib.crc32(), so the host can cross-check guest reads. */
uint32_t ext2_crc32(const void *data, uint32_t len);

/* Mount the registered ramdisk if needed, read every test file, verify the
 * documented patterns/contents and print
 *   [ext2] selftest PASS (hello=0x.. big=0x.. nested=0x.. sparse=0x.. double=0x..)
 * or
 *   [ext2] selftest FAIL (<reason>)
 * Returns EXT2_OK / EXT2_ERR. */
int ext2_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_EXT2_H */
