/* SutureOS: VimtuFS2 (read-only) driver for the unified VFS
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the real core in
 * kernel/fs/vimtufs2.c - the same shape as kernel/fs/ext2_vfs.c.  The driver is
 * registered under the name "vimtufs2", so the shell reaches it with
 *   mount vimtufs2 1 /mnt/v2
 * (device = block drive number, optionally "<drive>:<start-lba>").
 *
 * READ-ONLY BY DESIGN.  VimtuFS2 is VimtuOS' filesystem (format spec: VimtuOS
 * kernel/vfs64.h + vfs64.cpp, GPL-3.0; this port is a re-implementation, see
 * kernel/fs/vimtufs2.c).  Writing it needs the journal / quota / ACL / snapshot
 * machinery this port does not implement, so open-for-write, mkdir, rmdir,
 * unlink and rename all return VFS_EPERM and say so in the log instead of
 * silently succeeding.  sync() is a no-op that reports success: there is
 * nothing to flush.
 *
 * Known limitation (documented, not hidden): VimtuFS2 stores a packed 32-bit
 * timestamp and vfs_stat_t wants epoch seconds, so stat reports mtime = 0
 * rather than inventing a calendar conversion.  vimtufs2_stat() still exposes
 * the raw packed value to anything that asks the driver directly.
 */
#include "vfs.h"

#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "vimtufs2.h"

/* One entry per mounted volume: the driver keeps up to VIMTUFS2_MAX_MOUNTS
 * volumes, and mountpoint <-> slot has to be remembered per mount because the
 * same volume may later be reachable through another mountpoint. */
#define VF_VFS_MAX_MOUNTS 4
struct vf_mnt {
    int      used;
    char     mp[VFS_MAX_PATH];
    int      drive;
    uint32_t lba;
    int      vol;
};
static struct vf_mnt g_vf_mnts[VF_VFS_MAX_MOUNTS];

/* Write refusals are logged a few times, then the log is left alone. */
static int g_write_warned;

static struct vf_mnt *vf_mnt_find_point(const char *mountpoint) {
    if (!mountpoint) return NULL;
    for (int i = 0; i < VF_VFS_MAX_MOUNTS; i++) {
        if (g_vf_mnts[i].used && strcmp(g_vf_mnts[i].mp, mountpoint) == 0) {
            return &g_vf_mnts[i];
        }
    }
    return NULL;
}

static void vf_refuse_write(const char *op) {
    if (g_write_warned < 4) {
        console_printf("[vimtufs2] %s refused: vimtufs2 is a read-only driver\n", op);
        g_write_warned++;
        if (g_write_warned == 4) {
            console_puts("[vimtufs2] further write refusals are not logged\n");
        }
    }
}

/* driver error codes -> VFS error codes */
static int vf_map_err(int e) {
    switch (e) {
        case VIMTUFS2_OK:           return VFS_OK;
        case VIMTUFS2_ENOENT:       return VFS_ENOTFOUND;
        case VIMTUFS2_ENOTDIR:      return VFS_ENOTDIR;
        case VIMTUFS2_EISDIR:       return VFS_EISDIR;
        case VIMTUFS2_EINVAL:       return VFS_EINVAL;
        case VIMTUFS2_ECORRUPT:     return VFS_EIO;
        case VIMTUFS2_EIO:          return VFS_EIO;
        case VIMTUFS2_ENAMETOOLONG: return VFS_EINVAL;
        case VIMTUFS2_ENOTSUP:      return VFS_EPERM;
        case VIMTUFS2_EBUSY:        return VFS_EBUSY;
        case VIMTUFS2_ENOSPC:       return VFS_ENOSPC;
        default:                    return VFS_ERR;
    }
}

/* Device strings understood here:
 *   "1"        -> block drive 1, LBA 0        (what the acceptance script uses)
 *   "1:2048"   -> block drive 1, LBA 2048
 *   "sdb"      -> block drive 1 (sdX letters, /dev/ prefix tolerated)
 * RAM disks are not supported: this driver never takes a RAM image.
 */
static int vf_parse_device(const char *device, int *out_drive, uint32_t *out_lba) {
    const char *p = device;
    int drive = -1;
    uint32_t lba = 0;

    *out_drive = 0;
    *out_lba = 0;
    if (!device || !device[0]) return VFS_EINVAL;
    if (strncmp(p, "/dev/", 5) == 0) p += 5;

    if ((p[0] == 'r' || p[0] == 'R') && (p[1] == 'a' || p[1] == 'A') &&
        (p[2] == 'm' || p[2] == 'M')) {
        console_puts("[vimtufs2] mount: RAM disks are not supported by this "
                     "read-only driver\n");
        return VFS_EINVAL;
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
    } else {
        return VFS_EINVAL;
    }
    if (*p == ':' || *p == ',' || *p == '+') {
        p++;
        while (*p >= '0' && *p <= '9') {
            lba = lba * 10u + (uint32_t)(*p - '0');
            p++;
        }
    }
    if (drive < 0) return VFS_EINVAL;
    *out_drive = drive;
    *out_lba = lba;
    return VFS_OK;
}

/* VFS path -> volume-relative path ("/HELLO.TXT"), stripping the mountpoint. */
static int vf_vol_path(struct vf_mnt *m, const char *path, char *out, uint32_t cap) {
    const char *rel = path;
    const size_t mplen = m ? strlen(m->mp) : 0;

    if (!path || !out || cap == 0) return VFS_EINVAL;
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

static int vf_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    int drive = 0;
    uint32_t lba = 0;
    struct vf_mnt *m = NULL;

    if (!mountpoint) return VFS_EINVAL;
    int r = vf_parse_device(device, &drive, &lba);
    if (r != VFS_OK) return r;

    for (int i = 0; i < VF_VFS_MAX_MOUNTS; i++) {
        if (g_vf_mnts[i].used) {
            if (strcmp(g_vf_mnts[i].mp, mountpoint) == 0) return VFS_EBUSY;
            continue;
        }
        if (!m) m = &g_vf_mnts[i];
    }
    if (!m) return VFS_ENOSPC;

    const int slot = vimtufs2_mount(drive, lba);
    if (slot < 0) return vf_map_err(slot);

    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->drive = drive;
    m->lba = lba;
    m->vol = slot;
    strncpy(m->mp, mountpoint, sizeof(m->mp) - 1);
    m->mp[sizeof(m->mp) - 1] = '\0';
    if (fs_data) *fs_data = m;

    console_printf("[vimtufs2] vfs mount drive=%d at %s (volume slot %d, "
                   "read-only)\n", (int64_t)drive, m->mp, (int64_t)slot);
    return VFS_OK;
}

static int vf_vfs_unmount(const char *mountpoint) {
    struct vf_mnt *m = vf_mnt_find_point(mountpoint);
    if (!m) return VFS_ENOTFOUND;
    int r = vimtufs2_unmount(m->drive, m->lba);
    memset(m, 0, sizeof(*m));
    return (r == VIMTUFS2_OK) ? VFS_OK : vf_map_err(r);
}

static int vf_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    struct vf_mnt *m = (struct vf_mnt *)fs_data;
    if (!m || !m->used || !file_handle) return VFS_EINVAL;
    (void)vimtufs2_select(m->vol);

    {
        const int acc = flags & VFS_O_RDWR;
        const int wants_write = (acc == VFS_O_WRONLY || acc == VFS_O_RDWR ||
                                 (flags & (VFS_O_CREAT | VFS_O_TRUNC |
                                           VFS_O_APPEND)) != 0);
        if (wants_write) {
            vf_refuse_write("open for write");
            return VFS_EPERM;
        }
    }
    {
        char vp[VFS_MAX_PATH];
        int r = vf_vol_path(m, path, vp, sizeof(vp));
        if (r != VFS_OK) return r;
        struct vimtufs2_file *fh = NULL;
        r = vimtufs2_open(vp, &fh);
        if (r != VIMTUFS2_OK) return vf_map_err(r);
        *file_handle = fh;
    }
    return VFS_OK;
}

static int vf_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return vf_map_err(vimtufs2_close((struct vimtufs2_file *)file_handle));
}

static int vf_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    if (!file_handle) return VFS_EINVAL;
    const int n = vimtufs2_read((struct vimtufs2_file *)file_handle, buf, size, offset);
    return (n < 0) ? vf_map_err(n) : n;         /* >= 0 = bytes read */
}

static int vf_vfs_write(void *file_handle, const void *buf, uint32_t size,
                        uint32_t offset) {
    (void)file_handle; (void)buf; (void)size; (void)offset;
    vf_refuse_write("write");
    return VFS_EPERM;
}

static int vf_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* The VFS core owns the file position and passes absolute offsets (SEEK_SET
     * semantics); vimtufs2 reads are random access, so nothing to do here. */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int vf_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    struct vf_mnt *m = (struct vf_mnt *)fs_data;
    if (!m || !m->used || !dir_handle) return VFS_EINVAL;
    (void)vimtufs2_select(m->vol);
    {
        char vp[VFS_MAX_PATH];
        int r = vf_vol_path(m, path, vp, sizeof(vp));
        if (r != VFS_OK) return r;
        struct vimtufs2_dir *dh = NULL;
        r = vimtufs2_opendir(vp, &dh);
        if (r != VIMTUFS2_OK) return vf_map_err(r);
        *dir_handle = dh;
    }
    return VFS_OK;
}

static int vf_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                          vfs_node_type_t *type) {
    uint32_t len = 0;
    int is_dir = 0;
    if (!dir_handle || !name) return VFS_EINVAL;
    const int r = vimtufs2_readdir((struct vimtufs2_dir *)dir_handle, name,
                                   VFS_MAX_NAME, &len, &is_dir);
    if (r == VIMTUFS2_ENOENT) return VFS_ENOTFOUND;      /* end of directory */
    if (r != VIMTUFS2_OK) return vf_map_err(r);
    if (name_len) *name_len = len;
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int vf_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return vf_map_err(vimtufs2_closedir((struct vimtufs2_dir *)dir_handle));
}

static int vf_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    struct vf_mnt *m = (struct vf_mnt *)fs_data;
    if (!m || !m->used || !st) return VFS_EINVAL;
    (void)vimtufs2_select(m->vol);
    {
        char vp[VFS_MAX_PATH];
        struct vimtufs2_stat vs;
        int r = vf_vol_path(m, path, vp, sizeof(vp));
        if (r != VFS_OK) return r;
        r = vimtufs2_stat(vp, &vs);
        if (r != VIMTUFS2_OK) return vf_map_err(r);

        memset(st, 0, sizeof(*st));
        st->type = (vs.type == VIMTUFS2_T_DIR) ? VFS_NODE_DIR
                 : (vs.type == VIMTUFS2_T_LINK) ? VFS_NODE_SYMLINK : VFS_NODE_FILE;
        st->size = vs.size;
        st->mode = vs.mode & 0xFFFu;          /* permission bits only */
        st->uid = vs.uid;
        st->gid = vs.gid;
        /* See the file header: the packed VimtuFS2 time is not epoch seconds,
         * and this driver does not fake a conversion. */
        st->mtime = 0;
        st->atime = 0;
        st->ctime = 0;
    }
    return VFS_OK;
}

static int vf_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)path; (void)mode;
    vf_refuse_write("mkdir");
    return VFS_EPERM;
}

static int vf_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    vf_refuse_write("rmdir");
    return VFS_EPERM;
}

static int vf_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    vf_refuse_write("unlink");
    return VFS_EPERM;
}

static int vf_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    vf_refuse_write("rename");
    return VFS_EPERM;
}

static int vf_vfs_sync(void *fs_data) {
    /* Read-only: nothing this driver ever wrote can be dirty.  Returning OK (not
     * an error) keeps `sync`/`umount` quiet for a read-only volume. */
    (void)fs_data;
    return VFS_OK;
}

static int vf_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                 uint64_t *total_bytes) {
    struct vf_mnt *m = (struct vf_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    {
        struct vimtufs2_vol_info info;
        if (vimtufs2_volume_info(m->vol, &info) != VIMTUFS2_OK) return VFS_EBUSY;
        /* Free space is measured from the on-disk bitmap at mount time, so `df`
         * reports the volume's real free-block count (the driver just cannot
         * consume it). */
        if (free_bytes) *free_bytes = (uint64_t)info.free_blocks * VIMTUFS2_SECTOR_BYTES;
        if (total_bytes) *total_bytes = (uint64_t)info.blocks * VIMTUFS2_SECTOR_BYTES;
    }
    return VFS_OK;
}

/* ==================== driver export ==================== */

const vfs_fs_driver_t vimtufs2_driver = {
    .name = "vimtufs2",
    .mount = vf_vfs_mount,
    .unmount = vf_vfs_unmount,
    .open = vf_vfs_open,
    .close = vf_vfs_close,
    .read = vf_vfs_read,
    .write = vf_vfs_write,
    .seek = vf_vfs_seek,
    .opendir = vf_vfs_opendir,
    .readdir = vf_vfs_readdir,
    .closedir = vf_vfs_closedir,
    .stat = vf_vfs_stat,
    .mkdir = vf_vfs_mkdir,
    .rmdir = vf_vfs_rmdir,
    .unlink = vf_vfs_unlink,
    .rename = vf_vfs_rename,
    .sync = vf_vfs_sync,
    .get_free_space = vf_vfs_get_free_space,
};
