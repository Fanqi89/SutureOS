/* SutureOS module: FatFs (FAT12/16/32/exFAT) - 移植自 CoolPotOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\fatfs\ff.c
 * Original license: BSD-like (ChaN's FatFs license)
 * Changes: Adapted to SutureOS; prefix fatfs_; stub implementation
 */
#include "fatfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* ==================== 存根实现 ==================== */

static uint8_t g_fatfs_drv = 0xFF;

int fatfs_mount(uint8_t drv, void* fs) {
    (void)drv; (void)fs;
    g_fatfs_drv = 0;
    console_puts("[fatfs] mount stub\n");
    return 0;
}

int fatfs_unmount(uint8_t drv) {
    (void)drv;
    g_fatfs_drv = 0xFF;
    return 0;
}

int fatfs_open(void* fp, const char* path, uint8_t mode) {
    (void)fp; (void)path; (void)mode;
    console_printf("[fatfs] open stub: %s\n", path);
    return 0;
}

int fatfs_close(void* fp) {
    (void)fp;
    return 0;
}

int fatfs_read(void* fp, void* buff, uint32_t btr, uint32_t* br) {
    (void)fp; (void)buff; (void)btr; (void)br;
    return 0;
}

int fatfs_write(void* fp, const void* buff, uint32_t btw, uint32_t* bw) {
    (void)fp; (void)buff; (void)btw; (void)bw;
    return 0;
}

int fatfs_lseek(void* fp, uint32_t ofs) {
    (void)fp; (void)ofs;
    return 0;
}

int fatfs_truncate(void* fp) {
    (void)fp;
    return 0;
}

int fatfs_sync(void* fp) {
    (void)fp;
    return 0;
}

int fatfs_opendir(void* dp, const char* path) {
    (void)dp; (void)path;
    return 0;
}

int fatfs_closedir(void* dp) {
    (void)dp;
    return 0;
}

int fatfs_readdir(void* dp, void* fno) {
    (void)dp; (void)fno;
    return 0;
}

int fatfs_mkdir(const char* path) {
    (void)path;
    return 0;
}

int fatfs_unlink(const char* path) {
    (void)path;
    return 0;
}

int fatfs_rename(const char* path_old, const char* path_new) {
    (void)path_old; (void)path_new;
    return 0;
}

int fatfs_stat(const char* path, void* fno) {
    (void)path; (void)fno;
    return 0;
}

int fatfs_chmod(const char* path, uint8_t attr, uint8_t mask) {
    (void)path; (void)attr; (void)mask;
    return 0;
}

int fatfs_utime(const char* path, void* fno) {
    (void)path; (void)fno;
    return 0;
}

int fatfs_chdir(const char* path) {
    (void)path;
    return 0;
}

int fatfs_chdrive(const char* path) {
    (void)path;
    return 0;
}

int fatfs_getcwd(char* buff, uint32_t len) {
    (void)buff; (void)len;
    return 0;
}

int fatfs_getfree(const char* path, uint32_t* nclst, void** fatfs) {
    (void)path; (void)nclst; (void)fatfs;
    return 0;
}

int fatfs_mkfs(const char* path, const void* opt, void* work, uint32_t len) {
    (void)path; (void)opt; (void)work; (void)len;
    return 0;
}

int fatfs_fdisk(uint8_t pdrv, const uint32_t ptbl[], void* work) {
    (void)pdrv; (void)ptbl; (void)work;
    return 0;
}

int fatfs_getlabel(const char* path, char* label, uint32_t* vsn) {
    (void)path; (void)label; (void)vsn;
    return 0;
}

int fatfs_setlabel(const char* label) {
    (void)label;
    return 0;
}

/* FatFs 与 SutureOS VFS 对接 */
int fatfs_vfs_init(void) {
    console_puts("[fatfs] VFS init stub\n");
    return 0;
}

int fatfs_vfs_register(void) {
    console_puts("[fatfs] VFS register stub\n");
    return 0;
}