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

/* Volume lifecycle. fatfs_mount() returns the volume slot (>= 0) or a negative
 * error, so several FAT32 volumes can be mounted at once (v0.24.0). */
int fatfs_mount(uint8_t drv, void *fs);
int fatfs_select_volume(int slot);
int fatfs_current_volume(void);
int fatfs_volume_count(void);
int fatfs_unmount(int drv);
int fatfs_vfs_init(void);

/* Partition support (v0.31.0, installer): fatfs_mount() keeps the historical
 * superfloppy behaviour (BPB at device LBA 0); fatfs_mount_at() mounts a FAT32
 * partition at start_lba (512-byte sectors); fatfs_mount_auto() tries LBA 0
 * first and then the MBR partition table (0x0B/0x0C/0x0E), which is how the
 * kernel finds the installed system volume. */
int fatfs_mount_at(uint8_t drv, void *fs, uint32_t start_lba);
int fatfs_mount_auto(uint8_t drv);

/* Bulk-write mode (installer): when on, sector writes skip the read-back verify
 * (the data is still written and the FAT-level checks still run). Off by default
 * so every normal path keeps the full verification. */
void fatfs_set_bulk_mode(int on);

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

/* ---- minimal FAT32 formatter (kernel/fs/fatfs_format.c) ----
 * Used by the installer to create the system volume on a freshly partitioned
 * disk. Geometry follows scripts/mkfat32_testimg.py so the host-side parsers
 * work unchanged. Returns FATFS_OK / negative error; fills *out when given. */
struct fat32_geometry {
    uint32_t start_lba;      /* partition start in 512-byte sectors */
    uint32_t total_sectors;  /* partition size */
    uint32_t spc;            /* sectors per cluster */
    uint32_t fat_sz;         /* sectors per FAT */
    uint32_t clusters;
    uint32_t data_lba;       /* partition-relative sector of cluster 2 */
    uint32_t root_cluster;
};
int fat32_format_partition(int drive, uint32_t start_lba, uint32_t total_sectors,
                           struct fat32_geometry *out);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_FATFS_H */
