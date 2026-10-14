/* SutureOS: real SquashFS 4.0 READ-ONLY filesystem driver (COMPRESSOR_GZIP)
 *
 * Upstream references consulted while writing this (all read-only, none
 * modified):
 *   - SquashFS 4.0 on-disk layout: the official squashfs-tools-ng headers
 *     bundled as a reference implementation in
 *       C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\squashfs\
 *         include\sqfs\super.h      (struct sqfs_super_t, SQFS_MAGIC, flags)
 *         include\sqfs\inode.h      (sqfs_inode_t + basic/extended inode payloads)
 *         include\sqfs\dir.h        (sqfs_dir_header_t / sqfs_dir_node_t)
 *         include\sqfs\compressor.h (SQFS_COMPRESSOR ids)
 *         src\read_super.c, src\read_inode.c, src\read_table.c,
 *         src\dir_reader.c, src\data_reader.c, src\meta_reader.c
 *     (LGPL-3.0-or-later / GPL-3.0, David Oberhollenzer - format facts only:
 *      struct field order, sizes and the metadata-block header encoding)
 *   - CoolPotOS's own VFS wrapper for the same library:
 *       C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\squashfs\squashfs.c
 *       C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\squashfs\squashfs.h
 *     (MIT wrapper; the pre-existing stub in this tree was ported from it)
 *   - The pre-existing SutureOS stub kernel/fs/squashfs.c/.h (prefix names and
 *     the legacy entry points that kernel/fs/squashfs_vfs.c / shell.c use)
 *
 * Adaptations for SutureOS:
 *   - SutureOS block layer (kernel/drivers/block.h blk_read/blk_get_info) plus
 *     a RAM-disk path (squashfs_register_ramdisk) for in-RAM test images
 *   - no libc / no zlib: gzip blocks are inflated by the RFC 1951 inflate that
 *     kernel/pkg/deb.c used to own, now shared as kernel/pkg/inflate.c
 *     (single copy in the tree - see inflate.h)
 *   - bounded everything: table offset arrays are heap-allocated and capped,
 *     metadata/data caches are single reusable buffers, no unbounded loops
 *   - READ-ONLY: there is no write path at all
 *
 * Scope (honest):
 *   - SquashFS 4.0, version 4.0, COMPRESSOR_GZIP only. LZO/LZMA/XZ/LZ4/ZSTD
 *     images are REJECTED at mount with an explicit reason.
 *   - basic AND extended inodes (dir/file/symlink), fragments, sparse blocks
 *     (a data-block list entry of 0 = a whole block of zeros), symlink targets
 *     (fast symlink with an inline target; extended symlink with an out-of-line
 *     target), multi-block files, zero-length files, nested directories.
 *   - files larger than SQUASHFS_MAX_FILE_BLOCKS blocks are REFUSED (open/read
 *     returns ENOTSUP) rather than silently truncated.
 *   - extended attribute / export (NFS lookup) tables are NOT parsed: xattrs
 *     are ignored, directory lookups use the real directory table only.
 *   - no write, no mkdir/unlink/rename; every mutating entry point fails.
 *   - out-of-order/dirty images: SquashFS has no journal or dirty flag, so any
 *     image whose tables/pointers are inconsistent is rejected or reported per
 *     operation (never silently mis-read).
 */
#ifndef STITCH_SQUASHFS_H
#define STITCH_SQUASHFS_H

#include <stitch/types.h>
#include <stitch/console.h>
#include "vfs64.h"     /* struct Vfs64Info64, used by the legacy stat entry point */

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== on-disk constants (SquashFS 4.0) ==================== */

#define SQUASHFS_MAGIC        0x73717368u   /* "hsqs" little-endian == 's','q','s','h' */
#define SQUASHFS_VERSION_MAJOR 4u
#define SQUASHFS_VERSION_MINOR 0u

/* compression ids (superblock s_compression) */
#define SQUASHFS_COMP_GZIP    1
#define SQUASHFS_COMP_LZO     2
#define SQUASHFS_COMP_LZMA    3
#define SQUASHFS_COMP_XZ      4
#define SQUASHFS_COMP_LZ4     5
#define SQUASHFS_COMP_ZSTD    6

/* allowed data block size range, and the fixed metadata block size */
#define SQUASHFS_MIN_BLOCK_SIZE   4096u
#define SQUASHFS_MAX_BLOCK_SIZE   1048576u
#define SQUASHFS_METADATA_SIZE    8192u

/* metadata block header: bit 15 set == the block is stored UNCOMPRESSED */
#define SQUASHFS_COMPRESSED_BIT       0x8000u
/* data/fragment block-list entry: bit 24 set == the block is UNCOMPRESSED */
#define SQUASHFS_COMPRESSED_BIT_BLOCK 0x01000000u
#define SQUASHFS_BLOCK_SIZE_MASK      0x00FFFFFFu

/* superblock flags */
#define SQUASHFS_FLAG_UNCOMPRESSED_INODES    0x0001u
#define SQUASHFS_FLAG_UNCOMPRESSED_DATA      0x0002u
#define SQUASHFS_FLAG_CHECK                  0x0004u
#define SQUASHFS_FLAG_UNCOMPRESSED_FRAGMENTS 0x0008u
#define SQUASHFS_FLAG_NO_FRAGMENTS           0x0010u
#define SQUASHFS_FLAG_ALWAYS_FRAGMENTS       0x0020u
#define SQUASHFS_FLAG_NO_DUPLICATES          0x0040u
#define SQUASHFS_FLAG_EXPORTABLE             0x0080u
#define SQUASHFS_FLAG_UNCOMPRESSED_XATTRS    0x0100u
#define SQUASHFS_FLAG_NO_XATTRS              0x0200u
#define SQUASHFS_FLAG_COMPRESSOR_OPTIONS     0x0400u
#define SQUASHFS_FLAG_UNCOMPRESSED_IDS       0x0800u

/* inode types (the 16 bits at the start of every inode) */
#define SQUASHFS_INODE_DIR            1
#define SQUASHFS_INODE_FILE           2
#define SQUASHFS_INODE_SYMLINK        3
#define SQUASHFS_INODE_BLKDEV         4
#define SQUASHFS_INODE_CHRDEV         5
#define SQUASHFS_INODE_FIFO           6
#define SQUASHFS_INODE_SOCKET         7
#define SQUASHFS_INODE_EXT_DIR        8
#define SQUASHFS_INODE_EXT_FILE       9
#define SQUASHFS_INODE_EXT_SYMLINK   10
#define SQUASHFS_INODE_EXT_BLKDEV    11
#define SQUASHFS_INODE_EXT_CHRDEV    12
#define SQUASHFS_INODE_EXT_FIFO      13
#define SQUASHFS_INODE_EXT_SOCKET    14

/* directory entry type base value (entry type 3 == SQUASHFS_INODE_DIR) */
#define SQUASHFS_DIR_TYPE_OFFSET      3u

#define SQUASHFS_INVALID_FRAG         0xFFFFFFFFu   /* fragment_index "no fragment" */
#define SQUASHFS_INVALID_BLK          0xFFFFFFFFu   /* fragment_offset "no fragment" */

/* Path/name limits. */
#define SQUASHFS_NAME_MAX  256u
#define SQUASHFS_PATH_MAX  512u

/* Bound on the number of data blocks a single file may have (and therefore on
 * the inode block-list scratch buffer: 4096 * 4 = 16 KiB). 4096 blocks is
 * 16 MiB at 4 KiB blocks and 512 MiB at the 128 KiB default. */
#define SQUASHFS_MAX_FILE_BLOCKS 4096u
/* Bound on the metadata-block offset arrays built at mount time (8192 blocks
 * is 64 MiB of metadata, far past anything this driver needs to support). */
#define SQUASHFS_MAX_TABLE_BLOCKS 8192u
/* Bound on the fragment table entries held in .bss (32 KiB). */
#define SQUASHFS_MAX_FRAGMENTS 4096u
/* Bound on directory entries walked for one directory (a 100 KiB directory
 * listing fits in ~7000 entries, so this is generous). */
#define SQUASHFS_MAX_DIR_ENTRIES 65536u
/* Open-handle pools, same shape as the ext2 driver. */
#define SQUASHFS_MAX_OPEN_FILES 8
#define SQUASHFS_MAX_OPEN_DIRS  8
/* Warnings are budgeted so a corrupt image cannot flood the serial log. */
#define SQUASHFS_WARN_BUDGET 32u

/* ==================== error codes ==================== */

typedef enum squashfs_err {
    SQUASHFS_OK       = 0,
    SQUASHFS_ERR      = -1,   /* generic failure */
    SQUASHFS_ENOENT   = -2,
    SQUASHFS_ENOTDIR  = -3,
    SQUASHFS_EISDIR   = -4,
    SQUASHFS_EINVAL   = -5,
    SQUASHFS_EIO      = -6,
    SQUASHFS_EROFS    = -7,   /* read-only filesystem */
    SQUASHFS_ENOMEM   = -8,
    SQUASHFS_ENOTSUP  = -9,
    SQUASHFS_EBUSY    = -10,
    SQUASHFS_ERANGE   = -11,  /* on-disk value outside the image */
} squashfs_err_t;

/* ==================== on-disk structures (packed, byte-exact) ==================== */

struct squashfs_super {
    uint32_t magic;                  /*  0 */
    uint32_t inode_count;            /*  4 */
    uint32_t mkfs_time;              /*  8 */
    uint32_t block_size;             /* 12 */
    uint32_t fragment_entry_count;   /* 16 */
    uint16_t compression;            /* 20 */
    uint16_t block_log;              /* 22 */
    uint16_t flags;                  /* 24 */
    uint16_t id_count;               /* 26 */
    uint16_t version_major;          /* 28 */
    uint16_t version_minor;          /* 30 */
    uint64_t root_inode;             /* 32: (block<<16)|offset into the inode table */
    uint64_t bytes_used;             /* 40 */
    uint64_t id_table_start;         /* 48 */
    uint64_t xattr_id_table_start;   /* 56 */
    uint64_t inode_table_start;      /* 64 */
    uint64_t directory_table_start;  /* 72 */
    uint64_t fragment_table_start;   /* 80 */
    uint64_t lookup_table_start;     /* 88 */
} __attribute__((packed));

/* 8-byte compressor options block that follows the superblock when
 * SQUASHFS_FLAG_COMPRESSOR_OPTIONS is set (gzip: 4 bytes of "options",
 * 4 bytes of "flags"). Read and validated, not otherwise used. */
struct squashfs_comp_options {
    uint32_t options;
    uint32_t flags;
} __attribute__((packed));

/* common inode header, fixed 16 bytes */
struct squashfs_base_inode {
    uint16_t inode_type;   /* SQUASHFS_INODE_* */
    uint16_t mode;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
} __attribute__((packed));

/* payload of a basic directory inode (SQUASHFS_INODE_DIR) */
struct squashfs_reg_dir_inode {
    uint32_t start_block;  /* offset from the directory table start */
    uint32_t nlink;
    uint16_t size;         /* bytes of listing, off by +3 */
    uint16_t offset;       /* byte offset inside the uncompressed start block */
    uint32_t parent_inode;
} __attribute__((packed));

struct squashfs_lreg_dir_inode {   /* SQUASHFS_INODE_EXT_DIR */
    uint32_t nlink;
    uint32_t size;
    uint32_t start_block;
    uint32_t parent_inode;
    uint16_t i_count;      /* directory index entries following the inode */
    uint16_t offset;
    uint32_t xattr_idx;
} __attribute__((packed));

struct squashfs_reg_file_inode {   /* SQUASHFS_INODE_FILE */
    uint32_t blocks_start; /* absolute byte offset of the first data block */
    uint32_t fragment_idx; /* SQUASHFS_INVALID_FRAG == none */
    uint32_t fragment_offset;
    uint32_t file_size;
    /* uint32_t block_sizes[] follows (block_count entries) */
} __attribute__((packed));

struct squashfs_lreg_file_inode {  /* SQUASHFS_INODE_EXT_FILE */
    uint64_t blocks_start;
    uint64_t file_size;
    uint64_t sparse;
    uint32_t nlink;
    uint32_t fragment_idx;
    uint32_t fragment_offset;
    uint32_t xattr_idx;
    /* uint32_t block_sizes[] follows (block_count entries) */
} __attribute__((packed));

struct squashfs_symlink_inode {    /* SQUASHFS_INODE_SYMLINK */
    uint32_t nlink;
    uint32_t target_size;
    /* target bytes follow inline */
} __attribute__((packed));

struct squashfs_lsymlink_inode {   /* SQUASHFS_INODE_EXT_SYMLINK */
    uint32_t nlink;
    uint32_t target_size;
    uint8_t  target[256];  /* inline when target_size < 256 */
    /* otherwise the target lives at the absolute offset stored in `target` */
} __attribute__((packed));

struct squashfs_dev_inode {        /* BLKDEV / CHRDEV */
    uint32_t nlink;
    uint32_t rdev;
} __attribute__((packed));

struct squashfs_ldev_inode {       /* EXT_BLKDEV / EXT_CHRDEV */
    uint32_t nlink;
    uint32_t rdev;
    uint32_t xattr_idx;
} __attribute__((packed));

struct squashfs_ipc_inode {        /* FIFO / SOCKET */
    uint32_t nlink;
} __attribute__((packed));

struct squashfs_lipc_inode {       /* EXT_FIFO / EXT_SOCKET */
    uint32_t nlink;
    uint32_t xattr_idx;
} __attribute__((packed));

/* one entry in the directory table */
struct squashfs_dir_header {
    uint32_t count;        /* number of entries following, off by -1 */
    uint32_t start_block;  /* inode metadata block, relative to inode_table_start */
    uint32_t inode_number; /* inode number of the first entry */
} __attribute__((packed));

struct squashfs_dir_entry {
    uint16_t offset;       /* into the uncompressed inode metadata block */
    int16_t  inode_diff;   /* signed delta from the header's inode_number */
    uint16_t type;         /* SQUASHFS_INODE_* */
    uint16_t size;         /* name length, off by -1 */
    /* char name[] follows, NOT NUL terminated */
} __attribute__((packed));

/* fragment table entry */
struct squashfs_fragment_entry {
    uint64_t start_block;  /* absolute byte offset of the fragment block */
    uint32_t size;         /* on-disk size, bit 24 set == uncompressed, 0 == hole */
    uint32_t unused;
} __attribute__((packed));

/* Compile-time proof that the packed structs match the on-disk layout. */
_Static_assert(sizeof(struct squashfs_super) == 96, "squashfs superblock layout");
_Static_assert(sizeof(struct squashfs_base_inode) == 16, "squashfs base inode layout");
_Static_assert(sizeof(struct squashfs_reg_dir_inode) == 16, "squashfs basic dir inode layout");
_Static_assert(sizeof(struct squashfs_lreg_dir_inode) == 24, "squashfs ext dir inode layout");
_Static_assert(sizeof(struct squashfs_reg_file_inode) == 16, "squashfs basic file inode layout");
_Static_assert(sizeof(struct squashfs_lreg_file_inode) == 40, "squashfs ext file inode layout");
_Static_assert(sizeof(struct squashfs_symlink_inode) == 8, "squashfs symlink inode layout");
_Static_assert(sizeof(struct squashfs_dir_header) == 12, "squashfs dir header layout");
_Static_assert(sizeof(struct squashfs_dir_entry) == 8, "squashfs dir entry layout");
_Static_assert(sizeof(struct squashfs_fragment_entry) == 16, "squashfs fragment entry layout");
_Static_assert(sizeof(struct squashfs_comp_options) == 8, "squashfs compressor options layout");

/* ==================== in-memory volume context ==================== */

struct squashfs_meta_table {
    uint64_t *block_offsets;   /* heap: [0..block_count) absolute image offsets */
    uint32_t block_count;
    uint32_t capacity;
};

struct squashfs_fs {
    /* device */
    int      drive;             /* block-device number for the block path */
    uint32_t start_lba;         /* partition offset, 512-byte sectors */
    const uint8_t *ram;         /* RAM image base (ramdisk path), NULL = none */
    uint32_t ram_size;          /* RAM image size in bytes */
    uint64_t image_size;        /* usable bytes of the volume (device or RAM) */

    /* geometry (validated at mount) */
    uint32_t block_size;
    uint16_t flags;
    uint16_t compression;
    uint16_t id_count;
    uint32_t inode_count;
    uint32_t fragment_count;
    uint64_t root_inode_ref;

    struct squashfs_super sb;

    /* metadata table offset arrays (heap) */
    struct squashfs_meta_table inode_table;
    struct squashfs_meta_table dir_table;
    struct squashfs_meta_table frag_table;
    struct squashfs_meta_table id_table;

    /* reusable scratch buffers (heap) */
    uint8_t *meta_buf;          /* SQUASHFS_METADATA_SIZE decompressed */
    uint8_t *meta_raw;          /* SQUASHFS_METADATA_SIZE on-disk (worst case) */
    uint8_t *data_buf;          /* block_size decompressed data block */
    uint8_t *data_raw;          /* block_size on-disk data block */
    uint8_t *frag_buf;          /* block_size decompressed fragment block */
    uint32_t *sizes;            /* SQUASHFS_MAX_FILE_BLOCKS block-list entries */
    uint32_t sizes_cap;

    /* last decompressed data block, so sequential reads decompress once */
    uint32_t cached_block;      /* index, or 0xFFFFFFFF when empty */
    uint32_t cached_len;

    int      mounted;
};

extern struct squashfs_fs g_squashfs_fs;

/* The legacy global that the ported-from-CoolPotOS stub exposed. Kept so any
 * out-of-tree reference still links; it mirrors the head of g_squashfs_fs. */
struct squashfs_mount {
    int drive;
    uint32_t start_lba;
    uint32_t block_size;
    uint32_t block_log;
    uint32_t inodes;
    uint32_t fragments;
    uint16_t compression;
    uint16_t flags;
    uint32_t inode_table_start;
    uint32_t directory_table_start;
    uint32_t fragment_table_start;
    uint32_t lookup_table_start;
    uint32_t lookup_table_size;
    uint32_t inode_table_start_dup;
    uint32_t directory_table_start_dup;
    bool mounted;
    uint8_t block_buf[SQUASHFS_METADATA_SIZE];
};
extern struct squashfs_mount g_squashfs_mount;

/* ==================== stat / handles ==================== */

struct squashfs_stat {
    uint64_t inode_ref;   /* (metadata block << 16) | offset */
    uint32_t inode_number;
    uint32_t type;        /* SQUASHFS_INODE_* (extended types kept distinct) */
    uint32_t mode;        /* permission bits + S_IF* type bits */
    uint32_t uid;
    uint32_t gid;
    uint32_t mtime;
    uint64_t size;        /* file size, symlink target size, else 0 */
    uint32_t nlink;
    int      is_dir;
    int      is_file;
    int      is_symlink;
};

struct squashfs_file {
    int      used;
    uint64_t inode_ref;
    uint32_t inode_number;
    uint64_t size;
    uint32_t block_count;    /* full-size data blocks (excluding the fragment) */
    uint32_t block_sizes_cap;
    uint32_t *block_sizes;   /* heap: copy of the inode block list */
    uint32_t fragment_idx;
    uint32_t fragment_offset;
    uint64_t blocks_start;   /* absolute byte offset of the first data block */
};

struct squashfs_dir {
    int      used;
    uint64_t inode_ref;
    uint32_t size;           /* listing bytes, off-by-3 already removed */
    uint64_t start_block;    /* offset from the directory table start */
    uint16_t start_offset;   /* byte offset inside that uncompressed block */
    uint64_t pos;            /* bytes consumed of the listing */
    uint64_t next_header;    /* byte offset of the next header */
    uint32_t header_count;   /* entries remaining in the current header */
    uint32_t header_inode;   /* inode_number of the first entry in the header */
    uint32_t header_block;   /* inode metadata block of the header */
    uint32_t header_index;   /* index of the next entry inside the header */
    uint16_t header_num;     /* entries in the current header */
    uint32_t entries_done;   /* total entries returned (bounded by the cap) */
};

/* ==================== lifecycle ==================== */

/* Mount the SquashFS volume on a block device at start_lba (real partition
 * path). Returns SQUASHFS_OK (0) or a negative squashfs_err_t. */
int squashfs_mount(int drive, uint32_t start_lba);

/* Mount the RAM image previously handed to squashfs_register_ramdisk(). */
int squashfs_mount_ramdisk(void);

/* Register (not mount) a RAM-resident SquashFS image. The caller keeps the
 * buffer alive for as long as the volume stays mounted; nothing is copied. */
int squashfs_register_ramdisk(const void *image, uint32_t size);

int squashfs_unmount(void);
int squashfs_is_mounted(void);
int squashfs_ramdisk_registered(void);
uint32_t squashfs_block_size(void);
uint32_t squashfs_compression(void);
uint32_t squashfs_entry_count(void);   /* superblock inode_count */

/* ==================== path operations (volume-relative, e.g. "/SUB/A.TXT") ==== */

int squashfs_lookup(const char *path, uint64_t *out_ref);
int squashfs_stat(const char *path, struct squashfs_stat *out);

/* Read the target of a symlink inode reference into `buf` (NUL terminated when
 * the buffer is large enough). Returns the target length or a negative code. */
int squashfs_readlink_ref(uint64_t inode_ref, char *buf, uint32_t cap);
int squashfs_readlink(const char *path, char *buf, uint32_t cap);

/* Read `size` bytes at `offset` from inode reference `ref`. Returns the number
 * of bytes read (>= 0, short only at EOF) or a negative squashfs_err_t. */
int squashfs_read_ref(uint64_t ref, void *buf, uint32_t offset, uint32_t size);

/* Open/read/close a regular file. Handles come from a small static pool. */
int squashfs_open(const char *path, struct squashfs_file **out);
int squashfs_file_read(struct squashfs_file *f, void *buf, uint32_t size, uint32_t offset);
int squashfs_close(struct squashfs_file *f);

/* Directory iteration. readdir() returns SQUASHFS_OK per entry and
 * SQUASHFS_ENOENT at the end; "." and ".." are skipped (same convention as the
 * FAT32 and ext2 drivers). */
int squashfs_opendir(const char *path, struct squashfs_dir **out);
int squashfs_readdir(struct squashfs_dir *d, char *name, uint32_t name_cap,
                     uint32_t *name_len, int *is_dir);
int squashfs_closedir(struct squashfs_dir *d);

/* ==================== helpers ==================== */

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) - identical to Python's
 * zlib.crc32(), so the host scripts can cross-check guest reads. */
uint32_t squashfs_crc32(const void *data, uint32_t len);

/* Decompress one SquashFS block payload. `algo` is a SQUASHFS_COMP_* id,
 * `compressed` selects compressed vs stored. Returns the output length or a
 * negative squashfs_err_t / INFLATE_ERR_*. Also exported as the generic
 * squashfs_decompress() shim kept for the legacy API. */
int squashfs_decompress_block(uint16_t algo, int compressed,
                              const uint8_t *src, uint32_t src_len,
                              uint8_t *dst, uint32_t dst_cap);

/* Mount the registered ramdisk if needed, read every documented test file,
 * verify contents/sizes/directories/symlink and print
 *   [squashfs] selftest PASS (hello=0x.. large=0x.. nested=0x.. uncomp=0x.. frag=0x..)
 * or
 *   [squashfs] selftest FAIL (<reason>)
 * Returns SQUASHFS_OK / SQUASHFS_ERR. */
int squashfs_selftest(void);

/* ==================== legacy entry points (must keep linking) ====================
 * These are the symbols the pre-existing stub exported and that
 * kernel/fs/squashfs_vfs.c, kernel/shell/shell.c and kernel/main/kmain.c refer
 * to. Their signatures are unchanged; they now back onto the real driver.
 * squashfs_read_inode()/squashfs_write_inode() take the OLD 32-bit inode
 * "number" and therefore can only work for the root (2); every other value
 * fails honestly instead of pretending.
 */
int squashfs_init(void);
int squashfs_register(void);
int squashfs_vfs_init(void);
int squashfs_vfs_register(void);
int squashfs_mkdir(const char *path, uint32_t mode);
int squashfs_unlink(const char *path);
int squashfs_rmdir(const char *path);
int squashfs_stat_vfs64(const char *path, struct Vfs64Info64 *out);
int squashfs_read_block(uint32_t block_num, void *buf);
int squashfs_write_block(uint32_t block_num, const void *buf);
int squashfs_read_inode(uint32_t ino, void *inode_buf);
int squashfs_write_inode(uint32_t ino, const void *inode_buf);
int squashfs_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size);
int squashfs_write_file(uint32_t ino, const void *buf, uint32_t offset, uint32_t size);
int squashfs_read_dir(uint32_t dir_ino, void *buf, uint32_t max_entries);
int squashfs_compress(uint16_t algo, const void *src, size_t src_len,
                      void *dst, size_t *dst_len);
int squashfs_decompress(uint16_t algo, const void *src, size_t src_len,
                        void *dst, size_t *dst_len);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_SQUASHFS_H */
