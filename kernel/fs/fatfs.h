/* SutureOS: FAT32 filesystem - real driver
 * Ported/adapted from: SpiritFoxOS kernel/src/fs/fat32.c
 * (C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS), GPL-3.0
 * Changes: SutureOS block layer (blk_read/blk_write) I/O, single-volume
 *          context, sector-granular heap scratch buffers, contiguous-run
 *          cluster allocation, byte-for-byte write verification.
 *
 * Scope (honest): superfloppy volumes only (BPB at device LBA 0 - no MBR
 * partition scan). 8.3 names, no LFN. Writable: create/write/truncate/
 * mkdir/rmdir/unlink/sync and same-directory rename (name-field rewrite;
 * cross-directory move is refused with a log).
 */
#ifndef STITCH_FATFS_H
#define STITCH_FATFS_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (negative) */
typedef enum fatfs_err {
    FATFS_OK           = 0,
    FATFS_ERR          = -1,   /* generic */
    FATFS_ENOENT       = -2,
    FATFS_ENOTFOUND    = -2,   /* alias used by the VFS adapter */
    FATFS_EIO          = -3,
    FATFS_ENOSPC       = -4,
    FATFS_EINVAL       = -5,
    FATFS_ENOTDIR      = -6,
    FATFS_EISDIR       = -7,
    FATFS_ENOTEMPTY    = -8,
    FATFS_EBUSY        = -9,
    FATFS_ENAMETOOLONG = -10,
    FATFS_EEXIST       = -11,  /* rename target already present */
} fatfs_err_t;

/* Volume lifecycle */
int fatfs_mount(uint8_t drv, void *fs);
int fatfs_unmount(int drv);
int fatfs_vfs_init(void);

/* Path operations (volume-relative absolute path, e.g. "/HELLO.TXT"). */
typedef struct fatfs_file fatfs_file_t;
typedef struct fatfs_dir  fatfs_dir_t;

int fatfs_open(const char *path, int create, fatfs_file_t **out);
int fatfs_close(fatfs_file_t *f);
int fatfs_read(fatfs_file_t *f, uint32_t off, void *buf, uint32_t len);
int fatfs_write(fatfs_file_t *f, uint32_t off, const void *buf, uint32_t len);
int fatfs_truncate(fatfs_file_t *f, uint32_t new_size);

int fatfs_opendir(const char *path, fatfs_dir_t **out);
/* Returns FATFS_OK per entry, FATFS_ENOTFOUND at end. */
int fatfs_readdir(fatfs_dir_t *d, char *name, uint32_t name_cap,
                  uint32_t *size, int *is_dir);
int fatfs_closedir(fatfs_dir_t *d);

int fatfs_stat(const char *path, uint32_t *size, int *is_dir);
int fatfs_mkdir(const char *path);
int fatfs_rmdir(const char *path);
int fatfs_unlink(const char *path);
/* Rename within one directory (8.3). FATFS_EEXIST when the target exists,
 * FATFS_EINVAL for a cross-directory move or the root. */
int fatfs_rename(const char *old_path, const char *new_path);
int fatfs_sync(void);
int fatfs_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_FATFS_H */
