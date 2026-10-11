/* SutureOS module: ext2/ext4 filesystem driver - 移植自 Cinux
 * Source: C:\Users\fanqi\Desktop\自研操作系统\Cinux\libs\ext2\ext2.hpp
 * Original license: MIT (Cinux)
 * Changes: Adapted to SutureOS; prefix ext2_; integrated with SutureOS VFS
 */
#ifndef STITCH_EXT2_H
#define STITCH_EXT2_H

#include <stitch/types.h>
#include "vfs64.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ext2/ext4 常量 */
#define EXT2_SUPER_MAGIC      0xEF53
#define EXT2_MAX_GROUPS       32
#define EXT2_INODE_CACHE_SIZE 256
#define EXT2_MAX_BLOCK_SIZE   4096
#define EXT2_INODE_SIZE       128
#define EXT2_SUPERBLOCK_OFFSET 1024

/* ext2 超级块 */
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
    uint16_t s_rev_level;
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
};

/* ext2 块组描述符 */
struct ext2_block_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};

/* ext2 inode */
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
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  osd2[12];
};

/* 文件类型 */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* inode 模式位 */
#define EXT2_S_IFMT   0170000
#define EXT2_S_IFREG  0100000
#define EXT2_S_IFDIR  0040000
#define EXT2_S_IFLNK  0120000

/* 文件系统状态 */
#define EXT2_VALID_FS     0x0001
#define EXT2_ERROR_FS     0x0002

/* 错误处理 */
#define EXT2_ERRORS_CONTINUE 1
#define EXT2_ERRORS_RO       2
#define EXT2_ERRORS_PANIC    3

/* ext2 文件系统上下文 */
struct ext2_fs {
    int drive;
    uint32_t start_lba;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint32_t group_count;
    uint32_t sectors_per_block;
    uint32_t mounted;
    
    struct ext2_superblock sb;
    struct ext2_block_group_desc bgdt[32];
    
    uint8_t block_buf[4096];
    uint8_t inode_cache[256][128];
    uint8_t inode_used[256];
    int cache_lru[256];
    int cache_count;
};

/* 全局文件系统实例 */
extern struct ext2_fs g_ext2_fs;

/* ext2 文件系统操作 */
int ext2_init(void);
int ext2_mount(int drive, uint32_t start_lba);
int ext2_unmount(void);
int ext2_read_block(uint32_t block_num, void* buf);
int ext2_write_block(uint32_t block_num, const void* buf);
int ext2_read_inode(uint32_t ino, void* inode_buf);
int ext2_write_inode(uint32_t ino, const void* inode_buf);
int ext2_lookup(const char* path, uint32_t* out_ino);
int ext2_read_file(uint32_t ino, void* buf, uint32_t offset, uint32_t size);
int ext2_write_file(uint32_t ino, const void* buf, uint32_t offset, uint32_t size);
int ext2_create(const char* path, uint32_t mode);
int ext2_mkdir(const char* path, uint32_t mode);
int ext2_unlink(const char* path);
int ext2_rmdir(const char* path);

/* 与 VFS 对接 */
int ext2_vfs_init(void);
int ext2_vfs_register(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_EXT2_H */