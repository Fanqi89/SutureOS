/* SutureOS module: FatFs (FAT12/16/32/exFAT) - 移植自 CoolPotOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\module\fatfs\ff.h
 * Original license: BSD-like (ChaN's FatFs license - very permissive)
 * Changes: Adapted to SutureOS; prefix fatfs_; adapted to SutureOS VFS
 */
#ifndef STITCH_FATFS_H
#define STITCH_FATFS_H

#include <stitch/types.h>
#include "vfs64.h"  /* 对接 SutureOS VFS */

#ifdef __cplusplus
extern "C" {
#endif

/* FatFs 配置常量 */
#define FATFS_MAX_SS        512
#define FATFS_MIN_SS        512
#define FATFS_USE_LFN       2       /* 2 = UTF-8 LFN */
#define FATFS_LFN_UNICODE   2       /* UTF-8 */
#define FATFS_MAX_LFN       255
#define FATFS_FS_EXFAT      1
#define FATFS_FS_RPATH      2
#define FATFS_FS_RPATH_EX   1
#define FATFS_FS_LOCK       1
#define FATFS_FS_REENTRANT  0
#define FATFS_STR_VOLUME_ID 1
#define FATFS_VOLUMES       4
#define FATFS_FS_READONLY   0
#define FATFS_FS_NORTC      0
#define FATFS_USE_FASTSEEK  1
#define FATFS_USE_FIND      1
#define FATFS_USE_LABEL     1
#define FATFS_USE_MKFS      1
#define FATFS_USE_FDISK     1
#define FATFS_USE_EXPAND    1
#define FATFS_USE_FORWARD   1
#define FATFS_USE_CHMOD     1
#define FATFS_USE_UTIME     1
#define FATFS_USE_CHDRIVE   1
#define FATFS_USE_CHDIR     1
#define FATFS_USE_GETCWD    1
#define FATFS_USE_GETFREE   1
#define FATFS_USE_GETLABEL  1
#define FATFS_USE_SETLABEL  1
#define FATFS_USE_FORWARD   1
#define FATFS_USE_EXPAND    1
#define FATFS_USE_LABEL     1
#define FATFS_USE_CHMOD     1
#define FATFS_USE_UTIME     1

/* FatFs 错误码 */
typedef enum {
    FATFS_OK = 0,
    FATFS_DISK_ERR,
    FATFS_INT_ERR,
    FATFS_NOT_READY,
    FATFS_NO_FILE,
    FATFS_NO_PATH,
    FATFS_INVALID_NAME,
    FATFS_DENIED,
    FATFS_EXIST,
    FATFS_INVALID_OBJECT,
    FATFS_WRITE_PROTECTED,
    FATFS_INVALID_DRIVE,
    FATFS_NOT_ENABLED,
    FATFS_NO_FILESYSTEM,
    FATFS_MKFS_ABORTED,
    FATFS_TIMEOUT,
    FATFS_LOCKED,
    FATFS_NOT_ENOUGH_CORE,
    FATFS_TOO_MANY_OPEN_FILES,
    FATFS_INVALID_PARAMETER
} fatfs_result_t;

/* FATFS 文件系统对象 */
typedef struct {
    uint8_t  fs_type;
    uint8_t  pdrv;
    uint8_t  ldrv;
    uint8_t  n_fats;
    uint8_t  wflag;
    uint8_t  fsi_flag;
    uint16_t id;
    uint16_t n_rootdir;
    uint16_t csize;
    uint16_t ssize;
    uint32_t last_clst;
    uint32_t free_clst;
    uint32_t cdir;
    uint32_t n_fatent;
    uint32_t fsize;
    uint32_t volbase;
    uint32_t fatbase;
    uint32_t dirbase;
    uint32_t database;
    uint32_t winsect;
    uint8_t  win[512];
} fatfs_t;

/* 文件对象 */
typedef struct {
    uint32_t sclust;
    uint32_t fsize;
    uint32_t fptr;
    uint32_t clust;
    uint32_t sect;
    uint32_t dir_sect;
    uint8_t* dir_ptr;
    uint8_t  flag;
    uint8_t  err;
} fatfs_fil_t;

/* 目录对象 */
typedef struct {
    uint32_t dptr;
    uint32_t clust;
    uint32_t sect;
    uint8_t* dir;
    uint8_t fn[12];
} fatfs_dir_t;

/* 文件信息 */
typedef struct {
    uint32_t fsize;
    uint16_t fdate;
    uint16_t ftime;
    uint8_t  fattrib;
    char fname[256];
} fatfs_filinfo_t;

/* 格式化参数 */
typedef struct {
    uint8_t  fmt;
    uint8_t  n_fat;
    uint32_t align;
    uint32_t n_root;
    uint32_t au_size;
} fatfs_mkfs_parm_t;

/* 文件访问模式 */
#define FATFS_FA_READ          0x01
#define FATFS_FA_WRITE         0x02
#define FATFS_FA_OPEN_EXISTING 0x00
#define FATFS_FA_CREATE_NEW    0x04
#define FATFS_FA_CREATE_ALWAYS 0x08
#define FATFS_FA_OPEN_ALWAYS   0x10
#define FATFS_FA_OPEN_APPEND   0x30

/* 文件属性 */
#define FATFS_AM_RDO  0x01
#define FATFS_AM_HID  0x02
#define FATFS_AM_SYS  0x04
#define FATFS_AM_DIR  0x10
#define FATFS_AM_ARC  0x20

/* FatFs 接口函数 */
int fatfs_mount(uint8_t drv, void* fs);
int fatfs_unmount(uint8_t drv);
int fatfs_open(void* fp, const char* path, uint8_t mode);
int fatfs_close(void* fp);
int fatfs_read(void* fp, void* buff, uint32_t btr, uint32_t* br);
int fatfs_write(void* fp, const void* buff, uint32_t btw, uint32_t* bw);
int fatfs_lseek(void* fp, uint32_t ofs);
int fatfs_truncate(void* fp);
int fatfs_sync(void* fp);
int fatfs_opendir(void* dp, const char* path);
int fatfs_closedir(void* dp);
int fatfs_readdir(void* dp, void* fno);
int fatfs_mkdir(const char* path);
int fatfs_unlink(const char* path);
int fatfs_rename(const char* path_old, const char* path_new);
int fatfs_stat(const char* path, void* fno);
int fatfs_chmod(const char* path, uint8_t attr, uint8_t mask);
int fatfs_utime(const char* path, void* fno);
int fatfs_chdir(const char* path);
int fatfs_chdrive(const char* path);
int fatfs_getcwd(char* buff, uint32_t len);
int fatfs_getfree(const char* path, uint32_t* nclst, void** fatfs);
int fatfs_mkfs(const char* path, const void* opt, void* work, uint32_t len);
int fatfs_fdisk(uint8_t pdrv, const uint32_t ptbl[], void* work);
int fatfs_getlabel(const char* path, char* label, uint32_t* vsn);
int fatfs_setlabel(const char* label);

/* FatFs 与 SutureOS VFS 对接 */
int fatfs_vfs_init(void);
int fatfs_vfs_register(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_FATFS_H */