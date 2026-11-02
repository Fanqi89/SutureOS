/* SutureOS: ext2 filesystem driver for the unified VFS
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the real ext2
 * core in kernel/fs/ext2.c - the same shape as kernel/fs/fatfs_vfs.c.
 * Driver name is "ext2" so `mount ext2 0 /mnt/ext2` finds it.
 *
 * READ-WRITE since the B3 port batch: the volume is mounted read-write by
 * default and create / write / truncate (O_TRUNC) / mkdir / rmdir / unlink /
 * rename / sync all reach the ext2 core. The VFS driver interface has no
 * separate truncate() entry, so a truncate reaches ext2 through
 * open(O_TRUNC) - that is exactly what vfs_write_file() and the
 * truncate(2) syscall wrapper use.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "ext2.h"

/* ---- single mounted volume ---- */
static char g_mountpoint[VFS_MAX_PATH] = "";
static int  g_ram = 0;

/* Map core error codes to VFS error codes. */
static int map_err(int e) {
    switch (e) {
        case EXT2_OK:       return VFS_OK;
        case EXT2_ENOENT:   return VFS_ENOTFOUND;
        case EXT2_ENOTDIR:  return VFS_ENOTDIR;
        case EXT2_EISDIR:   return VFS_EISDIR;
        case EXT2_EINVAL:   return VFS_EINVAL;
        case EXT2_EIO:      return VFS_EIO;
        case EXT2_EROFS:    return VFS_EPERM;
        case EXT2_ENOMEM:   return VFS_ENOSPC;
        case EXT2_ENOSPC:   return VFS_ENOSPC;
        case EXT2_EEXIST:   return VFS_ECONFLICT;
        case EXT2_ENOTEMPTY: return VFS_ECONFLICT;
        case EXT2_ENOTSUP:  return VFS_EPERM;
        case EXT2_EBUSY:    return VFS_EBUSY;
        default:            return VFS_ERR;
    }
}

/* Device strings understood here:
 *   "ram0", "ram", "/dev/ram0"  -> the image registered with ext2_register_ramdisk()
 *   "0", "sda", "/dev/sda"      -> block drive 0, LBA 0
 *   "0:2048", "sda:2048"        -> block drive 0 starting at LBA 2048
 */
static int parse_device(const char *device, int *out_drive, uint32_t *out_lba, int *out_ram) {
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

static int ext2_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    int drive = 0, ram = 0, r;
    uint32_t lba = 0;

    if (!mountpoint) return VFS_EINVAL;
    r = parse_device(device, &drive, &lba, &ram);
    if (r != VFS_OK) return r;

    if (ram) {
        if (!ext2_ramdisk_registered()) {
            console_puts("[ext2] mount failed: no ramdisk registered "
                         "(call ext2_register_ramdisk first)\n");
            return VFS_EINVAL;
        }
        r = ext2_mount_ramdisk();
    } else {
        r = ext2_mount(drive, lba);
    }
    if (r != EXT2_OK) return map_err(r);

    g_ram = ram;
    strncpy(g_mountpoint, mountpoint, sizeof(g_mountpoint) - 1);
    g_mountpoint[sizeof(g_mountpoint) - 1] = '\0';
    if (fs_data) *fs_data = (void *)(uintptr_t)(ram ? 0x100u : (uint32_t)drive);
    return VFS_OK;
}

static int ext2_vfs_unmount(const char *mountpoint) {
    (void)mountpoint;
    ext2_sync();                     /* push the counters out before leaving */
    ext2_unmount();
    g_mountpoint[0] = '\0';
    g_ram = 0;
    return VFS_OK;
}

static int ext2_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    char vp[VFS_MAX_PATH];
    struct ext2_file *fh = NULL;
    int acc = flags & VFS_O_RDWR;      /* 0 = unspecified, 1 RDONLY, 2 WRONLY, 3 RDWR */
    int wants_write;
    int r;

    (void)fs_data;
    if (!file_handle) return VFS_EINVAL;
    /* VFS_O_RDWR (0x3) contains VFS_O_RDONLY's bit, so test the access mode
     * value instead of AND-ing the three flags together. */
    wants_write = (acc == VFS_O_WRONLY || acc == VFS_O_RDWR ||
                   (flags & (VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0);

    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    if (wants_write) {
        r = ext2_file_open(vp,
                           (flags & VFS_O_CREAT) ? 1 : 0,
                           (flags & VFS_O_TRUNC) ? 1 : 0,
                           (flags & VFS_O_APPEND) ? 1 : 0,
                           &fh);
    } else {
        r = ext2_open(vp, &fh);
    }
    if (r != EXT2_OK) return map_err(r);
    *file_handle = fh;
    return VFS_OK;
}

static int ext2_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return map_err(ext2_close((struct ext2_file *)file_handle));
}

static int ext2_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    int n;
    if (!file_handle) return VFS_EINVAL;
    n = ext2_file_read((struct ext2_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = bytes read */
}

static int ext2_vfs_write(void *file_handle, const void *buf, uint32_t size, uint32_t offset) {
    int n;
    if (!file_handle) return VFS_EINVAL;
    n = ext2_file_write((struct ext2_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = bytes written */
}

static int ext2_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* The VFS core owns the file position and always passes the absolute
     * target offset (SEEK_SET semantics); ext2 reads are random access. */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int ext2_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    char vp[VFS_MAX_PATH];
    struct ext2_dir *d = NULL;
    int r;

    (void)fs_data;
    if (!dir_handle) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = ext2_opendir(vp, &d);
    if (r != EXT2_OK) return map_err(r);
    *dir_handle = d;
    return VFS_OK;
}

static int ext2_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                            vfs_node_type_t *type) {
    uint32_t len = 0;
    int is_dir = 0;
    int r;

    if (!dir_handle || !name) return VFS_EINVAL;
    r = ext2_readdir((struct ext2_dir *)dir_handle, name, VFS_MAX_NAME, &len, &is_dir);
    if (r == EXT2_ENOENT) return VFS_ENOTFOUND;      /* end of directory */
    if (r != EXT2_OK) return map_err(r);
    if (name_len) *name_len = len;
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int ext2_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return map_err(ext2_closedir((struct ext2_dir *)dir_handle));
}

static int ext2_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    char vp[VFS_MAX_PATH];
    struct ext2_stat es;
    int r;

    (void)fs_data;
    if (!st) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = ext2_stat(vp, &es);
    if (r != EXT2_OK) return map_err(r);

    memset(st, 0, sizeof(*st));
    if (es.is_dir) {
        st->type = VFS_NODE_DIR;
    } else if (es.is_symlink) {
        st->type = VFS_NODE_SYMLINK;
    } else {
        st->type = VFS_NODE_FILE;
    }
    st->size = es.size;
    st->mode = es.mode & 0xFFFu;         /* permission bits only */
    st->uid = es.uid;
    st->gid = es.gid;
    st->atime = es.atime;
    st->mtime = es.mtime;
    st->ctime = es.ctime;
    return VFS_OK;
}

static int ext2_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    char vp[VFS_MAX_PATH];
    int r;

    (void)fs_data;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(ext2_mkdir(vp, mode & 0xFFFu));
}

static int ext2_vfs_rmdir(void *fs_data, const char *path) {
    char vp[VFS_MAX_PATH];
    int r;

    (void)fs_data;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(ext2_rmdir(vp));
}

static int ext2_vfs_unlink(void *fs_data, const char *path) {
    char vp[VFS_MAX_PATH];
    int r;

    (void)fs_data;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    return map_err(ext2_unlink(vp));
}

static int ext2_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    char ovp[VFS_MAX_PATH], nvp[VFS_MAX_PATH];
    int r;

    (void)fs_data;
    r = vol_path(oldpath, ovp, sizeof(ovp));
    if (r != VFS_OK) return r;
    r = vol_path(newpath, nvp, sizeof(nvp));
    if (r != VFS_OK) return r;
    return map_err(ext2_rename(ovp, nvp));
}

static int ext2_vfs_sync(void *fs_data) {
    (void)fs_data;
    if (!ext2_is_mounted()) return VFS_EBUSY;
    return map_err(ext2_sync());
}

static int ext2_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                   uint64_t *total_bytes) {
    uint32_t free_blocks = 0, free_inodes = 0;
    (void)fs_data;
    if (!ext2_is_mounted()) return VFS_EBUSY;
    /* measure instead of trusting the cached counters: df must never lie */
    if (ext2_count_free(&free_blocks, &free_inodes) == EXT2_OK) {
        if (free_bytes) *free_bytes = (uint64_t)free_blocks * g_ext2_fs.block_size;
    } else if (free_bytes) {
        *free_bytes = (uint64_t)g_ext2_fs.sb.s_free_blocks_count * g_ext2_fs.block_size;
    }
    if (total_bytes) {
        *total_bytes = (uint64_t)g_ext2_fs.blocks_count * g_ext2_fs.block_size;
    }
    return VFS_OK;
}

/* ==================== VFS-layer write self-test ==================== */

/* Names used by the probe below; the host verifier checks that they are gone. */
#define EXT2_VFS_PROBE_FILE   "/VFSPROBE.BIN"
#define EXT2_VFS_PROBE_DIR    "/VFSDIR"
#define EXT2_VFS_PROBE_MOVED  "/VFSDIR/MOVED.BIN"

static int ext2_vfs_probe_fail(const char *what) {
    console_printf("[ext2] vfs write self-test FAIL (%s)\n", what);
    return EXT2_ERR;
}

/* Build "<mountpoint><suffix>" into out (the mountpoint is "/mnt/ext2" in the
 * default boot, but never assume it). */
static int ext2_vfs_probe_path(const char *suffix, char *out, uint32_t cap) {
    size_t mp = strlen(g_mountpoint);
    int n;
    if (cap == 0) return VFS_EINVAL;
    if (mp == 0) return VFS_EINVAL;
    n = snprintf(out, cap, "%s%s", g_mountpoint, suffix);
    if (n <= 0 || (uint32_t)n >= cap) return VFS_EINVAL;
    return VFS_OK;
}

/* Round trip through the VFS adapter only - no direct ext2 core call. Proves
 * that ext2_vfs.c really is read-write: O_CREAT/O_TRUNC create and truncate the
 * file, write/read go through the driver's write/read entry points, and
 * mkdir/rename/unlink/rmdir reach the directory code. */
int ext2_vfs_selftest(void) {
    char file_p[VFS_MAX_PATH], dir_p[VFS_MAX_PATH], moved_p[VFS_MAX_PATH];
    uint8_t *buf = NULL;
    uint32_t bs = g_ext2_fs.block_size, total, i, k;
    vfs_stat_t st;
    int open_ok = 0, write_ok = 0, read_ok = 0, truncate_ok = 0;
    int mkdir_ok = 0, rename_ok = 0, unlink_ok = 0, rmdir_ok = 0;
    int fd = -1, r;

    if (!ext2_is_mounted() || g_mountpoint[0] == '\0') {
        console_puts("[ext2] vfs write self-test skipped (no ext2 mount point)\n");
        return EXT2_OK;
    }
    /* Destructive probe: only run it on the generated test volume, i.e. when the
     * core write self-test has already left its /WRT_KEEP.BIN artifact behind. */
    {
        struct ext2_stat es;
        if (ext2_stat(EXT2_KEEP_NAME, &es) != EXT2_OK) {
            console_puts("[ext2] vfs write self-test skipped "
                         "(not the generated test image)\n");
            return EXT2_OK;
        }
    }
    if (ext2_vfs_probe_path(EXT2_VFS_PROBE_FILE, file_p, sizeof(file_p)) != VFS_OK ||
        ext2_vfs_probe_path(EXT2_VFS_PROBE_DIR, dir_p, sizeof(dir_p)) != VFS_OK ||
        ext2_vfs_probe_path(EXT2_VFS_PROBE_MOVED, moved_p, sizeof(moved_p)) != VFS_OK) {
        return ext2_vfs_probe_fail("cannot build the probe paths");
    }

    total = 3u * bs;
    buf = (uint8_t *)lemis_heap_alloc(bs);
    if (!buf) return ext2_vfs_probe_fail("out of memory");

    /* leftovers from an interrupted earlier boot */
    if (vfs_stat(file_p, &st) == VFS_OK) vfs_unlink(file_p);
    if (vfs_stat(moved_p, &st) == VFS_OK) vfs_unlink(moved_p);
    if (vfs_stat(dir_p, &st) == VFS_OK) vfs_rmdir(dir_p);

    /* ---- create (O_CREAT|O_WRONLY|O_TRUNC) and write 3 blocks ---- */
    fd = vfs_open(file_p, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) { ext2_vfs_probe_fail("open(O_CREAT) failed"); goto done; }
    open_ok = 1;
    for (i = 0; i < 3; i++) {
        for (k = 0; k < bs; k++) {
            buf[k] = (uint8_t)((((uint32_t)i * bs + k) * 53u + 7u) & 0xFFu);
        }
        r = vfs_write(fd, buf, bs);
        if (r != (int)bs) { ext2_vfs_probe_fail("write failed"); goto done; }
    }
    write_ok = 1;
    r = vfs_close(fd);
    fd = -1;
    if (r != VFS_OK) { ext2_vfs_probe_fail("close after write failed"); goto done; }

    /* ---- read it back through the VFS ---- */
    if (vfs_stat(file_p, &st) != VFS_OK || st.size != total) {
        ext2_vfs_probe_fail("stat after write failed");
        goto done;
    }
    fd = vfs_open(file_p, VFS_O_RDONLY);
    if (fd < 0) { ext2_vfs_probe_fail("open(O_RDONLY) failed"); goto done; }
    read_ok = 1;
    for (i = 0; i < 3 && read_ok; i++) {
        r = vfs_read(fd, buf, bs);
        if (r != (int)bs) { read_ok = 0; break; }
        for (k = 0; k < bs; k++) {
            if (buf[k] != (uint8_t)((((uint32_t)i * bs + k) * 53u + 7u) & 0xFFu)) {
                read_ok = 0;
                break;
            }
        }
    }
    vfs_close(fd);
    fd = -1;
    if (!read_ok) { ext2_vfs_probe_fail("readback mismatch"); goto done; }

    /* ---- truncate through open(O_TRUNC) ---- */
    fd = vfs_open(file_p, VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) { ext2_vfs_probe_fail("open(O_TRUNC) failed"); goto done; }
    vfs_close(fd);
    fd = -1;
    if (vfs_stat(file_p, &st) != VFS_OK || st.size != 0) {
        ext2_vfs_probe_fail("truncate did not reach 0 bytes");
        goto done;
    }
    truncate_ok = 1;

    /* ---- mkdir / rename / unlink / rmdir ---- */
    if (vfs_mkdir(dir_p, 0755) != VFS_OK) { ext2_vfs_probe_fail("mkdir failed"); goto done; }
    mkdir_ok = 1;
    if (vfs_rename(file_p, moved_p) != VFS_OK) {
        ext2_vfs_probe_fail("rename failed");
        goto done;
    }
    if (vfs_stat(file_p, &st) == VFS_OK) {
        ext2_vfs_probe_fail("rename left the source name behind");
        goto done;
    }
    if (vfs_stat(moved_p, &st) != VFS_OK || st.type != VFS_NODE_FILE) {
        ext2_vfs_probe_fail("renamed file is missing");
        goto done;
    }
    rename_ok = 1;
    if (vfs_unlink(moved_p) != VFS_OK) { ext2_vfs_probe_fail("unlink failed"); goto done; }
    unlink_ok = 1;
    if (vfs_rmdir(dir_p) != VFS_OK) { ext2_vfs_probe_fail("rmdir failed"); goto done; }
    rmdir_ok = 1;
    if (vfs_sync(g_mountpoint) != VFS_OK) {
        ext2_vfs_probe_fail("sync failed");
        goto done;
    }

done:
    if (fd >= 0) vfs_close(fd);
    if (buf) lemis_heap_free(buf);

    if (open_ok && write_ok && read_ok && truncate_ok && mkdir_ok && rename_ok &&
        unlink_ok && rmdir_ok) {
        console_puts("[ext2] vfs write self-test PASS (open=1 write=1 read=1 "
                     "truncate=1 mkdir=1 rename=1 unlink=1 rmdir=1)\n");
        return EXT2_OK;
    }
    console_printf("[ext2] vfs write self-test FAIL (open=%u write=%u read=%u "
                   "truncate=%u mkdir=%u rename=%u unlink=%u rmdir=%u)\n",
                   (uint64_t)open_ok, (uint64_t)write_ok, (uint64_t)read_ok,
                   (uint64_t)truncate_ok, (uint64_t)mkdir_ok, (uint64_t)rename_ok,
                   (uint64_t)unlink_ok, (uint64_t)rmdir_ok);
    return EXT2_ERR;
}

/* ==================== driver export ==================== */

const vfs_fs_driver_t ext2_driver = {
    .name = "ext2",
    .mount = ext2_vfs_mount,
    .unmount = ext2_vfs_unmount,
    .open = ext2_vfs_open,
    .close = ext2_vfs_close,
    .read = ext2_vfs_read,
    .write = ext2_vfs_write,
    .seek = ext2_vfs_seek,
    .opendir = ext2_vfs_opendir,
    .readdir = ext2_vfs_readdir,
    .closedir = ext2_vfs_closedir,
    .stat = ext2_vfs_stat,
    .mkdir = ext2_vfs_mkdir,
    .rmdir = ext2_vfs_rmdir,
    .unlink = ext2_vfs_unlink,
    .rename = ext2_vfs_rename,
    .sync = ext2_vfs_sync,
    .get_free_space = ext2_vfs_get_free_space,
};
