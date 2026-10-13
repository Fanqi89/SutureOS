/* SutureOS: FAT32 filesystem driver for the unified VFS
 * Adapter between vfs_fs_driver_t and the real FAT32 core (fatfs.c).
 * Driver name is "fat32" so `mount fat32 0 /mnt/x` finds it.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "fatfs.h"

/* ---- mountpoint prefix handling (one volume) ---- */
static char g_mountpoint[VFS_MAX_PATH] = "";
static int  g_drive = -1;

/* Parse a block drive number from the device string:
 * whole-string decimal ("0"), else the last digit anywhere ("ata0",
 * "/dev/sda"->0), else 0. */
static int parse_drive(const char *device) {
    if (!device) return 0;
    uint32_t i = 0;
    int all_digits = 1;
    for (const char *p = device; *p; p++) {
        if (*p < '0' || *p > '9') { all_digits = 0; break; }
    }
    if (all_digits && device[0]) {
        int v = 0;
        for (const char *p = device; *p; p++) v = v * 10 + (*p - '0');
        return v;
    }
    /* last digit anywhere; letter a..z maps to 0..25 only if no digit */
    int last_digit = -1;
    for (const char *p = device; *p; p++) {
        if (*p >= '0' && *p <= '9') last_digit = *p - '0';
    }
    if (last_digit >= 0) return last_digit;
    for (const char *p = device; *p; p++) {
        if (*p >= 'a' && *p <= 'z') i = (uint32_t)(*p - 'a');
    }
    return (int)i;
}

/* Map core error codes to VFS error codes */
static int map_err(int e) {
    switch (e) {
        case FATFS_OK:          return VFS_OK;
        case FATFS_ENOENT:      return VFS_ENOTFOUND;
        case FATFS_ENOTDIR:     return VFS_ENOTDIR;
        case FATFS_EISDIR:      return VFS_EISDIR;
        case FATFS_ENOSPC:      return VFS_ENOSPC;
        case FATFS_ENOTEMPTY:   return VFS_ECONFLICT;
        case FATFS_EBUSY:       return VFS_EBUSY;
        case FATFS_EINVAL:      return VFS_EINVAL;
        case FATFS_EIO:         return VFS_EIO;
        case FATFS_ENAMETOOLONG:return VFS_EINVAL;
        default:                return VFS_ERR;
    }
}

/* VFS path -> volume-relative path ("/HELLO.TXT"). */
static int vol_path(const char *path, char *out, uint32_t cap) {
    if (!path || !out) return VFS_EINVAL;
    const char *rel = path;
    size_t mplen = strlen(g_mountpoint);

    if (mplen > 1) {
        if (strncmp(path, g_mountpoint, mplen) != 0) return VFS_ENOTFOUND;
        rel = path + mplen;
        if (*rel == '\0') rel = "/";
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

static int fatfs_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    if (!mountpoint) return VFS_EINVAL;
    int drv = parse_drive(device);

    int ret = fatfs_mount((uint8_t)drv, NULL);
    if (ret != FATFS_OK) return map_err(ret);

    g_drive = drv;
    strncpy(g_mountpoint, mountpoint, sizeof(g_mountpoint) - 1);
    g_mountpoint[sizeof(g_mountpoint) - 1] = '\0';
    if (fs_data) *fs_data = (void *)(uintptr_t)drv;
    return VFS_OK;
}

static int fatfs_vfs_unmount(const char *mountpoint) {
    (void)mountpoint;
    int r = fatfs_unmount(g_drive);
    if (r == FATFS_OK) {
        g_drive = -1;
        g_mountpoint[0] = '\0';
    }
    return map_err(r);
}

static int fatfs_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    (void)fs_data;
    if (!file_handle) return VFS_EINVAL;

    char vp[VFS_MAX_PATH];
    int r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    int create = (flags & VFS_O_CREAT) ? 1 : 0;
    fatfs_file_t *fh = NULL;
    r = fatfs_open(vp, create, &fh);
    if (r != FATFS_OK) return map_err(r);

    if (flags & VFS_O_TRUNC) {
        int t = fatfs_truncate(fh, 0);
        if (t != FATFS_OK) {
            fatfs_close(fh);
            return map_err(t);
        }
    }
    *file_handle = fh;
    return VFS_OK;
}

static int fatfs_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return map_err(fatfs_close((fatfs_file_t *)file_handle));
}

static int fatfs_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    if (!file_handle) return VFS_EINVAL;
    int n = fatfs_read((fatfs_file_t *)file_handle, offset, buf, size);
    return (n < 0) ? map_err(n) : n;
}

static int fatfs_vfs_write(void *file_handle, const void *buf, uint32_t size, uint32_t offset) {
    if (!file_handle) return VFS_EINVAL;
    int n = fatfs_write((fatfs_file_t *)file_handle, offset, buf, size);
    return (n < 0) ? map_err(n) : n;
}

static int fatfs_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* The VFS core owns the file position and always calls us with the
     * absolute target offset (SEEK_SET semantics). Random access is
     * inherent in the read/write offset parameter, so nothing to do. */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int fatfs_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    (void)fs_data;
    if (!dir_handle) return VFS_EINVAL;

    char vp[VFS_MAX_PATH];
    int r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    fatfs_dir_t *d = NULL;
    r = fatfs_opendir(vp, &d);
    if (r != FATFS_OK) return map_err(r);
    *dir_handle = d;
    return VFS_OK;
}

static int fatfs_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                             vfs_node_type_t *type) {
    if (!dir_handle || !name) return VFS_EINVAL;

    uint32_t size = 0;
    int is_dir = 0;
    int r = fatfs_readdir((fatfs_dir_t *)dir_handle, name, VFS_MAX_NAME,
                          &size, &is_dir);
    if (r == FATFS_ENOTFOUND) return VFS_ENOTFOUND;
    if (r != FATFS_OK) return map_err(r);

    if (name_len) *name_len = (uint32_t)strlen(name);
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int fatfs_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return map_err(fatfs_closedir((fatfs_dir_t *)dir_handle));
}

static int fatfs_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    (void)fs_data;
    if (!st) return VFS_EINVAL;

    char vp[VFS_MAX_PATH];
    int r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    uint32_t size = 0;
    int is_dir = 0;
    r = fatfs_stat(vp, &size, &is_dir);
    if (r != FATFS_OK) return map_err(r);

    memset(st, 0, sizeof(*st));
    st->type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    st->size = size;
    st->mode = is_dir ? 0755 : 0644;
    return VFS_OK;
}

static int fatfs_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)mode;
    char vp[VFS_MAX_PATH];
    int r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(fatfs_mkdir(vp));
}

static int fatfs_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data;
    char vp[VFS_MAX_PATH];
    int r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(fatfs_rmdir(vp));
}

static int fatfs_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    char vp[VFS_MAX_PATH];
    int r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(fatfs_unlink(vp));
}

static int fatfs_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    /* Honest: FAT32 rename is not implemented in the core (no LFN support
     * means name rewriting needs a fresh dirent; out of current scope). */
    console_puts("[fat32] rename not supported\n");
    return VFS_EPERM;
}

static int fatfs_vfs_sync(void *fs_data) {
    (void)fs_data;
    return map_err(fatfs_sync());
}

static int fatfs_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                    uint64_t *total_bytes) {
    (void)fs_data;
    return map_err(fatfs_get_free_space(free_bytes, total_bytes));
}

const vfs_fs_driver_t fatfs_driver = {
    .name = "fat32",
    .mount = fatfs_vfs_mount,
    .unmount = fatfs_vfs_unmount,
    .open = fatfs_vfs_open,
    .close = fatfs_vfs_close,
    .read = fatfs_vfs_read,
    .write = fatfs_vfs_write,
    .seek = fatfs_vfs_seek,
    .opendir = fatfs_vfs_opendir,
    .readdir = fatfs_vfs_readdir,
    .closedir = fatfs_vfs_closedir,
    .stat = fatfs_vfs_stat,
    .mkdir = fatfs_vfs_mkdir,
    .rmdir = fatfs_vfs_rmdir,
    .unlink = fatfs_vfs_unlink,
    .rename = fatfs_vfs_rename,
    .sync = fatfs_vfs_sync,
    .get_free_space = fatfs_vfs_get_free_space,
};
