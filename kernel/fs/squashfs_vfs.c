/* SutureOS: SquashFS filesystem driver for the unified VFS
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the real
 * SquashFS 4.0 core in kernel/fs/squashfs.c - the same shape as
 * kernel/fs/ext2_vfs.c and kernel/fs/fatfs_vfs.c. The driver name is
 * "squashfs" so `mount squashfs 1 /mnt/squash` (or vfs_mount("1", "/mnt/squash",
 * "squashfs")) finds it.
 *
 * READ-ONLY. kernel/vfs/vfs.h has no VFS_EROFS code, so every mutating call
 * returns VFS_EROFS, defined below as VFS_EPERM (the closest honest code the
 * VFS can report) after logging the rejection once.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "squashfs.h"

/* vfs.h has no EROFS; alias it so the intent is explicit at every call site. */
#define VFS_EROFS VFS_EPERM

/* ---- single mounted volume ---- */
static char g_mountpoint[VFS_MAX_PATH] = "";
static int  g_ram = 0;
static int  g_ro_logged = 0;

static void squashfs_ro_log(const char *what) {
    if (g_ro_logged) return;
    g_ro_logged = 1;
    console_printf("[squashfs] read-only filesystem: %s rejected\n", what);
}

/* Map core error codes to VFS error codes. The numeric values are pinned by
 * _Static_assert() in kernel/fs/squashfs.c. */
static int map_err(int e) {
    switch (e) {
        case SQUASHFS_OK:      return VFS_OK;
        case SQUASHFS_ENOENT:  return VFS_ENOTFOUND;   /* -2 */
        case SQUASHFS_ENOTDIR: return VFS_ENOTDIR;     /* -3 */
        case SQUASHFS_EISDIR:  return VFS_EISDIR;      /* -4 */
        case SQUASHFS_EINVAL:  return VFS_EINVAL;      /* -5 */
        case SQUASHFS_EIO:     return VFS_EIO;         /* -6 */
        case SQUASHFS_EROFS:   return VFS_EROFS;       /* -7 */
        case SQUASHFS_ENOMEM:  return VFS_ENOSPC;      /* -8 */
        case SQUASHFS_ENOTSUP: return VFS_EPERM;       /* -9 */
        case SQUASHFS_EBUSY:   return VFS_EBUSY;       /* -10 */
        default:               return VFS_ERR;         /* -11 ERANGE etc. */
    }
}

/* Device strings understood here:
 *   "ram0", "ram", "/dev/ram0"  -> the image registered with squashfs_register_ramdisk()
 *   "1", "sdb", "/dev/sdb"      -> block drive 1, LBA 0
 *   "1:2048", "sdb:2048"        -> block drive 1 starting at LBA 2048
 */
static int parse_device(const char *device, int *out_drive, uint32_t *out_lba,
                        int *out_ram) {
    const char *p = device;
    int drive = -1;
    uint32_t lba = 0;

    *out_ram = 0;
    *out_drive = 0;
    *out_lba = 0;
    if (!device) return VFS_EINVAL;
    if (strncmp(p, "/dev/", 5) == 0) p += 5;

    if ((p[0] == 'r' || p[0] == 'R') && (p[1] == 'a' || p[1] == 'A') &&
        (p[2] == 'm' || p[2] == 'M')) {
        *out_ram = 1;
        return VFS_OK;
    }
    if (p[0] == 's' && p[1] == 'd') {
        drive = (p[2] >= 'a' && p[2] <= 'z') ? (p[2] - 'a') : 0;
        p += (p[2] >= 'a' && p[2] <= 'z') ? 3 : 2;
    } else if (*p >= '0' && *p <= '9') {
        drive = 0;
        while (*p >= '0' && *p <= '9') {
            drive = drive * 10 + (*p - '0');
            p++;
        }
    }
    if (*p == ':' || *p == ',' || *p == '+') {
        p++;
        while (*p >= '0' && *p <= '9') {
            lba = lba * 10u + (uint32_t)(*p - '0');
            p++;
        }
    }
    *out_drive = (drive < 0) ? 0 : drive;
    *out_lba = lba;
    return VFS_OK;
}

/* VFS path -> volume-relative path ("/HELLO.TXT"), stripping the mountpoint. */
static int vol_path(const char *path, char *out, uint32_t cap) {
    if (!path || !out || cap == 0) return VFS_EINVAL;
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

/* ==================== driver operations ==================== */

static int squashfs_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    int drive = 0, ram = 0, r;
    uint32_t lba = 0;

    if (!mountpoint) return VFS_EINVAL;
    r = parse_device(device, &drive, &lba, &ram);
    if (r != VFS_OK) return r;

    if (ram) {
        if (!squashfs_ramdisk_registered()) {
            console_puts("[squashfs] mount failed: no ramdisk registered "
                         "(call squashfs_register_ramdisk first)\n");
            return VFS_EINVAL;
        }
        r = squashfs_mount_ramdisk();
    } else {
        r = squashfs_mount(drive, lba);
    }
    if (r != SQUASHFS_OK) return map_err(r);

    g_ram = ram;
    strncpy(g_mountpoint, mountpoint, sizeof(g_mountpoint) - 1);
    g_mountpoint[sizeof(g_mountpoint) - 1] = '\0';
    if (fs_data) *fs_data = (void *)(uintptr_t)(ram ? 0x100u : (uint32_t)drive);
    return VFS_OK;
}

static int squashfs_vfs_unmount(const char *mountpoint) {
    (void)mountpoint;
    squashfs_unmount();
    g_mountpoint[0] = '\0';
    g_ram = 0;
    return VFS_OK;
}

static int squashfs_vfs_open(void *fs_data, const char *path, int flags,
                             void **file_handle) {
    char vp[VFS_MAX_PATH];
    struct squashfs_file *fh = NULL;
    /* VFS_O_RDWR (0x3) contains VFS_O_RDONLY's bit, so test the access-mode
     * value instead of AND-ing the three flags together. */
    int acc = flags & VFS_O_RDWR;
    int r;

    (void)fs_data;
    if (!file_handle) return VFS_EINVAL;
    if (acc == VFS_O_WRONLY || acc == VFS_O_RDWR ||
        (flags & (VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0) {
        squashfs_ro_log("open for writing");
        return VFS_EROFS;
    }
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = squashfs_open(vp, &fh);
    if (r != SQUASHFS_OK) return map_err(r);
    *file_handle = fh;
    return VFS_OK;
}

static int squashfs_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return map_err(squashfs_close((struct squashfs_file *)file_handle));
}

static int squashfs_vfs_read(void *file_handle, void *buf, uint32_t size,
                             uint32_t offset) {
    int n;
    if (!file_handle) return VFS_EINVAL;
    n = squashfs_file_read((struct squashfs_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = bytes read */
}

static int squashfs_vfs_write(void *file_handle, const void *buf, uint32_t size,
                              uint32_t offset) {
    (void)file_handle; (void)buf; (void)size; (void)offset;
    squashfs_ro_log("write");
    return VFS_EROFS;
}

static int squashfs_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* The VFS core owns the file position and always passes the absolute
     * target offset (SEEK_SET semantics); SquashFS reads are random access. */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int squashfs_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    char vp[VFS_MAX_PATH];
    struct squashfs_dir *d = NULL;
    int r;

    (void)fs_data;
    if (!dir_handle) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = squashfs_opendir(vp, &d);
    if (r != SQUASHFS_OK) return map_err(r);
    *dir_handle = d;
    return VFS_OK;
}

static int squashfs_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                                vfs_node_type_t *type) {
    uint32_t len = 0;
    int is_dir = 0;
    int r;

    if (!dir_handle || !name) return VFS_EINVAL;
    r = squashfs_readdir((struct squashfs_dir *)dir_handle, name, VFS_MAX_NAME,
                         &len, &is_dir);
    if (r == SQUASHFS_ENOENT) return VFS_ENOTFOUND;      /* end of directory */
    if (r != SQUASHFS_OK) return map_err(r);
    if (name_len) *name_len = len;
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int squashfs_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return map_err(squashfs_closedir((struct squashfs_dir *)dir_handle));
}

static int squashfs_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    char vp[VFS_MAX_PATH];
    struct squashfs_stat ss;
    int r;

    (void)fs_data;
    if (!st) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = squashfs_stat(vp, &ss);
    if (r != SQUASHFS_OK) return map_err(r);

    memset(st, 0, sizeof(*st));
    if (ss.is_dir) {
        st->type = VFS_NODE_DIR;
    } else if (ss.is_symlink) {
        st->type = VFS_NODE_SYMLINK;
    } else {
        st->type = VFS_NODE_FILE;
    }
    st->size = ss.size;
    st->mode = ss.mode & 0xFFFu;         /* permission bits only */
    st->uid = ss.uid;
    st->gid = ss.gid;
    st->atime = ss.mtime;
    st->mtime = ss.mtime;
    st->ctime = ss.mtime;
    return VFS_OK;
}

static int squashfs_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)path; (void)mode;
    squashfs_ro_log("mkdir");
    return VFS_EROFS;
}

static int squashfs_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    squashfs_ro_log("rmdir");
    return VFS_EROFS;
}

static int squashfs_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    squashfs_ro_log("unlink");
    return VFS_EROFS;
}

static int squashfs_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    squashfs_ro_log("rename");
    return VFS_EROFS;
}

static int squashfs_vfs_sync(void *fs_data) {
    (void)fs_data;
    return VFS_OK;                        /* nothing to flush: read-only */
}

static int squashfs_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                       uint64_t *total_bytes) {
    (void)fs_data;
    if (!squashfs_is_mounted()) return VFS_EBUSY;
    /* SquashFS images are packed read-only volumes: there is no free space. */
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) {
        *total_bytes = (uint64_t)g_squashfs_fs.sb.bytes_used;
    }
    return VFS_OK;
}

/* ==================== driver export ==================== */

const vfs_fs_driver_t squashfs_driver = {
    .name = "squashfs",
    .mount = squashfs_vfs_mount,
    .unmount = squashfs_vfs_unmount,
    .open = squashfs_vfs_open,
    .close = squashfs_vfs_close,
    .read = squashfs_vfs_read,
    .write = squashfs_vfs_write,
    .seek = squashfs_vfs_seek,
    .opendir = squashfs_vfs_opendir,
    .readdir = squashfs_vfs_readdir,
    .closedir = squashfs_vfs_closedir,
    .stat = squashfs_vfs_stat,
    .mkdir = squashfs_vfs_mkdir,
    .rmdir = squashfs_vfs_rmdir,
    .unlink = squashfs_vfs_unlink,
    .rename = squashfs_vfs_rename,
    .sync = squashfs_vfs_sync,
    .get_free_space = squashfs_vfs_get_free_space,
};
