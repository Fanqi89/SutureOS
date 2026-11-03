/* SutureOS: FAT32 filesystem driver for the unified VFS
 * Adapter between vfs_fs_driver_t and the real FAT32 core (fatfs.c).
 * Driver name is "fat32" so `mount fat32 0 /mnt/x` finds it.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "fatfs.h"

/* ---- mountpoint handling (one entry per mounted volume, v0.24.0) ---- */
#define FATFS_VFS_MAX_MOUNTS 4
struct fat_mnt {
    int  used;
    char mp[VFS_MAX_PATH];
    int  drive;
    int  vol;
};
static struct fat_mnt g_mnts[FATFS_VFS_MAX_MOUNTS];

static struct fat_mnt *mnt_find_by_point(const char *mountpoint) {
    if (!mountpoint) return NULL;
    for (int i = 0; i < FATFS_VFS_MAX_MOUNTS; i++)
        if (g_mnts[i].used && strcmp(g_mnts[i].mp, mountpoint) == 0)
            return &g_mnts[i];
    return NULL;
}

/* Parse a block drive number from the device string:
 * whole-string decimal ("0"), else the last digit anywhere ("ata0",
 * "/dev/sda"->0), else 0. Only the part before an optional ":<offset>" is
 * considered, so "0:2048" means drive 0 at partition offset 2048. */
static int parse_drive(const char *device) {
    char head[64];
    uint32_t n = 0;
    if (!device) return 0;
    while (device[n] && device[n] != ':' && n + 1 < sizeof(head)) {
        head[n] = device[n];
        n++;
    }
    head[n] = '\0';
    device = head;
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
        case FATFS_EEXIST:      return VFS_ECONFLICT;   /* rename target taken */
        default:                return VFS_ERR;
    }
}

/* VFS path -> volume-relative path ("/HELLO.TXT"). */
/* VFS path -> volume-relative path ("/HELLO.TXT") for one mount. */
static int vol_path(struct fat_mnt *m, const char *path, char *out, uint32_t cap) {
    if (!m || !path || !out) return VFS_EINVAL;
    const char *rel = path;
    size_t mplen = strlen(m->mp);

    if (mplen > 1) {
        if (strncmp(path, m->mp, mplen) != 0) return VFS_ENOTFOUND;
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

/* Optional explicit partition offset in the device string: "0:2048" (or
 * "ata0:2048") means "the FAT32 partition that starts at LBA 2048". Without it
 * the driver auto-detects: LBA 0 (superfloppy) first, then the MBR partitions. */
static int parse_offset(const char *device, uint32_t *out) {
    if (!device || !out) return 0;
    const char *p = device;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
    *out = v;
    return 1;
}

static int fatfs_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    if (!mountpoint) return VFS_EINVAL;
    int drv = parse_drive(device);
    uint32_t start_lba = 0;
    const int explicit_offset = parse_offset(device, &start_lba);

    struct fat_mnt *m = NULL;
    for (int i = 0; i < FATFS_VFS_MAX_MOUNTS; i++)
        if (!g_mnts[i].used) { m = &g_mnts[i]; break; }
    if (!m) return VFS_ENOSPC;

    int slot = explicit_offset
             ? fatfs_mount_at((uint8_t)drv, NULL, start_lba)
             : fatfs_mount_auto((uint8_t)drv);
    if (slot < 0) return map_err(slot);        /* mount returns the volume slot */

    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->drive = drv;
    m->vol = slot;
    strncpy(m->mp, mountpoint, sizeof(m->mp) - 1);
    m->mp[sizeof(m->mp) - 1] = '\0';
    if (fs_data) *fs_data = m;
    console_printf("[fat32] vfs mount drive=%d at %s (volume slot %d)\n",
                   (int64_t)drv, m->mp, (int64_t)slot);
    return VFS_OK;
}

static int fatfs_vfs_unmount(const char *mountpoint) {
    struct fat_mnt *m = mnt_find_by_point(mountpoint);
    if (!m) return VFS_ENOTFOUND;
    int r = fatfs_unmount(m->drive);
    if (r == FATFS_OK) memset(m, 0, sizeof(*m));
    return map_err(r);
}

static int fatfs_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    if (!file_handle) return VFS_EINVAL;

    char vp[VFS_MAX_PATH];
    int r = vol_path(m, path, vp, sizeof(vp));
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
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    if (!dir_handle) return VFS_EINVAL;

    char vp[VFS_MAX_PATH];
    int r = vol_path(m, path, vp, sizeof(vp));
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
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    if (!st) return VFS_EINVAL;

    char vp[VFS_MAX_PATH];
    int r = vol_path(m, path, vp, sizeof(vp));
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
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data; (void)mode;
    char vp[VFS_MAX_PATH];
    int r = vol_path(m, path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(fatfs_mkdir(vp));
}

static int fatfs_vfs_rmdir(void *fs_data, const char *path) {
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    char vp[VFS_MAX_PATH];
    int r = vol_path(m, path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(fatfs_rmdir(vp));
}

static int fatfs_vfs_unlink(void *fs_data, const char *path) {
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    char vp[VFS_MAX_PATH];
    int r = vol_path(m, path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(fatfs_unlink(vp));
}

static int fatfs_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    char vo[VFS_MAX_PATH], vn[VFS_MAX_PATH];
    int r = vol_path(m, oldpath, vo, sizeof(vo));
    if (r != VFS_OK) return r;
    r = vol_path(m, newpath, vn, sizeof(vn));
    if (r != VFS_OK) return r;
    /* The core rewrites the 8.3 name field in place; same-directory only. */
    return map_err(fatfs_rename(vo, vn));
}

static int fatfs_vfs_sync(void *fs_data) {
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
    (void)fs_data;
    return map_err(fatfs_sync());
}

static int fatfs_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                    uint64_t *total_bytes) {
    struct fat_mnt *m = (struct fat_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    (void)fatfs_select_volume(m->vol);
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
