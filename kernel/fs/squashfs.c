/* SutureOS module: SquashFS filesystem driver - 移植自 CoolPotOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\squashfs\squashfs.c
 * Original license: GPL-2.0 (SquashFS) + MIT (CoolPotOS wrapper)
 * Changes: Adapted to SutureOS; prefix squashfs_; stub implementation
 */
#include "squashfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* ATA 弱引用 */
__attribute__((weak)) bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
__attribute__((weak)) bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

/* ==================== 全局状态定义 ==================== */
struct squashfs_mount g_squashfs_mount = {0};

/* ==================== 内部实现函数 (static) ==================== */

static bool squashfs_read_block_internal(uint32_t block_num, void* buf) {
    if (!ata64_read) return false;
    uint32_t lba = g_squashfs_mount.start_lba + block_num * (g_squashfs_mount.block_size / 512);
    return ata64_read(g_squashfs_mount.drive, lba, g_squashfs_mount.block_size / 512, buf);
}

static bool squashfs_write_block_internal(uint32_t block_num, const void* buf) {
    if (!ata64_write) return false;
    uint32_t lba = g_squashfs_mount.start_lba + block_num * (g_squashfs_mount.block_size / 512);
    return ata64_write(g_squashfs_mount.drive, lba, g_squashfs_mount.block_size / 512, buf);
}

static bool squashfs_read_superblock_internal(void) {
    if (!squashfs_read_block_internal(0, g_squashfs_mount.block_buf)) return false;
    
    const uint8_t* src = g_squashfs_mount.block_buf;
    uint32_t magic = *(uint32_t*)src;
    if (magic != SQUASHFS_MAGIC) {
        console_puts("[squashfs] invalid magic\n");
        return false;
    }
    
    // 简化实现
    return true;
}

static bool squashfs_read_inode_internal(uint32_t ino, void* inode_buf) {
    (void)ino; (void)inode_buf; return false;
}

static bool squashfs_lookup_dir_internal(uint32_t dir_ino, const char* name, uint32_t name_len) {
    (void)dir_ino; (void)name; (void)name_len; return 0;
}

static uint32_t squashfs_lookup_internal(const char* path) {
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
        
        uint32_t next_ino = 0; /* squashfs_lookup_dir_internal(current_ino, name, i); */
        if (!next_ino) return 0;
        current_ino = next_ino;
    }
    return current_ino;
}

static int squashfs_read_file_internal(uint32_t ino, void* buf, uint32_t offset, uint32_t size) {
    (void)ino; (void)buf; (void)offset; (void)size; return -1;
}

static int squashfs_write_file_internal(uint32_t ino, const void* buf, uint32_t offset, uint32_t size) {
    (void)ino; (void)buf; (void)offset; (void)size; return -1;
}

static int squashfs_read_dir_internal(uint32_t dir_ino, void* buf, uint32_t max_entries) {
    (void)dir_ino; (void)buf; (void)max_entries; return 0;
}

/* ==================== 公共 API 函数 (实现头文件声明) ==================== */

int squashfs_mkdir(const char* path, uint32_t mode) { (void)path; (void)mode; return 0; }
int squashfs_unlink(const char* path) { (void)path; return -1; }
int squashfs_rmdir(const char* path) { (void)path; return -1; }
int squashfs_stat(const char* path, struct Vfs64Info64* out) { (void)path; (void)out; return -1; }

int squashfs_vfs_init(void) {
    console_puts("[squashfs] init stub\n");
    return 0;
}

int squashfs_vfs_register(void) {
    console_puts("[squashfs] VFS register stub\n");
    return 0;
}

int squashfs_mount(int drive, uint32_t start_lba) {
    g_squashfs_mount.drive = drive;
    g_squashfs_mount.start_lba = start_lba;
    
    g_squashfs_mount.mounted = 1;
    console_printf("[squashfs] mounted on drive %d, LBA %u\n", drive, start_lba);
    return 0;
}

int squashfs_unmount(void) {
    g_squashfs_mount.mounted = 0;
    return 0;
}

/* 公共 API - 实现头文件声明的函数 */
int squashfs_read_block(uint32_t block_num, void* buf) { 
    return squashfs_read_block_internal(block_num, buf) ? 0 : -1; 
}

int squashfs_write_block(uint32_t block_num, const void* buf) {
    return squashfs_write_block_internal(block_num, buf) ? 0 : -1;
}

int squashfs_read_inode(uint32_t ino, void* inode_buf) { 
    (void)ino; (void)inode_buf; return 0; 
}

int squashfs_write_inode(uint32_t ino, const void* inode_buf) { 
    (void)ino; (void)inode_buf; return 0; 
}

int squashfs_lookup(const char* path, uint32_t* out_ino) { 
    uint32_t ino = squashfs_lookup_internal(path);
    if (ino == 0) return -1;
    *out_ino = ino;
    return 0; 
}

int squashfs_read_file(uint32_t ino, void* buf, uint32_t offset, uint32_t size) {
    return squashfs_read_file_internal(ino, buf, offset, size);
}

int squashfs_write_file(uint32_t ino, const void* buf, uint32_t offset, uint32_t size) {
    return squashfs_write_file_internal(ino, buf, offset, size);
}

int squashfs_read_dir(uint32_t dir_ino, void* buf, uint32_t max_entries) { 
    (void)dir_ino; (void)buf; (void)max_entries; return 0; 
}

/* 压缩/解压缩 */
int squashfs_compress(uint16_t algo, const void* src, size_t src_len, void* dst, size_t* dst_len) {
    (void)algo; (void)src; (void)src_len; (void)dst; (void)dst_len; return -1;
}

int squashfs_decompress(uint16_t algo, const void* src, size_t src_len, void* dst, size_t* dst_len) {
    (void)algo; (void)src; (void)src_len; (void)dst; (void)dst_len; return -1;
}

int squashfs_init(void) {
    console_puts("[squashfs] init stub\n");
    return 0;
}

int squashfs_register(void) {
    console_puts("[squashfs] VFS register stub\n");
    return 0;
}