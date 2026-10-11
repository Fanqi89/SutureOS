/* SutureOS module: SquashFS filesystem driver - 移植自 CoolPotOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\squashfs\squashfs.h
 * Original license: GPL-2.0 (SquashFS) + MIT (CoolPotOS wrapper)
 * Changes: Adapted to SutureOS; prefix squashfs_; stub implementation
 */
#ifndef STITCH_SQUASHFS_H
#define STITCH_SQUASHFS_H

#include <stitch/types.h>
#include "vfs64.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SquashFS 常量 */
#define SQUASHFS_MAGIC        0x73717368  /* 'sqsh' */
#define SQUASHFS_BLOCK_SIZE   4096
#define SQUASHFS_MAX_COMPRESSORS 8

/* 压缩算法 */
#define SQUASHFS_COMP_GZIP    1
#define SQUASHFS_COMP_LZO     2
#define SQUASHFS_COMP_LZMA    3
#define SQUASHFS_COMP_XZ      4
#define SQUASHFS_COMP_ZSTD    5
#define SQUASHFS_COMP_LZ4     6

/* SquashFS 超级块 */
struct squashfs_super {
    uint32_t s_magic;
    uint32_t s_inodes;
    uint32_t s_mkfs_time;
    uint32_t s_block_size;
    uint32_t s_fragments;
    uint16_t s_compression;
    uint16_t s_block_log;
    uint16_t s_flags;
    uint16_t s_no_ids;
    uint32_t s_sbytes;
    uint32_t s_id_table_start;
    uint32_t s_xattr_id_table_start;
    uint32_t s_inode_table_start;
    uint32_t s_directory_table_start;
    uint32_t s_fragment_table_start;
    uint32_t s_lookup_table_start;
    uint32_t s_lookup_table_size;
};

/* SquashFS inode 类型 */
#define SQUASHFS_INODE_DIR     1
#define SQUASHFS_INODE_FILE    2
#define SQUASHFS_INODE_SYMLINK 3
#define SQUASHFS_INODE_BLKDEV  4
#define SQUASHFS_INODE_CHRDEV  5
#define SQUASHFS_INODE_FIFO    6
#define SQUASHFS_INODE_SOCKET  7

/* 压缩器类型 */
struct squashfs_compressor {
    uint16_t id;
    const char* name;
    int (*compress)(const void* src, size_t src_len, void* dst, size_t* dst_len);
    int (*decompress)(const void* src, size_t src_len, void* dst, size_t* dst_len);
};

/* 压缩器注册表 */
extern struct squashfs_compressor g_squashfs_compressors[8];
extern int g_squashfs_compressor_count;

/* 注册压缩算法 */
int squashfs_register_compressor(uint16_t id, const char* name, 
                                 int (*compress)(const void*, size_t, void*, size_t*),
                                 int (*decompress)(const void*, size_t, void*, size_t*));

/* SquashFS 挂载上下文 */
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
    uint8_t block_buf[4096];  /* 添加缓冲区字段 */
};

/* 全局状态 */
extern struct squashfs_mount g_squashfs_mount;

/* SquashFS 操作 */
int squashfs_init(void);
int squashfs_mount(int drive, uint32_t start_lba);
int squashfs_unmount(void);
int squashfs_read_block(uint32_t block_num, void* buf);
int squashfs_write_block(uint32_t block_num, const void* buf);
int squashfs_read_inode(uint32_t ino, void* inode_buf);
int squashfs_lookup(const char* path, uint32_t* out_ino);
int squashfs_read_file(uint32_t ino, void* buf, uint32_t offset, uint32_t size);
int squashfs_read_dir(uint32_t dir_ino, void* buf, uint32_t max_entries);
int squashfs_stat(const char* path, struct Vfs64Info64* out);

/* 压缩/解压缩 */
int squashfs_compress(uint16_t algo, const void* src, size_t src_len, void* dst, size_t* dst_len);
int squashfs_decompress(uint16_t algo, const void* src, size_t src_len, void* dst, size_t* dst_len);

/* 初始化/注册 */
int squashfs_init(void);
int squashfs_register(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_SQUASHFS_H */