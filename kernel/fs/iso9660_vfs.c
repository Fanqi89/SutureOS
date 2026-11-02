/* SutureOS: ISO9660 filesystem driver for the unified VFS
 *
 * Thin adapter between vfs_fs_driver_t (kernel/vfs/vfs.h) and the real ISO9660
 * core in kernel/fs/iso9660.c - the same shape as kernel/fs/ext2_vfs.c.
 * Driver name is "iso9660" so `mount iso9660 14 /mnt/cd` finds it
 * (device = block-device drive number; ATAPI CD-ROMs are drives 12..15).
 *
 * READ-ONLY: kernel/vfs/vfs.h has no VFS_EROFS code, so every mutating call
 * returns VFS_EROFS, aliased to VFS_EPERM below (the closest honest code the
 * VFS can report) after logging the rejection once.
 *
 * 来源 / Source: CoolPotOS module/iso9660 (MIT) 的挂载/回调结构作为参考，
 * 适配层本身按 SutureOS 的 ext2_vfs.c 写。许可证：MIT + GPL-3.0(SutureOS 本体)。
 */
#include "vfs.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "iso9660.h"
#include "block.h"

/* vfs.h has no EROFS; alias it so the intent is explicit at every call site. */
#define VFS_EROFS VFS_EPERM

/* ---- single mounted volume ---- */
static char g_mountpoint[VFS_MAX_PATH] = "";
static int  g_ro_logged = 0;

static void iso_ro_log(const char *what) {
    if (g_ro_logged) return;
    g_ro_logged = 1;
    console_printf("[iso9660] read-only filesystem: %s rejected\n", what);
}

static int map_err(int e) {
    switch (e) {
        case ISO9660_OK:      return VFS_OK;
        case ISO9660_ENOENT:  return VFS_ENOTFOUND;
        case ISO9660_ENOTDIR: return VFS_ENOTDIR;
        case ISO9660_ENOTFILE: return VFS_EISDIR;   /* 目标是目录 */
        case ISO9660_EINVAL:  return VFS_EINVAL;
        case ISO9660_EIO:     return VFS_EIO;
        case ISO9660_EBADFS:  return VFS_EIO;
        case ISO9660_ENOSPC:  return VFS_ENOSPC;
        default:              return VFS_ERR;
    }
}

/* Device strings understood here:
 *   "12".."15"          block drive number (ATAPI CD-ROM drives)
 *   "cd0" .. "cd3"      ATAPI IDE position 0..3  (position -> drive 12+pos)
 *   "atapi0".."atapi3"  same as cd0..cd3
 *   any of the above with ":<lba>" to start at a partition offset
 */
static int parse_device(const char *device, int *out_drive, uint32_t *out_lba) {
    const char *p = device;
    int drive = -1;
    uint32_t lba = 0;

    *out_drive = 0;
    *out_lba = 0;
    if (!device) return VFS_EINVAL;
    if (strncmp(p, "/dev/", 5) == 0) p += 5;

    if ((p[0] == 'c' || p[0] == 'C') && (p[1] == 'd' || p[1] == 'D') &&
        p[2] >= '0' && p[2] <= '9') {
        drive = BLK_DRIVE_ATAPI_BASE + (p[2] - '0');
        p += 3;
    } else if (strncmp(p, "atapi", 5) == 0 && p[5] >= '0' && p[5] <= '9') {
        drive = BLK_DRIVE_ATAPI_BASE + (p[5] - '0');
        p += 6;
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
    if (drive < 0) return VFS_EINVAL;
    *out_drive = drive;
    *out_lba = lba;
    return VFS_OK;
}

/* VFS path -> volume-relative path ("/README.TXT"), stripping the mountpoint. */
static int vol_path(const char *path, char *out, uint32_t cap) {
    const char *rel = path;
    size_t mplen;

    if (!path || !out || cap == 0) return VFS_EINVAL;
    mplen = strlen(g_mountpoint);
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

static int iso_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    int drive = 0, r;
    uint32_t lba = 0;

    if (!mountpoint) return VFS_EINVAL;
    r = parse_device(device, &drive, &lba);
    if (r != VFS_OK) return r;
    if (!blk_find_by_drive(drive)) {
        console_printf("[iso9660] mount failed: no block device at drive %d\n", drive);
        return VFS_ENOTFOUND;
    }
    r = iso9660_mount(drive, lba);
    if (r != ISO9660_OK) return map_err(r);

    strncpy(g_mountpoint, mountpoint, sizeof(g_mountpoint) - 1);
    g_mountpoint[sizeof(g_mountpoint) - 1] = '\0';
    g_ro_logged = 0;
    if (fs_data) *fs_data = (void *)(uintptr_t)(uint32_t)drive;
    return VFS_OK;
}

static int iso_vfs_unmount(const char *mountpoint) {
    (void)mountpoint;
    iso9660_unmount();
    g_mountpoint[0] = '\0';
    return VFS_OK;
}

static int iso_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    char vp[VFS_MAX_PATH];
    struct iso9660_file *fh = NULL;
    int acc = flags & VFS_O_RDWR;      /* 0 = 未指定, 1 RDONLY, 2 WRONLY, 3 RDWR */
    int r;

    (void)fs_data;
    if (!file_handle) return VFS_EINVAL;
    if (acc == VFS_O_WRONLY || acc == VFS_O_RDWR ||
        (flags & (VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0) {
        iso_ro_log("open for writing");
        return VFS_EROFS;
    }
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = iso9660_open(vp, &fh);
    if (r != ISO9660_OK) return map_err(r);
    *file_handle = fh;
    return VFS_OK;
}

static int iso_vfs_close(void *file_handle) {
    if (!file_handle) return VFS_EINVAL;
    return map_err(iso9660_close((struct iso9660_file *)file_handle));
}

static int iso_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    int n;

    if (!file_handle) return VFS_EINVAL;
    n = iso9660_file_read((struct iso9660_file *)file_handle, buf, size, offset);
    return (n < 0) ? map_err(n) : n;      /* >= 0 = 读到的字节数 */
}

static int iso_vfs_write(void *file_handle, const void *buf, uint32_t size,
                         uint32_t offset) {
    (void)file_handle; (void)buf; (void)size; (void)offset;
    iso_ro_log("write");
    return VFS_EROFS;
}

static int iso_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    /* VFS 自己维护偏移，总是按 SEEK_SET 语义把绝对偏移传给 read(); ISO9660
     * 是随机访问，所以这里只接受 SEEK_SET。 */
    (void)file_handle; (void)offset;
    return (whence == VFS_SEEK_SET) ? VFS_OK : VFS_EPERM;
}

static int iso_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    char vp[VFS_MAX_PATH];
    struct iso9660_dir *d = NULL;
    int r;

    (void)fs_data;
    if (!dir_handle) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = iso9660_opendir(vp, &d);
    if (r != ISO9660_OK) return map_err(r);
    *dir_handle = d;
    return VFS_OK;
}

static int iso_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len,
                           vfs_node_type_t *type) {
    uint32_t len = 0;
    int is_dir = 0;
    int r;

    if (!dir_handle || !name) return VFS_EINVAL;
    r = iso9660_readdir((struct iso9660_dir *)dir_handle, name, VFS_MAX_NAME,
                        &len, &is_dir);
    if (r == ISO9660_ENOENT) return VFS_ENOTFOUND;     /* 目录结束 */
    if (r != ISO9660_OK) return map_err(r);
    if (name_len) *name_len = len;
    if (type) *type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    return VFS_OK;
}

static int iso_vfs_closedir(void *dir_handle) {
    if (!dir_handle) return VFS_EINVAL;
    return map_err(iso9660_closedir((struct iso9660_dir *)dir_handle));
}

static int iso_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    char vp[VFS_MAX_PATH];
    struct iso9660_stat is;
    int r;

    (void)fs_data;
    if (!st) return VFS_EINVAL;
    r = vol_path(path, vp, sizeof(vp));
    if (r != VFS_OK) return r;

    r = iso9660_lookup(vp, &is);
    if (r != ISO9660_OK) return map_err(r);

    memset(st, 0, sizeof(*st));
    st->type = is.is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    st->size = is.size;
    st->mode = is.is_dir ? 0555u : 0444u;    /* 只读介质 */
    return VFS_OK;
}

static int iso_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)path; (void)mode;
    iso_ro_log("mkdir");
    return VFS_EROFS;
}

static int iso_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    iso_ro_log("rmdir");
    return VFS_EROFS;
}

static int iso_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    iso_ro_log("unlink");
    return VFS_EROFS;
}

static int iso_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    iso_ro_log("rename");
    return VFS_EROFS;
}

static int iso_vfs_sync(void *fs_data) {
    (void)fs_data;
    return VFS_OK;                        /* 只读，没有要刷的数据 */
}

static int iso_vfs_get_free_space(void *fs_data, uint64_t *free_bytes,
                                  uint64_t *total_bytes) {
    (void)fs_data;
    if (!iso9660_is_mounted()) return VFS_EBUSY;
    if (free_bytes) *free_bytes = 0;      /* 只读介质：可写空间视为 0 */
    if (total_bytes) *total_bytes = (uint64_t)g_iso9660.vol_blocks * ISO9660_BLOCK_SIZE;
    return VFS_OK;
}

/* ==================== driver export ==================== */

const vfs_fs_driver_t iso9660_driver = {
    .name = "iso9660",
    .mount = iso_vfs_mount,
    .unmount = iso_vfs_unmount,
    .open = iso_vfs_open,
    .close = iso_vfs_close,
    .read = iso_vfs_read,
    .write = iso_vfs_write,
    .seek = iso_vfs_seek,
    .opendir = iso_vfs_opendir,
    .readdir = iso_vfs_readdir,
    .closedir = iso_vfs_closedir,
    .stat = iso_vfs_stat,
    .mkdir = iso_vfs_mkdir,
    .rmdir = iso_vfs_rmdir,
    .unlink = iso_vfs_unlink,
    .rename = iso_vfs_rename,
    .sync = iso_vfs_sync,
    .get_free_space = iso_vfs_get_free_space,
};

/* ==================== 开机自动挂载 ==================== */

/* 扫描所有 ATAPI 光盘，挑一张“数据盘”挂到 mountpoint 并跑自检。
 *
 * 为什么不是见到光盘就挂：本机的启动介质本身就是 limine 的 El Torito
 * ISO（也是 ISO9660），把它当数据盘挂到 /mnt/cd 会把 /boot/limine 之类
 * 的内部文件暴露成“光盘内容”，还会让自检在启动盘上误判。判据用规范的
 * El Torito 引导记录（卷描述符类型 0）：
 *   - 没有引导记录 => 数据盘，挂载；
 *   - 有引导记录   => 启动盘，跳过并如实打印一行原因。
 * 只有一张可引导光盘时（没有插数据盘）不挂载、不 PANIC，只打印一行说明；
 * 用户仍可手工 `mount iso9660 14 /mnt/cd` 强行挂载启动盘。
 * 完全没有 ATAPI 设备时本函数直接返回，不打印任何日志。 */
int iso9660_automount(const char *mountpoint) {
    int count = blk_atapi_device_count();
    int boot_drive = -1;

    if (!mountpoint) return VFS_EINVAL;
    if (count <= 0) return VFS_ENOTFOUND;        /* 没有光驱：安静返回 */

    for (int i = 0; i < count; i++) {
        struct iso9660_volume vol;
        int drive = blk_atapi_drive_at(i);
        char src[8];

        if (drive < 0) continue;
        if (iso9660_probe(drive, 0, &vol) != ISO9660_OK) continue;
        if (vol.has_boot_record) {
            if (boot_drive < 0) boot_drive = drive;
            continue;
        }
        snprintf(src, sizeof(src), "%d", drive);
        if (vfs_mount(src, mountpoint, "iso9660") != VFS_OK) {
            console_printf("[iso9660] automount drive %d failed\n", drive);
            return VFS_ERR;
        }
        (void)iso9660_selftest();
        return VFS_OK;
    }

    if (boot_drive >= 0) {
        console_printf("[iso9660] skip drive %d: El Torito boot disc, "
                       "not a data disc\n", boot_drive);
    }
    return VFS_ENOTFOUND;
}
