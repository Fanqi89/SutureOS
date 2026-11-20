/* SutureOS: MayFS filesystem driver for the unified VFS.
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the MayFS core in
 * kernel/fs/mayfs.c - same shape as kernel/fs/ext2_vfs.c and
 * kernel/fs/vimtufs2_vfs.c.  The driver name is "mayfs", so
 *     mount mayfs 1 /mnt/may
 * from the shell binds block drive 1 and the core becomes live.
 *
 * READ-WRITE: create / write / truncate (O_TRUNC) / unlink / mkdir / rmdir /
 * rename reach the core, and `sync` commits both 256 KiB snapshots (backup slot
 * first, then primary).  MayFS has no per-write journal: the in-memory table is
 * authoritative until a commit, exactly like MayOS (fs_sync()).
 *
 * Provenance/licence: format and algorithms from MayOS kernel/fs.c (MIT,
 * Copyright (c) 2024 MayOS Project - read from the repository LICENSE during
 * this port); adapter code written for SutureOS.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "mayfs.h"

/* One MayFS volume at a time (the core keeps a single 48-record table). */
struct mayfs_mnt {
    int used;
    char mp[VFS_MAX_PATH];
    int drive;
};
static struct mayfs_mnt g_mnt;

static int map_err(int e) {
    switch (e) {
        case MAYFS_OK:          return VFS_OK;
        case MAYFS_ENOENT:      return VFS_ENOTFOUND;
        case MAYFS_ENOTDIR:     return VFS_ENOTDIR;
        case MAYFS_EISDIR:      return VFS_EISDIR;
        case MAYFS_EINVAL:      return VFS_EINVAL;
        case MAYFS_EEXIST:      return VFS_ECONFLICT;
        case MAYFS_ENOSPC:      return VFS_ENOSPC;
        case MAYFS_EIO:         return VFS_EIO;
        case MAYFS_ENOTEMPTY:   return VFS_ECONFLICT;
        case MAYFS_EBUSY:       return VFS_EBUSY;
        case MAYFS_ENOTSUP:     return VFS_EPERM;
        default:                return VFS_ERR;
    }
}

/* Device strings understood here:
 *   "0" / "1" ...            block drive number (1 = second IDE disk, -hdb)
 *   "sda" / "sdb" ...        IDE-style letter, 'a' = drive 0
 *   "/dev/sdb", "/dev/1"     the same with the /dev/ prefix
 */
static int parse_device(const char *device, int *out_drive) {
    const char *p = device;

    if (!device || !out_drive) return VFS_EINVAL;
    if (strncmp(p, "/dev/", 5) == 0) p += 5;
    if (p[0] >= '0' && p[0] <= '9') {
        int d = 0;
        while (*p >= '0' && *p <= '9') { d = d * 10 + (*p - '0'); p++; }
        *out_drive = d;
        return VFS_OK;
    }
    if ((p[0] == 's' || p[0] == 'S') && (p[1] == 'd' || p[1] == 'D') &&
        p[2] >= 'a' && p[2] <= 'z') {
        *out_drive = p[2] - 'a';
        return VFS_OK;
    }
    return VFS_EINVAL;
}

/* VFS path -> volume-relative path ("/HELLO.TXT"), stripping the mountpoint. */
static int vol_path(const char *path, char *out, uint32_t cap) {
    const char *rel;
    size_t mplen;

    if (!path || !out || cap == 0) return VFS_EINVAL;
    if (!g_mnt.used) return VFS_EIO;
    mplen = strlen(g_mnt.mp);
    rel = path;
    if (mplen > 1) {
        if (strncmp(path, g_mnt.mp, mplen) != 0) return VFS_ENOTFOUND;
        rel = path + mplen;
    }
    if (rel[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        return VFS_OK;
    }
    if (rel[0] != '/') {
        out[0] = '/';
        strncpy(out + 1, rel, cap - 2);
        out[cap - 1] = '\0';
    } else {
        strncpy(out, rel, cap - 1);
        out[cap - 1] = '\0';
    }
    return VFS_OK;
}

/* ==================== driver operations ==================== */

static int mayfs_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    int drive = 0, r;

    if (!mountpoint) return VFS_EINVAL;
    if (g_mnt.used || mayfs_is_mounted()) {
        console_puts("[mayfs] mount: one MayFS volume at a time (already mounted)\n");
        return VFS_EBUSY;
    }
    r = parse_device(device, &drive);
    if (r != VFS_OK) return r;
    r = mayfs_mount(drive, 0);
    if (r != MAYFS_OK) return map_err(r);

    memset(&g_mnt, 0, sizeof(g_mnt));
    g_mnt.used = 1;
    g_mnt.drive = drive;
    strncpy(g_mnt.mp, mountpoint, sizeof(g_mnt.mp) - 1);
    g_mnt.mp[sizeof(g_mnt.mp) - 1] = '\0';
    if (fs_data) *fs_data = &g_mnt;
    console_printf("[mayfs] vfs mount drive=%d at %s (generation %u, %d of %u "
                   "records used)\n",
                   (int64_t)drive, g_mnt.mp,
                   (uint64_t)mayfs_generation(), (int64_t)mayfs_used_count(),
                   (uint64_t)MAYFS_MAX_FILES);
    return VFS_OK;
}

static int mayfs_vfs_unmount(const char *mountpoint) {
    int r;
    if (!g_mnt.used) return VFS_ENOTFOUND;
    if (strcmp(mountpoint, g_mnt.mp) != 0) return VFS_ENOTFOUND;
    r = mayfs_unmount();
    memset(&g_mnt, 0, sizeof(g_mnt));
    return map_err(r);
}

static int mayfs_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    char vp[MAYFS_PATH_MAX];
    struct mayfs_file *fh = NULL;
    int r;

    (void)fs_data;
    if (!file_handle) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    /* MayFS open is uniform (the VFS core enforces the access mode), so the
     * mutating flags only decide create-vs-fail and truncate-vs-keep. */
    r = mayfs_file_open(vp, (flags & VFS_O_CREAT) ? 1 : 0,
                            (flags & VFS_O_TRUNC) ? 1 : 0, &fh);
    if (r != MAYFS_OK) return map_err(r);
    *file_handle = fh;
    return VFS_OK;
}

static int mayfs_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return map_err(mayfs_file_close((struct mayfs_file *)file_handle));
}

static int mayfs_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    int n;
    if (!file_handle) return VFS_EINVAL;
    n = mayfs_file_read((struct mayfs_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = bytes read */
}

static int mayfs_vfs_write(void *file_handle, const void *buf, uint32_t size,
                           uint32_t offset) {
    int n;
    if (!file_handle) return VFS_EINVAL;
    n = mayfs_file_write((struct mayfs_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = bytes written */
}

static int mayfs_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* MayFS files are random-access record bodies; the VFS core owns the file
     * position and always calls this with the absolute offset (SEEK_SET). */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int mayfs_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    char vp[MAYFS_PATH_MAX];
    struct mayfs_dir *d = NULL;
    int r;

    (void)fs_data;
    if (!dir_handle) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = mayfs_opendir(vp, &d);
    if (r != MAYFS_OK) return map_err(r);
    *dir_handle = d;
    return VFS_OK;
}

static int mayfs_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                             vfs_node_type_t *type) {
    uint32_t len = 0;
    int is_dir = 0;
    int r;

    if (!dir_handle || !name) return VFS_EINVAL;
    r = mayfs_readdir((struct mayfs_dir *)dir_handle, name, VFS_MAX_NAME, &len, &is_dir);
    if (r == MAYFS_ENOENT) return VFS_ENOTFOUND;       /* end of directory */
    if (r != MAYFS_OK) return map_err(r);
    if (name_len) *name_len = len;
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int mayfs_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return map_err(mayfs_closedir((struct mayfs_dir *)dir_handle));
}

static int mayfs_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    char vp[MAYFS_PATH_MAX];
    struct mayfs_stat ms;
    int r;

    (void)fs_data;
    if (!st) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = mayfs_stat(vp, &ms);
    if (r != MAYFS_OK) return map_err(r);
    memset(st, 0, sizeof(*st));
    st->type = ms.is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    st->size = ms.size;
    st->mode = ms.is_dir ? 0755u : 0644u;
    return VFS_OK;
}

static int mayfs_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    char vp[MAYFS_PATH_MAX];
    int r;
    (void)fs_data; (void)mode;   /* MayFS carries no permission bits */
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = mayfs_create(vp, 1);
    return (r < 0) ? map_err(r) : VFS_OK;
}

static int mayfs_vfs_rmdir(void *fs_data, const char *path) {
    char vp[MAYFS_PATH_MAX];
    struct mayfs_stat ms;
    int r;
    (void)fs_data;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = mayfs_stat(vp, &ms);
    if (r != MAYFS_OK) return map_err(r);
    if (!ms.is_dir) return VFS_ENOTDIR;
    r = mayfs_remove(vp);
    return map_err(r);
}

static int mayfs_vfs_unlink(void *fs_data, const char *path) {
    char vp[MAYFS_PATH_MAX];
    struct mayfs_stat ms;
    int r;
    (void)fs_data;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = mayfs_stat(vp, &ms);
    if (r != MAYFS_OK) return map_err(r);
    if (ms.is_dir) return VFS_EISDIR;
    r = mayfs_remove(vp);
    return map_err(r);
}

static int mayfs_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    char ovp[MAYFS_PATH_MAX], nvp[MAYFS_PATH_MAX];
    int r;
    (void)fs_data;
    r = vol_path(oldpath, ovp, sizeof(ovp));
    if (r != VFS_OK) return r;
    r = vol_path(newpath, nvp, sizeof(nvp));
    if (r != VFS_OK) return r;
    r = mayfs_rename(ovp, nvp);
    return map_err(r);
}

static int mayfs_vfs_sync(void *fs_data) {
    (void)fs_data;
    if (!g_mnt.used || !mayfs_is_mounted()) return VFS_EBUSY;
    return map_err(mayfs_sync());
}

static int mayfs_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                    uint64_t *total_bytes) {
    uint32_t free_records = 0, used = 0;
    (void)fs_data;
    if (!g_mnt.used || !mayfs_is_mounted()) return VFS_EBUSY;
    if (mayfs_count_free(&free_records, &used) != MAYFS_OK) return VFS_EIO;
    /* MayFS capacity is counted in record slots, not disk blocks: every record
     * is 4160 bytes and holds at most 4095 payload bytes.  Report the payload
     * budget so `df` cannot claim more than the filesystem can store. */
    if (free_bytes) *free_bytes = (uint64_t)free_records * MAYFS_FILE_MAX;
    if (total_bytes) *total_bytes = (uint64_t)MAYFS_MAX_FILES * MAYFS_FILE_MAX;
    return VFS_OK;
}

/* ==================== driver export ==================== */

const vfs_fs_driver_t mayfs_driver = {
    .name = "mayfs",
    .mount = mayfs_vfs_mount,
    .unmount = mayfs_vfs_unmount,
    .open = mayfs_vfs_open,
    .close = mayfs_vfs_close,
    .read = mayfs_vfs_read,
    .write = mayfs_vfs_write,
    .seek = mayfs_vfs_seek,
    .opendir = mayfs_vfs_opendir,
    .readdir = mayfs_vfs_readdir,
    .closedir = mayfs_vfs_closedir,
    .stat = mayfs_vfs_stat,
    .mkdir = mayfs_vfs_mkdir,
    .rmdir = mayfs_vfs_rmdir,
    .unlink = mayfs_vfs_unlink,
    .rename = mayfs_vfs_rename,
    .sync = mayfs_vfs_sync,
    .get_free_space = mayfs_vfs_get_free_space,
};
