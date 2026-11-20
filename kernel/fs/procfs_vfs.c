/* SutureOS procfs: a synthetic, read-only filesystem mounted on /proc.
 *
 * Why: the file-system domain audit showed the VFS had a uniform driver model
 * but no generated (non-disk) filesystem, so live kernel state could not be read
 * as files. This provides the read-only half of the procfs idea from the porting
 * board without touching any on-disk driver.
 *
 * Mechanism sources (read-only reference, no upstream code copied):
 *   - XJ380 `driver/fs/vfs/vfs.cpp` procfs node-table idea (Apache-2.0)
 *   - LunaixOS POSIX-VFS procfs layout (reference)
 *   - the Linux /proc text conventions (key: value lines)
 * Implemented from scratch for SutureOS (GPL-3.0).
 *
 * VFS contract used here (verified against kernel/vfs/vfs.c):
 *   - driver->open(fs_data, path, flags, &handle): `flags` uses VFS_O_RDONLY=1,
 *     VFS_O_WRONLY=2, VFS_O_RDWR=3; the read/write permission bits are flags&3.
 *     A read-only driver must reject mode==WRONLY or mode==RDWR - testing
 *     `flags & (VFS_O_WRONLY|VFS_O_RDWR)` is wrong because that mask includes
 *     the RDONLY bit and rejects every read open (this was the real reason the
 *     first procfs attempt returned "operation not supported" for `cat`).
 *   - driver->read(handle, buf, size, offset): VFS tracks the offset and adds
 *     the returned byte count.
 *   - driver->stat(fs_data, path, st): must report a size for files.
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "pmm.h"                 /* moe_pmm_free_count() */
#include <sutureos/version.h>    /* SUTUREOS_VERSION */

#define PROC_COUNT 5
#define PROC_HANDLES 8
#define PROC_BUF_MAX 768

static const char *const g_proc_names[PROC_COUNT] = {
    "meminfo", "uptime", "version", "cpuinfo", "mounts",
};

static char g_proc_mp[VFS_MAX_PATH] = "/proc";

struct procfs_handle {
    int used;
    int idx;
    char text[PROC_BUF_MAX];
    int len;
};

static struct procfs_handle g_handles[PROC_HANDLES];

extern volatile uint64_t g_tick_count;
extern volatile uint8_t g_ap_online[];

static uint32_t procfs_online_cpus(void)
{
    uint32_t n = 0;
    for (int i = 0; i < 256; i++)
        if (g_ap_online[i]) n++;
    return n ? n : 1u;
}

/* Normalise a VFS path to the procfs-relative form ("/proc/meminfo" -> "meminfo",
 * "/proc" -> ""). Returns 0 on success. */
static int procfs_rel(const char *path, const char **out)
{
    if (!path) return -1;
    const size_t mplen = strlen(g_proc_mp);
    if (mplen > 1 && strncmp(path, g_proc_mp, mplen) == 0) path += mplen;
    while (*path == '/') path++;
    *out = path;
    return 0;
}

/* 0..PROC_COUNT-1 = file, -1 = the procfs root, -2 = unknown. */
static int procfs_lookup(const char *path)
{
    const char *rel = NULL;
    if (procfs_rel(path, &rel) != 0) return -2;
    if (*rel == '\0') return -1;
    for (int i = 0; i < PROC_COUNT; i++) {
        const char *name = g_proc_names[i];
        size_t k = 0;
        while (name[k] && rel[k] == name[k]) k++;
        if (name[k] == '\0' && (rel[k] == '\0' || rel[k] == '/')) return i;
    }
    return -2;
}

static int procfs_build(int idx, char *buf, uint32_t cap)
{
    int n = 0;
    switch (idx) {
    case 0: {   /* meminfo */
        const uint64_t free_pages = moe_pmm_free_count();
        n = snprintf(buf, cap,
                     "MemTotal:       %u kB\n"
                     "MemFree:        %u kB\n"
                     "PageSize:       4 kB\n"
                     "MemFreePages:   %u\n",
                     (unsigned)(free_pages * 4ull),
                     (unsigned)(free_pages * 4ull),
                     (unsigned)free_pages);
        break;
    }
    case 1: {   /* uptime */
        const unsigned up_s = (unsigned)(g_tick_count / 100ull);
        const unsigned up_cs = (unsigned)(g_tick_count % 100ull);
        n = snprintf(buf, cap, "%u.%u %u.%u\n", up_s, up_cs, up_s, up_cs);
        break;
    }
    case 2:     /* version */
        n = snprintf(buf, cap,
                     "SutureOS kernel version %s (GPL-3.0)\n"
                     "A stitched-together hobby OS: subsystems ported from "
                     "15+ upstream kernels, GUI look written in-house.\n",
                     SUTUREOS_VERSION);
        break;
    case 3:     /* cpuinfo */
        n = snprintf(buf, cap,
                     "processor\t: 0\n"
                     "vendor_id\t: SutureOS\n"
                     "cpu family\t: 6\n"
                     "model name\t: x86_64 hobby kernel target\n"
                     "cpu cores\t: %u\n",
                     (unsigned)procfs_online_cpus());
        break;
    case 4: {   /* mounts */
        vfs_mount_info_t infos[VFS_MAX_MOUNTS];
        const int count = vfs_list_mounts(infos, VFS_MAX_MOUNTS);
        for (int i = 0; i < count && n < (int)cap - 96; i++) {
            n += snprintf(buf + n, cap - (uint32_t)n, "%s %s %s 0 0\n",
                          infos[i].device, infos[i].mountpoint,
                          infos[i].fstype ? infos[i].fstype : "none");
            if (n >= (int)cap) { n = (int)cap - 1; break; }
        }
        break;
    }
    default:
        return 0;
    }
    if (n < 0) return 0;
    if (n >= (int)cap) n = (int)cap - 1;
    buf[n] = '\0';
    return n;
}

static int procfs_mount(const char *device, const char *mountpoint, void **fs_data)
{
    (void)device;
    if (!mountpoint) return VFS_EINVAL;
    strncpy(g_proc_mp, mountpoint, sizeof(g_proc_mp) - 1);
    g_proc_mp[sizeof(g_proc_mp) - 1] = '\0';
    memset(g_handles, 0, sizeof(g_handles));
    if (fs_data) *fs_data = NULL;
    console_printf("[procfs] mounted at %s (5 generated files)\n", g_proc_mp);
    return VFS_OK;
}

static int procfs_unmount(const char *mountpoint)
{
    (void)mountpoint;
    memset(g_handles, 0, sizeof(g_handles));
    return VFS_OK;
}

static int procfs_open(void *fs_data, const char *path, int flags, void **file_handle)
{
    (void)fs_data;
    const int mode = flags & 0x3;                 /* VFS_O_RDONLY / WRONLY / RDWR */
    if (mode == VFS_O_WRONLY || mode == VFS_O_RDWR) return VFS_EPERM;

    const int idx = procfs_lookup(path);
    if (idx == -1) return VFS_EISDIR;
    if (idx < 0) return VFS_ENOTFOUND;

    for (int i = 0; i < PROC_HANDLES; i++) {
        if (g_handles[i].used) continue;
        g_handles[i].used = 1;
        g_handles[i].idx = idx;
        g_handles[i].len = procfs_build(idx, g_handles[i].text, sizeof(g_handles[i].text));
        if (file_handle) *file_handle = &g_handles[i];
        return VFS_OK;
    }
    return VFS_ENOSPC;
}

static int procfs_close(void *file_handle)
{
    struct procfs_handle *h = (struct procfs_handle *)file_handle;
    if (h) memset(h, 0, sizeof(*h));
    return VFS_OK;
}

static int procfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset)
{
    struct procfs_handle *h = (struct procfs_handle *)file_handle;
    if (!h || !h->used || !buf) return VFS_EINVAL;
    if (offset >= (uint32_t)h->len) return 0;
    uint32_t n = (uint32_t)h->len - offset;
    if (n > size) n = size;
    memcpy(buf, h->text + offset, n);
    return (int)n;
}

static int procfs_write(void *file_handle, const void *buf, uint32_t size, uint32_t offset)
{
    (void)file_handle; (void)buf; (void)size; (void)offset;
    return VFS_EPERM;
}

static int procfs_seek(void *file_handle, uint32_t offset, int whence)
{
    (void)file_handle; (void)offset; (void)whence;
    return VFS_OK;
}

static int g_proc_dir_cursor;
static int procfs_opendir(void *fs_data, const char *path, void **dir_handle)
{
    (void)fs_data;
    const int idx = procfs_lookup(path);
    if (idx == -2) return VFS_ENOTFOUND;
    if (idx >= 0) return VFS_ENOTDIR;
    g_proc_dir_cursor = 0;
    if (dir_handle) *dir_handle = &g_proc_dir_cursor;
    return VFS_OK;
}

static int procfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                          vfs_node_type_t *type)
{
    int *cursor = (int *)dir_handle;
    if (!cursor || !name || !name_len) return VFS_EINVAL;
    if (*cursor >= PROC_COUNT) return VFS_ENOTFOUND;
    const char *n = g_proc_names[(*cursor)++];
    const size_t len = strlen(n);
    if (len + 1 > VFS_MAX_NAME) return VFS_EIO;
    memcpy(name, n, len + 1);
    *name_len = (uint32_t)len;
    if (type) *type = VFS_NODE_FILE;
    return VFS_OK;
}

static int procfs_closedir(void *dir_handle)
{
    (void)dir_handle;
    return VFS_OK;
}

static int procfs_stat(void *fs_data, const char *path, struct vfs_stat *st)
{
    (void)fs_data;
    if (!st) return VFS_EINVAL;
    memset(st, 0, sizeof(*st));
    const int idx = procfs_lookup(path);
    if (idx == -1) {
        st->type = VFS_NODE_DIR;
        st->mode = 0555;
        return VFS_OK;
    }
    if (idx < 0) return VFS_ENOTFOUND;
    st->type = VFS_NODE_FILE;
    st->mode = 0444;
    st->size = (uint64_t)procfs_build(idx, g_handles[0].text, sizeof(g_handles[0].text));
    return VFS_OK;
}

static int procfs_sync(void *fs_data) { (void)fs_data; return VFS_OK; }

static int procfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                 uint64_t *total_bytes)
{
    (void)fs_data;
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    return VFS_EPERM;                            /* no meaningful capacity */
}

const vfs_fs_driver_t procfs_driver = {
    .name = "procfs",
    .mount = procfs_mount,
    .unmount = procfs_unmount,
    .open = procfs_open,
    .close = procfs_close,
    .read = procfs_read,
    .write = procfs_write,
    .seek = procfs_seek,
    .opendir = procfs_opendir,
    .readdir = procfs_readdir,
    .closedir = procfs_closedir,
    .stat = procfs_stat,
    .mkdir = NULL,
    .rmdir = NULL,
    .unlink = NULL,
    .rename = NULL,
    .sync = procfs_sync,
    .get_free_space = procfs_get_free_space,
};
