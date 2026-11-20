/* SutureOS sysfs: a synthetic, read-only /sys tree over kernel objects.
 *
 * Complements /proc (flat generated files) with a hierarchical view, which is
 * what Linux-style tools expect for device discovery:
 *   /sys/kernel/version      kernel version string
 *   /sys/kernel/uptime       seconds.centiseconds
 *   /sys/block               one line per registered block device (as a dir)
 *   /sys/block/<name>/size   device size in 512-byte sectors
 *   /sys/block/<name>/dev    drive number
 *   /sys/block/<name>/ro     1 when the device is read-only
 *   /sys/block/<name>/type   SutureOS block type number
 *
 * Mechanism sources (read-only reference, no code copied): LunaixOS sysfs-style
 * tree layout, the Linux /sys/block/<dev>/{size,dev,ro} conventions.
 * Implemented from scratch for SutureOS (GPL-3.0).
 */
#include "vfs.h"
#include "block.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <sutureos/version.h>

#define SYSFS_HANDLES 8
#define SYSFS_BUF 512
#define SYSFS_NAME 48

static char g_sysfs_mp[VFS_MAX_PATH] = "/sys";

struct sysfs_handle {
    int used;
    char path[VFS_MAX_PATH];   /* normalised path relative to the mountpoint */
    char text[SYSFS_BUF];
    int len;
};

static struct sysfs_handle g_handles[SYSFS_HANDLES];

extern volatile uint64_t g_tick_count;

/* ---- helpers ------------------------------------------------------------- */

static int sysfs_rel(const char *path, const char **out)
{
    if (!path) return -1;
    const size_t mplen = strlen(g_sysfs_mp);
    if (mplen > 1 && strncmp(path, g_sysfs_mp, mplen) == 0) path += mplen;
    while (*path == '/') path++;
    *out = path;
    return 0;
}

/* Count path components of a relative path ("block/ata0/size" -> 3). */
static int sysfs_depth(const char *rel)
{
    int n = 1;
    if (!rel || !*rel) return 0;
    while (*rel) { if (*rel == '/') n++; rel++; }
    return n;
}

/* Copy component #idx (0-based) of `rel` into out (max len). */
static int sysfs_component(const char *rel, int idx, char *out, size_t cap)
{
    int cur = 0;
    while (*rel) {
        while (*rel == '/') rel++;
        if (!*rel) break;
        const char *start = rel;
        while (*rel && *rel != '/') rel++;
        const size_t len = (size_t)(rel - start);
        if (cur == idx) {
            if (len + 1 > cap) return -1;
            memcpy(out, start, len);
            out[len] = '\0';
            return 0;
        }
        cur++;
    }
    return -1;
}

static int sysfs_blk_exists(const char *name)
{
    return blk_find_by_name(name) != NULL;
}

/* last component of a relative path ("" for the root) */
static const char *sysfs_base(const char *rel)
{
    const char *b = rel;
    while (*rel) { if (*rel == '/') b = rel + 1; rel++; }
    return b;
}

/* Build the text of a generated file. Returns length (0 when not a file). */
static int sysfs_build(const char *rel, char *buf, uint32_t cap)
{
    if (!rel || !*rel) return 0;
    char c0[SYSFS_NAME], c1[SYSFS_NAME], c2[SYSFS_NAME];
    c0[0] = c1[0] = c2[0] = '\0';
    if (sysfs_component(rel, 0, c0, sizeof(c0)) != 0) return 0;
    const int depth = sysfs_depth(rel);

    if (strcmp(c0, "kernel") == 0 && depth == 2) {
        if (sysfs_component(rel, 1, c1, sizeof(c1)) != 0) return 0;
        if (strcmp(c1, "version") == 0)
            return snprintf(buf, cap, "%s\n", SUTUREOS_VERSION);
        if (strcmp(c1, "uptime") == 0) {
            const unsigned s = (unsigned)(g_tick_count / 100ull);
            const unsigned cs = (unsigned)(g_tick_count % 100ull);
            return snprintf(buf, cap, "%u.%u\n", s, cs);
        }
        return 0;
    }

    if (strcmp(c0, "block") == 0 && depth == 3) {
        if (sysfs_component(rel, 1, c1, sizeof(c1)) != 0) return 0;
        if (sysfs_component(rel, 2, c2, sizeof(c2)) != 0) return 0;
        if (strcmp(c1, "block") == 0) return 0;
        const struct blk_device *dev = blk_find_by_name(c1);
        if (!dev) return 0;
        if (strcmp(c2, "size") == 0)
            return snprintf(buf, cap, "%llu\n", (unsigned long long)dev->sector_count);
        if (strcmp(c2, "dev") == 0)
            return snprintf(buf, cap, "%d\n", dev->drive);
        if (strcmp(c2, "ro") == 0)
            return snprintf(buf, cap, "%d\n", (dev->flags & BLK_FLAG_READONLY) ? 1 : 0);
        if (strcmp(c2, "type") == 0)
            return snprintf(buf, cap, "%d\n", dev->type);
        return 0;
    }
    return 0;
}

/* A path is a directory when it is the root, /kernel, /block, or /block/<dev>. */
static int sysfs_is_dir(const char *rel)
{
    if (!rel || !*rel) return 1;
    if (strcmp(rel, "kernel") == 0 || strcmp(rel, "block") == 0) return 1;
    if (strncmp(rel, "block/", 6) == 0 && sysfs_depth(rel) == 2) {
        char name[SYSFS_NAME];
        if (sysfs_component(rel, 1, name, sizeof(name)) != 0) return 0;
        return sysfs_blk_exists(name);
    }
    return 0;
}

/* Directory enumeration: returns the child name #idx (0-based), 0 = end. */
static int sysfs_child(const char *rel, int idx, char *out, size_t cap,
                       vfs_node_type_t *type)
{
    if (!rel || !*rel) {   /* "/" */
        if (idx == 0) { strcpy(out, "kernel"); if (type) *type = VFS_NODE_DIR; return 1; }
        if (idx == 1) { strcpy(out, "block"); if (type) *type = VFS_NODE_DIR; return 1; }
        return 0;
    }
    if (strcmp(rel, "kernel") == 0) {
        if (idx == 0) { strcpy(out, "version"); if (type) *type = VFS_NODE_FILE; return 1; }
        if (idx == 1) { strcpy(out, "uptime");  if (type) *type = VFS_NODE_FILE; return 1; }
        return 0;
    }
    if (strcmp(rel, "block") == 0) {
        int n = 0;
        for (const struct blk_device *d = g_blk_devices; d; d = d->next) {
            if (n == idx) {
                if (!d->name) return 0;
                strncpy(out, d->name, cap - 1);
                out[cap - 1] = '\0';
                if (type) *type = VFS_NODE_DIR;
                return 1;
            }
            n++;
        }
        return 0;
    }
    if (strncmp(rel, "block/", 6) == 0 && sysfs_is_dir(rel)) {
        static const char *const files[] = { "size", "dev", "ro", "type" };
        if (idx < 0 || idx >= 4) return 0;
        strncpy(out, files[idx], cap - 1);
        out[cap - 1] = '\0';
        if (type) *type = VFS_NODE_FILE;
        return 1;
    }
    return 0;
}

/* ---- VFS operations ------------------------------------------------------ */

static int sysfs_mount(const char *device, const char *mountpoint, void **fs_data)
{
    (void)device;
    if (!mountpoint) return VFS_EINVAL;
    strncpy(g_sysfs_mp, mountpoint, sizeof(g_sysfs_mp) - 1);
    g_sysfs_mp[sizeof(g_sysfs_mp) - 1] = '\0';
    memset(g_handles, 0, sizeof(g_handles));
    if (fs_data) *fs_data = NULL;
    console_printf("[sysfs] mounted at %s (kernel/ + block/)\n", g_sysfs_mp);
    return VFS_OK;
}

static int sysfs_unmount(const char *mountpoint)
{
    (void)mountpoint;
    memset(g_handles, 0, sizeof(g_handles));
    return VFS_OK;
}

static int sysfs_open(void *fs_data, const char *path, int flags, void **file_handle)
{
    (void)fs_data;
    const int mode = flags & 0x3;                 /* RDONLY=1 / WRONLY=2 / RDWR=3 */
    if (mode == VFS_O_WRONLY || mode == VFS_O_RDWR) return VFS_EPERM;
    const char *rel = NULL;
    if (sysfs_rel(path, &rel) != 0) return VFS_EINVAL;
    if (sysfs_is_dir(rel)) return VFS_EISDIR;

    for (int i = 0; i < SYSFS_HANDLES; i++) {
        if (g_handles[i].used) continue;
        struct sysfs_handle *h = &g_handles[i];
        memset(h, 0, sizeof(*h));
        strncpy(h->path, rel, sizeof(h->path) - 1);
        h->len = sysfs_build(rel, h->text, sizeof(h->text));
        if (h->len <= 0) return VFS_ENOTFOUND;
        h->used = 1;
        if (file_handle) *file_handle = h;
        return VFS_OK;
    }
    return VFS_ENOSPC;
}

static int sysfs_close(void *file_handle)
{
    struct sysfs_handle *h = (struct sysfs_handle *)file_handle;
    if (h) memset(h, 0, sizeof(*h));
    return VFS_OK;
}

static int sysfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset)
{
    struct sysfs_handle *h = (struct sysfs_handle *)file_handle;
    if (!h || !h->used || !buf) return VFS_EINVAL;
    if (offset >= (uint32_t)h->len) return 0;
    uint32_t n = (uint32_t)h->len - offset;
    if (n > size) n = size;
    memcpy(buf, h->text + offset, n);
    return (int)n;
}

static int sysfs_write(void *h, const void *b, uint32_t s, uint32_t o)
{
    (void)h; (void)b; (void)s; (void)o;
    return VFS_EPERM;
}

static int sysfs_seek(void *h, uint32_t o, int w) { (void)h; (void)o; (void)w; return VFS_OK; }

struct sysfs_dir {
    int used;
    char path[VFS_MAX_PATH];
    int cursor;
};
static struct sysfs_dir g_dirs[4];

static int sysfs_opendir(void *fs_data, const char *path, void **dir_handle)
{
    (void)fs_data;
    const char *rel = NULL;
    if (sysfs_rel(path, &rel) != 0) return VFS_EINVAL;
    if (!sysfs_is_dir(rel)) return VFS_ENOTDIR;
    for (int i = 0; i < 4; i++) {
        if (g_dirs[i].used) continue;
        memset(&g_dirs[i], 0, sizeof(g_dirs[i]));
        strncpy(g_dirs[i].path, rel, sizeof(g_dirs[i].path) - 1);
        g_dirs[i].used = 1;
        if (dir_handle) *dir_handle = &g_dirs[i];
        return VFS_OK;
    }
    return VFS_ENOSPC;
}

static int sysfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                         vfs_node_type_t *type)
{
    struct sysfs_dir *d = (struct sysfs_dir *)dir_handle;
    if (!d || !d->used || !name || !name_len) return VFS_EINVAL;
    char child[SYSFS_NAME];
    if (!sysfs_child(d->path, d->cursor, child, sizeof(child), type))
        return VFS_ENOTFOUND;
    d->cursor++;
    const size_t len = strlen(child);
    if (len + 1 > VFS_MAX_NAME) return VFS_EIO;
    memcpy(name, child, len + 1);
    *name_len = (uint32_t)len;
    return VFS_OK;
}

static int sysfs_closedir(void *dir_handle)
{
    struct sysfs_dir *d = (struct sysfs_dir *)dir_handle;
    if (d) memset(d, 0, sizeof(*d));
    return VFS_OK;
}

static int sysfs_stat(void *fs_data, const char *path, struct vfs_stat *st)
{
    (void)fs_data;
    if (!st) return VFS_EINVAL;
    memset(st, 0, sizeof(*st));
    const char *rel = NULL;
    if (sysfs_rel(path, &rel) != 0) return VFS_EINVAL;
    if (!rel) rel = "";
    if (sysfs_is_dir(rel)) {
        st->type = VFS_NODE_DIR;
        st->mode = 0555;
        return VFS_OK;
    }
    /* unknown paths and known files that generate no text */
    int n = sysfs_build(rel, (char *)st, 0);   /* probe only: cap=0 -> 0 */
    (void)n;
    st->type = VFS_NODE_FILE;
    st->mode = 0444;
    st->size = 0;
    {
        char tmp[SYSFS_BUF];
        const int len = sysfs_build(rel, tmp, sizeof(tmp));
        if (len <= 0) return VFS_ENOTFOUND;
        st->size = (uint64_t)len;
    }
    return VFS_OK;
}

static int sysfs_sync(void *fs_data) { (void)fs_data; return VFS_OK; }

static int sysfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                uint64_t *total_bytes)
{
    (void)fs_data;
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    return VFS_EPERM;
}

const vfs_fs_driver_t sysfs_driver = {
    .name = "sysfs",
    .mount = sysfs_mount,
    .unmount = sysfs_unmount,
    .open = sysfs_open,
    .close = sysfs_close,
    .read = sysfs_read,
    .write = sysfs_write,
    .seek = sysfs_seek,
    .opendir = sysfs_opendir,
    .readdir = sysfs_readdir,
    .closedir = sysfs_closedir,
    .stat = sysfs_stat,
    .mkdir = NULL,
    .rmdir = NULL,
    .unlink = NULL,
    .rename = NULL,
    .sync = sysfs_sync,
    .get_free_space = sysfs_get_free_space,
};
