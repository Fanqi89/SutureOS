/* SutureOS: ext4 filesystem driver for the unified VFS (READ-ONLY)
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the ext4 core in
 * kernel/fs/ext4.c - the same shape as kernel/fs/ext2_vfs.c. The driver name is
 * "ext4", so `mount ext4 1 /mnt/e4` finds it (the VFS turns the device string
 * "1" into block drive 1, optionally "1:2048" for a partition offset).
 *
 * READ-ONLY: open() with any write intent, plus every mutating VFS entry point
 * (write / mkdir / rmdir / unlink / rename) returns VFS_EPERM. That is a policy,
 * not a stub - the ext4 core has no write path at all.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "ext4.h"

/* ---- mount table (the core is single-volume; one mount point at a time) ---- */
#define EXT4_VFS_MAX_MOUNTS 2
struct ext4_mnt {
    int  used;
    char mp[VFS_MAX_PATH];
    int  drive;
    uint32_t lba;
};
static struct ext4_mnt g_ext4_mnts[EXT4_VFS_MAX_MOUNTS];

static struct ext4_mnt *ext4_mnt_find_by_point(const char *mountpoint) {
    if (!mountpoint) return NULL;
    for (int i = 0; i < EXT4_VFS_MAX_MOUNTS; i++)
        if (g_ext4_mnts[i].used && strcmp(g_ext4_mnts[i].mp, mountpoint) == 0)
            return &g_ext4_mnts[i];
    return NULL;
}

/* Map core error codes to VFS error codes. */
static int map_err(int e) {
    switch (e) {
        case EXT4_OK:        return VFS_OK;
        case EXT4_ENOENT:    return VFS_ENOTFOUND;
        case EXT4_ENOTDIR:   return VFS_ENOTDIR;
        case EXT4_EISDIR:    return VFS_EISDIR;
        case EXT4_EINVAL:    return VFS_EINVAL;
        case EXT4_EIO:       return VFS_EIO;
        case EXT4_EROFS:     return VFS_EPERM;
        case EXT4_ECORRUPT:  return VFS_EIO;
        case EXT4_ENOMEM:    return VFS_ENOSPC;
        case EXT4_ENOSPC:    return VFS_ENOSPC;
        case EXT4_EEXIST:    return VFS_ECONFLICT;
        case EXT4_ENOTEMPTY: return VFS_ECONFLICT;
        case EXT4_ENOTSUP:   return VFS_EPERM;
        case EXT4_EBUSY:     return VFS_EBUSY;
        default:             return VFS_ERR;
    }
}

/* Device strings understood here:
 *   "1", "sdb", "/dev/sdb"   -> block drive 1, LBA 0
 *   "1:2048", "sdb:2048"     -> block drive 1 starting at LBA 2048
 * (identical to the ext2/fat32 adapters, so `mount ext4 <drive> <point>` works
 * with the same device syntax the other drivers accept.) */
static int parse_device(const char *device, int *out_drive, uint32_t *out_lba) {
    const char *p = device;
    int drive = -1;
    uint32_t lba = 0;

    *out_drive = 0;
    *out_lba = 0;
    if (!device) return VFS_EINVAL;
    if (strncmp(p, "/dev/", 5) == 0) p += 5;

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
static int vol_path(struct ext4_mnt *m, const char *path, char *out, uint32_t cap) {
    if (!path || !out || cap == 0) return VFS_EINVAL;
    const char *rel = path;
    size_t mplen = m ? strlen(m->mp) : 0;

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

/* ==================== driver operations ==================== */

static int ext4_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    int drive = 0, r;
    uint32_t lba = 0;
    struct ext4_mnt *m = NULL;

    if (!mountpoint) return VFS_EINVAL;
    r = parse_device(device, &drive, &lba);
    if (r != VFS_OK) return r;

    for (int i = 0; i < EXT4_VFS_MAX_MOUNTS; i++)
        if (!g_ext4_mnts[i].used) { m = &g_ext4_mnts[i]; break; }
    if (!m) return VFS_ENOSPC;

    r = ext4_mount(drive, lba);
    if (r != EXT4_OK) return map_err(r);

    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->drive = drive;
    m->lba = lba;
    strncpy(m->mp, mountpoint, sizeof(m->mp) - 1);
    m->mp[sizeof(m->mp) - 1] = '\0';
    if (fs_data) *fs_data = m;
    console_printf("[ext4] vfs mount drive=%d at %s (read-only, vol=%d)\n",
                   (int64_t)drive, m->mp, (int64_t)r);
    return VFS_OK;
}

static int ext4_vfs_unmount(const char *mountpoint) {
    struct ext4_mnt *m = ext4_mnt_find_by_point(mountpoint);
    if (!m) return VFS_ENOTFOUND;
    (void)ext4_unmount();
    memset(m, 0, sizeof(*m));
    return VFS_OK;
}

static int ext4_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    struct ext4_mnt *m = (struct ext4_mnt *)fs_data;
    char vp[VFS_MAX_PATH];
    struct ext4_file *fh = NULL;
    int acc, wants_write, r;

    if (!m || !m->used) return VFS_EINVAL;
    if (!file_handle) return VFS_EINVAL;
    acc = flags & VFS_O_RDWR;      /* 0 = unspecified, 1 RDONLY, 2 WRONLY, 3 RDWR */
    /* VFS_O_RDWR (0x3) contains VFS_O_RDONLY's bit, so compare the access mode
     * value instead of AND-ing the three flags together. */
    wants_write = (acc == VFS_O_WRONLY || acc == VFS_O_RDWR ||
                   (flags & (VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0);
    if (wants_write) {
        (void)ext4_write_refused("open with write intent");
        return VFS_EPERM;
    }
    r = vol_path(m, path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = ext4_open(vp, &fh);
    if (r != EXT4_OK) return map_err(r);
    *file_handle = fh;
    return VFS_OK;
}

static int ext4_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return map_err(ext4_close((struct ext4_file *)file_handle));
}

static int ext4_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    int n;
    if (!file_handle) return VFS_EINVAL;
    n = ext4_file_read((struct ext4_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = bytes read */
}

static int ext4_vfs_write(void *file_handle, const void *buf, uint32_t size,
                          uint32_t offset) {
    (void)file_handle; (void)buf; (void)size; (void)offset;
    (void)ext4_write_refused("write");
    return VFS_EPERM;
}

static int ext4_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* The VFS core owns the file position and always passes the absolute target
     * offset (SEEK_SET semantics); ext4 reads are random access. */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int ext4_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    struct ext4_mnt *m = (struct ext4_mnt *)fs_data;
    char vp[VFS_MAX_PATH];
    struct ext4_dir *d = NULL;
    int r;

    if (!m || !m->used) return VFS_EINVAL;
    if (!dir_handle) return VFS_EINVAL;
    r = vol_path(m, path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = ext4_opendir(vp, &d);
    if (r != EXT4_OK) return map_err(r);
    *dir_handle = d;
    return VFS_OK;
}

static int ext4_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                            vfs_node_type_t *type) {
    uint32_t len = 0;
    int is_dir = 0;
    int r;

    if (!dir_handle || !name) return VFS_EINVAL;
    r = ext4_readdir((struct ext4_dir *)dir_handle, name, VFS_MAX_NAME, &len, &is_dir);
    if (r == EXT4_ENOENT) return VFS_ENOTFOUND;      /* end of directory */
    if (r != EXT4_OK) return map_err(r);
    if (name_len) *name_len = len;
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int ext4_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return map_err(ext4_closedir((struct ext4_dir *)dir_handle));
}

static int ext4_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    struct ext4_mnt *m = (struct ext4_mnt *)fs_data;
    char vp[VFS_MAX_PATH];
    struct ext4_stat es;
    int r;

    if (!m || !m->used) return VFS_EINVAL;
    if (!st) return VFS_EINVAL;
    r = vol_path(m, path, vp, sizeof(vp));
    if (r != VFS_OK) return r;
    r = ext4_stat_path(vp, &es);
    if (r != EXT4_OK) return map_err(r);

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

/* ---- mutations: refused, never silently accepted ---- */

static int ext4_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)path; (void)mode;
    (void)ext4_write_refused("mkdir");
    return VFS_EPERM;
}

static int ext4_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    (void)ext4_write_refused("rmdir");
    return VFS_EPERM;
}

static int ext4_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    (void)ext4_write_refused("unlink");
    return VFS_EPERM;
}

static int ext4_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    (void)ext4_write_refused("rename");
    return VFS_EPERM;
}

/* A read-only filesystem has nothing to flush: report success so `sync` does not
 * print a spurious failure for a volume that cannot be dirty. */
static int ext4_vfs_sync(void *fs_data) {
    struct ext4_mnt *m = (struct ext4_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    return VFS_OK;
}

static int ext4_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                   uint64_t *total_bytes) {
    struct ext4_mnt *m = (struct ext4_mnt *)fs_data;
    uint64_t block_size;
    if (!m || !m->used) return VFS_EINVAL;
    if (!ext4_is_mounted()) return VFS_EBUSY;
    block_size = (uint64_t)ext4_block_size();
    /* Read-only: the volume cannot become dirty, so the superblock counters are
     * the current truth (there is no write path that could desynchronise them). */
    if (free_bytes) *free_bytes = (uint64_t)ext4_free_blocks_count() * block_size;
    if (total_bytes) *total_bytes = (uint64_t)ext4_blocks_count() * block_size;
    return VFS_OK;
}

/* ==================== driver export ==================== */

const vfs_fs_driver_t ext4_driver = {
    .name = "ext4",
    .mount = ext4_vfs_mount,
    .unmount = ext4_vfs_unmount,
    .open = ext4_vfs_open,
    .close = ext4_vfs_close,
    .read = ext4_vfs_read,
    .write = ext4_vfs_write,
    .seek = ext4_vfs_seek,
    .opendir = ext4_vfs_opendir,
    .readdir = ext4_vfs_readdir,
    .closedir = ext4_vfs_closedir,
    .stat = ext4_vfs_stat,
    .mkdir = ext4_vfs_mkdir,
    .rmdir = ext4_vfs_rmdir,
    .unlink = ext4_vfs_unlink,
    .rename = ext4_vfs_rename,
    .sync = ext4_vfs_sync,
    .get_free_space = ext4_vfs_get_free_space,
};
