/* SutureOS module: ext2/ext4 filesystem driver - 移植自 Cinux
 * Source: C:\Users\fanqi\Desktop\自研操作系统\Cinux\libs\ext2\ext2.cpp
 * Original license: MIT
 * Changes: Adapted to SutureOS; prefix ext2_; stub implementation
 */
#include "ext2.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* ATA 弱引用 */
__attribute__((weak)) bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
__attribute__((weak)) bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

/* ==================== 全局状态定义 ==================== */
struct ext2_fs g_ext2_fs = {0};

/* ==================== 内部实现函数 (static) ==================== */

static bool ext2_read_block_internal(uint32_t block_num, void* buf) {
    if (!ata64_read) return false;
    uint32_t lba = g_ext2_fs.start_lba + block_num * (g_ext2_fs.block_size / 512);
    return ata64_read(g_ext2_fs.drive, lba, g_ext2_fs.block_size / 512, buf);
}

static bool ext2_write_block_internal(uint32_t block_num, const void* buf) {
    if (!ata64_write) return false;
    uint32_t lba = g_ext2_fs.start_lba + block_num * (g_ext2_fs.block_size / 512);
    return ata64_write(g_ext2_fs.drive, lba, g_ext2_fs.block_size / 512, buf);
}

static bool ext2_read_superblock_internal(void) {
    if (!ext2_read_block(2, g_ext2_fs.block_buf)) return false;
    
    const uint8_t* src = g_ext2_fs.block_buf + 1024;
    
    uint32_t magic = *(uint16_t*)(src + 56);
    if (magic != 0xEF53) {
        console_puts("[ext2] invalid magic\n");
        return false;
    }
    
    uint8_t* dst = (uint8_t*)&g_ext2_fs.sb;
    for (int i = 0; i < 84; i++) {
        ((uint8_t*)&g_ext2_fs.sb)[i] = src[i];
    }
    
    g_ext2_fs.block_size = 1024 << g_ext2_fs.sb.s_log_block_size;
    g_ext2_fs.sectors_per_block = g_ext2_fs.block_size / 512;
    g_ext2_fs.blocks_per_group = g_ext2_fs.sb.s_blocks_per_group;
    g_ext2_fs.inodes_per_group = g_ext2_fs.sb.s_inodes_per_group;
    g_ext2_fs.first_data_block = g_ext2_fs.sb.s_first_data_block;
    g_ext2_fs.block_size = 1024 << g_ext2_fs.sb.s_log_block_size;
    g_ext2_fs.sectors_per_block = g_ext2_fs.block_size / 512;
    
    return true;
}

static bool ext2_read_bgdt_internal(void) {
    uint32_t bgdt_block = g_ext2_fs.first_data_block + 1;
    uint32_t bgdt_blocks = (g_ext2_fs.sb.s_blocks_count + g_ext2_fs.blocks_per_group - 1) / g_ext2_fs.blocks_per_group;
    bgdt_blocks = (bgdt_blocks * sizeof(struct ext2_block_group_desc) + g_ext2_fs.block_size - 1) / g_ext2_fs.block_size;
    
    for (uint32_t i = 0; i < 32 && i < bgdt_blocks; i++) {
        if (!ext2_read_block(bgdt_block + i, g_ext2_fs.block_buf)) return false;
        struct ext2_block_group_desc* bgdt_src = (struct ext2_block_group_desc*)g_ext2_fs.block_buf;
        g_ext2_fs.bgdt[i] = bgdt_src[i];
    }
    return true;
}

static bool ext2_read_inode_internal(uint32_t ino, void* inode_buf) {
    if (ino == 0 || ino > g_ext2_fs.sb.s_inodes_count) return false;
    
    uint32_t ino_per_group = g_ext2_fs.sb.s_inodes_per_group;
    uint32_t group = (ino - 1) / ino_per_group;
    uint32_t index = (ino - 1) % ino_per_group;
    
    if (group >= 32) return false;
    
    uint32_t inodes_per_block = 4096 / 128;
    uint32_t block = g_ext2_fs.bgdt[group].bg_inode_table + index / inodes_per_block;
    uint32_t offset = (index % inodes_per_block) * 128;
    
    if (!ext2_read_block(block, g_ext2_fs.block_buf)) return false;
    
    uint8_t* src = g_ext2_fs.block_buf + offset;
    uint8_t* dst = (uint8_t*)inode_buf;
    for (int i = 0; i < 128; i++) {
        ((uint8_t*)inode_buf)[i] = src[i];
    }
    return true;
}

static bool ext2_write_inode_internal(uint32_t ino, const void* inode_buf) {
    (void)ino; (void)inode_buf; return true;
}

static uint32_t ext2_lookup_dir_internal(uint32_t dir_ino, const char* name, uint32_t name_len) {
    (void)dir_ino; (void)name; (void)name_len; return 0;
}

static uint32_t ext2_lookup_internal(const char* path) {
    if (!path || path[0] != '/') return 0;
    if (strcmp(path, "/") == 0) return 2;
    
    uint32_t current_ino = 2;
    const char* p = path + 1;
    
    while (*p) {
        char name[256];
        uint32_t i = 0;
        while (*p && *p != '/' && i < 255) name[i++] = *p++;
        name[i] = '\0';
        if (*p == '/') p++;
        if (i == 0) continue;
        
        uint32_t next_ino = 0;
        if (!next_ino) return 0;
        current_ino = next_ino;
    }
    return current_ino;
}

static int ext2_read_file_internal(uint32_t ino, void* buf, uint32_t offset, uint32_t size) {
    (void)ino; (void)buf; (void)offset; (void)size; return -1;
}

static int ext2_write_file_internal(uint32_t ino, const void* buf, uint32_t offset, uint32_t size) {
    (void)ino; (void)buf; (void)offset; (void)size; return -1;
}

/* ==================== 公共 API 函数 (实现头文件声明) ==================== */

int ext2_read_block(uint32_t block_num, void* buf) { 
    return ext2_read_block_internal(block_num, buf) ? 0 : -1;
}

int ext2_write_block(uint32_t block_num, const void* buf) {
    return ext2_write_block_internal(block_num, buf) ? 0 : -1;
}

int ext2_read_inode(uint32_t ino, void* inode_buf) {
    return ext2_read_inode_internal(ino, inode_buf) ? 0 : -1;
}

int ext2_write_inode(uint32_t ino, const void* inode_buf) {
    (void)ino; (void)inode_buf; return 0;
}

int ext2_lookup(const char* path, uint32_t* out_ino) {
    uint32_t ino = ext2_lookup_internal(path);
    if (ino == 0) return -1;
    *out_ino = ino;
    return 0;
}

int ext2_read_file(uint32_t ino, void* buf, uint32_t offset, uint32_t size) {
    return -1;
}

int ext2_write_file(uint32_t ino, const void* buf, uint32_t offset, uint32_t size) {
    return -1;
}

int ext2_create(const char* path, uint32_t mode) { (void)path; (void)mode; return 0; }
int ext2_mkdir(const char* path, uint32_t mode) { (void)path; (void)mode; return 0; }
int ext2_unlink(const char* path) { (void)path; return -1; }
int ext2_rmdir(const char* path) { (void)path; return -1; }
int ext2_stat(const char* path, struct vfs64_info64* out) { (void)path; (void)out; return -1; }

int ext2_vfs_init(void) {
    console_puts("[ext2] init stub\n");
    return 0;
}

int ext2_vfs_register(void) {
    console_puts("[ext2] VFS register stub\n");
    return 0;
}

int ext2_mount(int drive, uint32_t start_lba) {
    g_ext2_fs.drive = drive;
    g_ext2_fs.start_lba = start_lba;
    
    if (!ext2_read_superblock_internal()) return -1;
    if (!ext2_read_bgdt_internal()) return -1;
    
    g_ext2_fs.mounted = 1;
    console_printf("[ext2] mounted on drive %d, LBA %u\n", drive, start_lba);
    return 0;
}

int ext2_unmount(void) {
    g_ext2_fs.mounted = 0;
    return 0;
}