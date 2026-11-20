/* SutureOS: OS67 inode-filesystem (Minix v1 layout) VFS adapter.
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the ported core
 * in kernel/fs/os67fs.c, the same shape as ext2_vfs.c / vimtufs2_vfs.c.
 * Driver name is "os67fs", so `mount os67fs 1 /mnt/o67` finds it.
 *
 * Read-only: open with write intent, write/mkdir/rmdir/unlink/rename all
 * report VFS_EPERM before touching the volume.
 */
#include "os67fs.h"
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

#define OS67FS_VFS_MAX_MOUNTS 2
#define OS67FS_VFS_MAX_FILES  8
#define OS67FS_VFS_MAX_DIRS   8

struct os67fs_mnt {
    int  used;
    char mp[VFS_MAX_PATH];
    int  drive;
    int  vol;
};

struct os67fs_file {
    int      used;
    int      vol;
    uint32_t ino;
    uint32_t size;
};

struct os67fs_dir {
    int      used;
    int      vol;
    uint32_t ino;
    int      cursor;
};

static struct os67fs_mnt  g_os67fs_mnts[OS67FS_VFS_MAX_MOUNTS];
static struct os67fs_file g_os67fs_files[OS67FS_VFS_MAX_FILES];
static struct os67fs_dir  g_os67fs_dirs[OS67FS_VFS_MAX_DIRS];

static struct os67fs_mnt *os67fs_mnt_by_point(const char *mp)
{
    if (!mp) return NULL;
    for (int i = 0; i < OS67FS_VFS_MAX_MOUNTS; i++)
        if (g_os67fs_mnts[i].used && strcmp(g_os67fs_mnts[i].mp, mp) == 0)
            return &g_os67fs_mnts[i];
    return NULL;
}

/* Strip the mount point from an absolute VFS path. */
static const char *os67fs_rel_path(const struct os67fs_mnt *m, const char *path)
{
    if (!path) return "";
    const size_t n = strlen(m->mp);
    if (n > 1 && strncmp(path, m->mp, n) == 0) path += n;
    while (*path == '/') path++;
    return path;
}

static int os67fs_vfs_mount(const char *device, const char *mountpoint, void **fs_data)
{
    if (!mountpoint) return VFS_EINVAL;
    int drive = 1;
    if (device && *device) {
        int v = 0;
        const char *p = device;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        drive = v;
    }
    struct os67fs_mnt *slot = NULL;
    for (int i = 0; i < OS67FS_VFS_MAX_MOUNTS; i++)
        if (!g_os67fs_mnts[i].used) { slot = &g_os67fs_mnts[i]; break; }
    if (!slot) return VFS_ENOSPC;

    const int rc = os67fs_mount_drive(drive);
    if (rc != VFS_OK) return rc;

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->drive = drive;
    slot->vol = 0;                       /* mount_drive selects the volume    */
    strncpy(slot->mp, mountpoint, sizeof(slot->mp) - 1);
    slot->mp[sizeof(slot->mp) - 1] = '\0';
    if (fs_data) *fs_data = slot;
    console_printf("[os67fs] vfs mount drive=%d at %s (read-only)\n", drive, slot->mp);
    return VFS_OK;
}

static int os67fs_vfs_unmount(const char *mountpoint)
{
    struct os67fs_mnt *m = os67fs_mnt_by_point(mountpoint);
    if (!m) return VFS_EINVAL;
    memset(m, 0, sizeof(*m));
    return VFS_OK;
}

static int os67fs_vfs_open(void *fs_data, const char *path, int flags, void **file_handle)
{
    struct os67fs_mnt *m = (struct os67fs_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    const int mode = flags & 0x3;
    if (mode == VFS_O_WRONLY || mode == VFS_O_RDWR) {
        console_printf("[os67fs] write rejected: this driver is read-only\n");
        return VFS_EPERM;
    }
    const char *rel = os67fs_rel_path(m, path);
    uint32_t ino = 0;
    int rc = os67fs_resolve(m->vol, rel, &ino);
    if (rc != VFS_OK) return rc;

    struct vfs_stat st;
    rc = os67fs_inode_stat(m->vol, ino, &st);
    if (rc != VFS_OK) return rc;
    if (st.type == VFS_NODE_DIR) return VFS_EISDIR;

    for (int i = 0; i < OS67FS_VFS_MAX_FILES; i++) {
        if (g_os67fs_files[i].used) continue;
        memset(&g_os67fs_files[i], 0, sizeof(g_os67fs_files[i]));
        g_os67fs_files[i].used = 1;
        g_os67fs_files[i].vol = m->vol;
        g_os67fs_files[i].ino = ino;
        g_os67fs_files[i].size = (uint32_t)st.size;
        if (file_handle) *file_handle = &g_os67fs_files[i];
        return VFS_OK;
    }
    return VFS_ENOSPC;
}

static int os67fs_vfs_close(void *file_handle)
{
    if (file_handle) memset(file_handle, 0, sizeof(struct os67fs_file));
    return VFS_OK;
}

static int os67fs_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset)
{
    struct os67fs_file *f = (struct os67fs_file *)file_handle;
    if (!f || !f->used || !buf) return VFS_EINVAL;
    return os67fs_read(f->vol, f->ino, buf, size, offset);
}

static int os67fs_vfs_write(void *h, const void *b, uint32_t s, uint32_t o)
{
    (void)h; (void)b; (void)s; (void)o;
    return VFS_EPERM;
}

static int os67fs_vfs_seek(void *h, uint32_t offset, int whence)
{
    (void)h; (void)offset; (void)whence;
    return VFS_OK;
}

static int os67fs_vfs_opendir(void *fs_data, const char *path, void **dir_handle)
{
    struct os67fs_mnt *m = (struct os67fs_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    const char *rel = os67fs_rel_path(m, path);
    uint32_t ino = 0;
    int rc = os67fs_resolve(m->vol, rel, &ino);
    if (rc != VFS_OK) return rc;

    struct vfs_stat st;
    rc = os67fs_inode_stat(m->vol, ino, &st);
    if (rc != VFS_OK) return rc;
    if (st.type != VFS_NODE_DIR) return VFS_ENOTDIR;

    for (int i = 0; i < OS67FS_VFS_MAX_DIRS; i++) {
        if (g_os67fs_dirs[i].used) continue;
        memset(&g_os67fs_dirs[i], 0, sizeof(g_os67fs_dirs[i]));
        g_os67fs_dirs[i].used = 1;
        g_os67fs_dirs[i].vol = m->vol;
        g_os67fs_dirs[i].ino = ino;
        if (dir_handle) *dir_handle = &g_os67fs_dirs[i];
        return VFS_OK;
    }
    return VFS_ENOSPC;
}

static int os67fs_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                              vfs_node_type_t *type)
{
    struct os67fs_dir *d = (struct os67fs_dir *)dir_handle;
    if (!d || !d->used || !name || !name_len) return VFS_EINVAL;
    for (;;) {
        const int n = os67fs_dir_entry(d->vol, d->ino, d->cursor, name,
                                       (uint32_t)VFS_MAX_NAME, NULL, type);
        d->cursor++;
        if (n <= 0) return VFS_ENOTFOUND;
        *name_len = (uint32_t)strlen(name);
        return VFS_OK;
    }
}

static int os67fs_vfs_closedir(void *dir_handle)
{
    if (dir_handle) memset(dir_handle, 0, sizeof(struct os67fs_dir));
    return VFS_OK;
}

static int os67fs_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st)
{
    struct os67fs_mnt *m = (struct os67fs_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    const char *rel = os67fs_rel_path(m, path);
    uint32_t ino = 0;
    int rc = os67fs_resolve(m->vol, rel, &ino);
    if (rc != VFS_OK) return rc;
    return os67fs_inode_stat(m->vol, ino, st);
}

static int os67fs_ro_op(void *a, const char *b, uint32_t c) { (void)a; (void)b; (void)c; return VFS_EPERM; }
static int os67fs_ro_op2(void *a, const char *b, const char *c) { (void)a; (void)b; (void)c; return VFS_EPERM; }
static int os67fs_ro_unlink(void *a, const char *b) { (void)a; (void)b; return VFS_EPERM; }

static int os67fs_vfs_sync(void *fs_data) { (void)fs_data; return VFS_OK; }

static int os67fs_vfs_free_space(void *fs_data, uint64_t *free_bytes, uint64_t *total_bytes)
{
    struct os67fs_mnt *m = (struct os67fs_mnt *)fs_data;
    if (!m || !m->used) return VFS_EINVAL;
    if (free_bytes) *free_bytes = 0;                 /* read-only: no allocator */
    if (total_bytes) *total_bytes = 0;
    return VFS_EPERM;
}

const vfs_fs_driver_t os67fs_driver = {
    .name = "os67fs",
    .mount = os67fs_vfs_mount,
    .unmount = os67fs_vfs_unmount,
    .open = os67fs_vfs_open,
    .close = os67fs_vfs_close,
    .read = os67fs_vfs_read,
    .write = os67fs_vfs_write,
    .seek = os67fs_vfs_seek,
    .opendir = os67fs_vfs_opendir,
    .readdir = os67fs_vfs_readdir,
    .closedir = os67fs_vfs_closedir,
    .stat = os67fs_vfs_stat,
    .mkdir = os67fs_ro_op,
    .rmdir = os67fs_ro_unlink,
    .unlink = os67fs_ro_unlink,
    .rename = os67fs_ro_op2,
    .sync = os67fs_vfs_sync,
    .get_free_space = os67fs_vfs_free_space,
};
