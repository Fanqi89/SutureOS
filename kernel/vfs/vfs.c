/* SutureOS: Unified VFS Layer
 * Provides a common interface for multiple filesystem drivers
 */
#include "vfs.h"
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>
#include "heap.h"
#include "ext2.h"
#include "fatfs.h"
#include "squashfs.h"

/* Forward declarations for driver structs defined later in this file */
extern const vfs_fs_driver_t ext2_driver;
extern const vfs_fs_driver_t fatfs_driver;
extern const vfs_fs_driver_t squashfs_driver;
extern const vfs_fs_driver_t xj_vfs_driver;

/* ==================== Global State ==================== */

static vfs_file_t g_open_files[VFS_MAX_OPEN_FILES];
static vfs_dir_t g_open_dirs[VFS_MAX_OPEN_FILES];
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static const vfs_fs_driver_t *g_drivers[16];
static int g_driver_count = 0;
static char g_cwd[VFS_MAX_PATH] = "/";

/* ==================== Filesystem Driver Registration ==================== */

int vfs_register_driver(const vfs_fs_driver_t *driver) {
    if (!driver || g_driver_count >= 16) return VFS_ERR;
    g_drivers[g_driver_count++] = driver;
    console_printf("[vfs] Registered driver: %s\n", driver->name);
    return VFS_OK;
}

/* ==================== Helper Functions ==================== */

static const vfs_fs_driver_t *vfs_find_driver(const char *fstype) {
    for (int i = 0; i < g_driver_count; i++) {
        if (strcmp(g_drivers[i]->name, fstype) == 0) {
            return g_drivers[i];
        }
    }
    return NULL;
}

static vfs_mount_t *vfs_find_mount_for_path(const char *path) {
    vfs_mount_t *best = NULL;
    int best_len = -1;
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) continue;
        
        int len = strlen(g_mounts[i].mountpoint);
        if (len > best_len && strncmp(path, g_mounts[i].mountpoint, len) == 0) {
            if (len == 1 || path[len] == '/' || path[len] == '\0') {
                best = &g_mounts[i];
                best_len = len;
            }
        }
    }
    
    return best;
}

static int vfs_get_free_fd(void) {
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!g_open_files[i].in_use) return i;
    }
    return -1;
}

static int vfs_get_free_dir_fd(void) {
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!g_open_dirs[i].in_use) return i;
    }
    return -1;
}

/* ==================== VFS Core API ==================== */

int vfs_init(void) {
    memset(g_open_files, 0, sizeof(g_open_files));
    memset(g_open_dirs, 0, sizeof(g_open_dirs));
    memset(g_mounts, 0, sizeof(g_mounts));
    g_driver_count = 0;
    strcpy(g_cwd, "/");
    
    /* Register built-in filesystem drivers */
    extern const vfs_fs_driver_t ext2_driver;
    extern const vfs_fs_driver_t fatfs_driver;
    extern const vfs_fs_driver_t squashfs_driver;
    extern const vfs_fs_driver_t xj_vfs_driver;
    
    vfs_register_driver(&ext2_driver);
    vfs_register_driver(&fatfs_driver);
    vfs_register_driver(&squashfs_driver);
    vfs_register_driver(&xj_vfs_driver);
    
    console_puts("[vfs] VFS subsystem initialized\n");
    return VFS_OK;
}

int vfs_mount(const char *device, const char *mountpoint, const char *fstype) {
    if (!device || !mountpoint || !fstype) return VFS_EINVAL;
    
    /* Find driver */
    const vfs_fs_driver_t *driver = vfs_find_driver(fstype);
    if (!driver) {
        console_printf("[vfs] Unknown filesystem type: %s\n", fstype);
        return VFS_EINVAL;
    }
    
    /* Find free mount slot */
    vfs_mount_t *mount = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) {
            mount = &g_mounts[i];
            break;
        }
    }
    if (!mount) return VFS_ENOSPC;
    
    /* Call driver mount */
    void *fs_data = NULL;
    int ret = driver->mount(device, mountpoint, &fs_data);
    if (ret != VFS_OK) return ret;
    
    /* Fill mount info */
    strncpy(mount->mountpoint, mountpoint, VFS_MAX_PATH - 1);
    strncpy(mount->device, device, VFS_MAX_PATH - 1);
    mount->driver = driver;
    mount->fs_data = fs_data;
    mount->in_use = true;
    
    console_printf("[vfs] Mounted %s on %s (%s)\n", device, mountpoint, fstype);
    return VFS_OK;
}

int vfs_unmount(const char *mountpoint) {
    if (!mountpoint) return VFS_EINVAL;
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].mountpoint, mountpoint) == 0) {
            vfs_mount_t *mount = &g_mounts[i];
            
            /* Check for open files on this mount */
            for (int j = 0; j < VFS_MAX_OPEN_FILES; j++) {
                if (g_open_files[j].in_use && g_open_files[j].driver == mount->driver) {
                    return VFS_EBUSY;
                }
            }
            
            int ret = mount->driver->unmount(mountpoint);
            if (ret != VFS_OK) return ret;
            
            memset(mount, 0, sizeof(vfs_mount_t));
            console_printf("[vfs] Unmounted %s\n", mountpoint);
            return VFS_OK;
        }
    }
    return VFS_ENOTFOUND;
}

vfs_mount_t *vfs_find_mount(const char *path) {
    return vfs_find_mount_for_path(path);
}

const char *vfs_get_cwd(void) {
    return g_cwd;
}

void vfs_set_cwd(const char *path) {
    if (path) strncpy(g_cwd, path, VFS_MAX_PATH - 1);
}

/* ==================== File Operations ==================== */

int vfs_open(const char *path, int flags) {
    if (!path) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(path);
    if (!mount) return VFS_ENOTFOUND;
    
    int fd = vfs_get_free_fd();
    if (fd < 0) return VFS_ENOSPC;
    
    void *file_handle = NULL;
    int ret = mount->driver->open(mount->fs_data, path, flags, &file_handle);
    if (ret != VFS_OK) return ret;
    
    vfs_file_t *file = &g_open_files[fd];
    file->fs_data = mount->fs_data;
    file->file_handle = file_handle;
    file->driver = mount->driver;
    strncpy(file->path, path, VFS_MAX_PATH - 1);
    file->offset = 0;
    file->flags = flags;
    file->in_use = true;
    
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_EINVAL;
    
    vfs_file_t *file = &g_open_files[fd];
    if (!file->in_use) return VFS_EINVAL;
    
    int ret = file->driver->close(file->file_handle);
    memset(file, 0, sizeof(vfs_file_t));
    return ret;
}

int vfs_read(int fd, void *buf, uint32_t size) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_EINVAL;
    
    vfs_file_t *file = &g_open_files[fd];
    if (!file->in_use) return VFS_EINVAL;
    if (!(file->flags & VFS_O_RDONLY) && !(file->flags & VFS_O_RDWR)) return VFS_EACCES;
    
    int ret = file->driver->read(file->file_handle, buf, size, file->offset);
    if (ret > 0) file->offset += ret;
    return ret;
}

int vfs_write(int fd, const void *buf, uint32_t size) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_EINVAL;
    
    vfs_file_t *file = &g_open_files[fd];
    if (!file->in_use) return VFS_EINVAL;
    if (!(file->flags & VFS_O_WRONLY) && !(file->flags & VFS_O_RDWR)) return VFS_EACCES;
    
    int ret = file->driver->write(file->file_handle, buf, size, file->offset);
    if (ret > 0) file->offset += ret;
    return ret;
}

int vfs_seek(int fd, uint32_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_EINVAL;
    
    vfs_file_t *file = &g_open_files[fd];
    if (!file->in_use) return VFS_EINVAL;
    
    uint32_t new_off;
    switch (whence) {
        case VFS_SEEK_SET:
            new_off = offset;
            break;
        case VFS_SEEK_CUR:
            new_off = file->offset + offset;
            break;
        case VFS_SEEK_END: {
            vfs_stat_t st;
            int s = vfs_stat(file->path, &st);
            if (s != VFS_OK) return s;
            new_off = (uint32_t)st.size + offset;
            break;
        }
        default:
            return VFS_EINVAL;
    }
    
    file->offset = new_off;
    /* Let drivers that keep their own cursor know about the seek */
    if (file->driver->seek) {
        int r = file->driver->seek(file->file_handle, new_off, VFS_SEEK_SET);
        if (r < 0) return r;
    }
    return (int)new_off;
}

/* ==================== Directory Operations ==================== */

int vfs_opendir(const char *path) {
    if (!path) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(path);
    if (!mount) return VFS_ENOTFOUND;
    
    int fd = vfs_get_free_dir_fd();
    if (fd < 0) return VFS_ENOSPC;
    
    void *dir_handle = NULL;
    int ret = mount->driver->opendir(mount->fs_data, path, &dir_handle);
    if (ret != VFS_OK) return ret;
    
    vfs_dir_t *dir = &g_open_dirs[fd];
    dir->fs_data = mount->fs_data;
    dir->dir_handle = dir_handle;
    dir->driver = mount->driver;
    strncpy(dir->path, path, VFS_MAX_PATH - 1);
    dir->in_use = true;
    
    return fd;
}

int vfs_readdir(int fd, char *name, vfs_node_type_t *type) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_EINVAL;
    
    vfs_dir_t *dir = &g_open_dirs[fd];
    if (!dir->in_use) return VFS_EINVAL;
    
    uint32_t name_len = VFS_MAX_NAME;
    return dir->driver->readdir(dir->dir_handle, name, &name_len, type);
}

int vfs_closedir(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_EINVAL;
    
    vfs_dir_t *dir = &g_open_dirs[fd];
    if (!dir->in_use) return VFS_EINVAL;
    
    int ret = dir->driver->closedir(dir->dir_handle);
    memset(dir, 0, sizeof(vfs_dir_t));
    return ret;
}

/* ==================== File Metadata ==================== */

int vfs_stat(const char *path, vfs_stat_t *st) {
    if (!path || !st) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(path);
    if (!mount) return VFS_ENOTFOUND;
    
    return mount->driver->stat(mount->fs_data, path, st);
}

int vfs_mkdir(const char *path, uint32_t mode) {
    if (!path) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(path);
    if (!mount) return VFS_ENOTFOUND;
    
    return mount->driver->mkdir(mount->fs_data, path, mode);
}

int vfs_rmdir(const char *path) {
    if (!path) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(path);
    if (!mount) return VFS_ENOTFOUND;
    
    return mount->driver->rmdir(mount->fs_data, path);
}

int vfs_unlink(const char *path) {
    if (!path) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(path);
    if (!mount) return VFS_ENOTFOUND;
    
    return mount->driver->unlink(mount->fs_data, path);
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(oldpath);
    if (!mount) return VFS_ENOTFOUND;
    
    /* For simplicity, require same mount point */
    vfs_mount_t *mount2 = vfs_find_mount_for_path(newpath);
    if (mount != mount2) return VFS_EXDEV;
    
    return mount->driver->rename(mount->fs_data, oldpath, newpath);
}

/* ==================== Filesystem Info ==================== */

int vfs_sync(const char *mountpoint) {
    if (!mountpoint) {
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (g_mounts[i].in_use) {
                g_mounts[i].driver->sync(g_mounts[i].fs_data);
            }
        }
        return VFS_OK;
    }
    
    vfs_mount_t *mount = vfs_find_mount_for_path(mountpoint);
    if (!mount) return VFS_ENOTFOUND;
    
    return mount->driver->sync(mount->fs_data);
}

int vfs_get_free_space(const char *mountpoint, uint64_t *free_bytes, uint64_t *total_bytes) {
    if (!mountpoint || !free_bytes || !total_bytes) return VFS_EINVAL;
    
    vfs_mount_t *mount = vfs_find_mount_for_path(mountpoint);
    if (!mount) return VFS_ENOTFOUND;
    
    return mount->driver->get_free_space(mount->fs_data, free_bytes, total_bytes);
}

/* ==================== Mount enumeration ==================== */

int vfs_list_mounts(vfs_mount_info_t *out, int max) {
    if (!out || max <= 0) return VFS_EINVAL;
    int n = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS && n < max; i++) {
        if (!g_mounts[i].in_use) continue;
        vfs_mount_info_t *info = &out[n];
        memset(info, 0, sizeof(*info));
        strncpy(info->device, g_mounts[i].device, sizeof(info->device) - 1);
        strncpy(info->mountpoint, g_mounts[i].mountpoint, sizeof(info->mountpoint) - 1);
        info->fstype = g_mounts[i].driver->name;
        if (g_mounts[i].driver->get_free_space &&
            g_mounts[i].driver->get_free_space(g_mounts[i].fs_data,
                                               &info->free_bytes,
                                               &info->total_bytes) == VFS_OK) {
            info->space_valid = 1;
        }
        n++;
    }
    return n;
}

/* ==================== Whole-file helpers ==================== */

int vfs_write_file(const char *path, const void *buf, uint32_t len) {
    if (!path || !buf) return VFS_EINVAL;
    int fd = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) return fd;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t left = len;
    while (left > 0) {
        int w = vfs_write(fd, p, left);
        if (w <= 0) { vfs_close(fd); return (w < 0) ? w : VFS_EIO; }
        p += w;
        left -= (uint32_t)w;
    }
    vfs_close(fd);
    return VFS_OK;
}

int vfs_mkdir_p(const char *path) {
    if (!path || path[0] != '/') return VFS_EINVAL;
    char tmp[VFS_MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    /* strip trailing slash */
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            vfs_stat_t st;
            int r = vfs_stat(tmp, &st);
            if (r == VFS_ENOTFOUND) {
                int m = vfs_mkdir(tmp, 0755);
                if (m != VFS_OK) { *p = '/'; return m; }
            } else if (r == VFS_OK && st.type != VFS_NODE_DIR) {
                *p = '/';
                return VFS_ENOTDIR;
            } else if (r != VFS_OK && r != VFS_ENOTFOUND) {
                *p = '/';
                return r;
            }
            *p = '/';
        }
    }
    vfs_stat_t st;
    int r = vfs_stat(tmp, &st);
    if (r == VFS_ENOTFOUND) return vfs_mkdir(tmp, 0755);
    if (r == VFS_OK) return (st.type == VFS_NODE_DIR) ? VFS_OK : VFS_ENOTDIR;
    return r;
}

/* ==================== Legacy xj_vfs compatibility ==================== */
/* These functions maintain compatibility with code that uses the original xj_vfs API */

#include "vfs.h"  /* Original xj_vfs header */

static xj_vfs_node_t xj_node_pool[XJ_VFS_MAX_NODES];
static uint32_t xj_node_count = 0;
static xj_vfs_node_t *xj_root = NULL;

int xj_vfs_init(void) {
    xj_node_count = 0;
    xj_root = NULL;
    memset(xj_node_pool, 0, sizeof(xj_node_pool));
    xj_root = xj_vfs_create("/", XJ_VFS_NODE_DIR);
    return xj_root ? XJ_VFS_OK : XJ_VFS_ENOSPC;
}

static xj_vfs_node_t *xj_alloc_node(void) {
    if (xj_node_count >= XJ_VFS_MAX_NODES) return NULL;
    return &xj_node_pool[xj_node_count++];
}

xj_vfs_node_t *xj_vfs_create(const char *path, xj_vfs_node_type_t type) {
    if (!path || path[0] != '/') return NULL;
    if (strcmp(path, "/") == 0) {
        if (xj_root) return NULL;
        xj_root = xj_alloc_node();
        if (!xj_root) return NULL;
        xj_root->name[0] = '/';
        xj_root->name[1] = '\0';
        xj_root->type = XJ_VFS_NODE_DIR;
        xj_root->size = 0;
        xj_root->parent = NULL;
        xj_root->child_count = 0;
        return xj_root;
    }
    char parent_path[XJ_VFS_MAX_NAME];
    strncpy(parent_path, path, XJ_VFS_MAX_NAME - 1);
    parent_path[XJ_VFS_MAX_NAME - 1] = '\0';
    char *last_slash = strrchr(parent_path, '/');
    if (!last_slash) return NULL;
    *last_slash = '\0';
    xj_vfs_node_t *parent = xj_vfs_lookup(parent_path[0] ? parent_path : "/");
    if (!parent || parent->type != XJ_VFS_NODE_DIR) return NULL;
    if (parent->child_count >= XJ_VFS_MAX_NODES) return NULL;

    xj_vfs_node_t *node = xj_alloc_node();
    if (!node) return NULL;
    strncpy(node->name, last_slash + 1, XJ_VFS_MAX_NAME - 1);
    node->name[XJ_VFS_MAX_NAME - 1] = '\0';
    node->type = type;
    node->size = 0;
    node->parent = parent;
    node->child_count = 0;
    parent->children[parent->child_count++] = node;
    return node;
}

xj_vfs_node_t *xj_vfs_lookup(const char *path) {
    if (!path || !xj_root) return NULL;
    if (strcmp(path, "/") == 0) return xj_root;
    if (path[0] != '/') return NULL;

    xj_vfs_node_t *cur = xj_root;
    const char *p = path + 1;
    while (*p) {
        char name[XJ_VFS_MAX_NAME];
        uint32_t i = 0;
        while (*p && *p != '/' && i < XJ_VFS_MAX_NAME - 1) name[i++] = *p++;
        name[i] = '\0';
        if (*p == '/') p++;
        if (i == 0) continue;

        xj_vfs_node_t *next = NULL;
        for (uint32_t j = 0; j < cur->child_count; j++) {
            if (strcmp(cur->children[j]->name, name) == 0) { next = cur->children[j]; break; }
        }
        if (!next) return NULL;
        cur = next;
    }
    return cur;
}

int xj_vfs_read(xj_vfs_node_t *node, uint32_t offset, uint32_t size, void *buf) {
    if (!node || !buf) return XJ_VFS_EINVAL;
    if (node->type != XJ_VFS_NODE_FILE) return XJ_VFS_EISDIR;
    if (offset >= node->size) return XJ_VFS_OK;
    if (offset + size > node->size) size = node->size - offset;
    memcpy(buf, (uint8_t *)node->data + offset, size);
    return (int)size;
}

int xj_vfs_write(xj_vfs_node_t *node, uint32_t offset, uint32_t size, const void *buf) {
    if (!node || !buf) return XJ_VFS_EINVAL;
    if (node->type != XJ_VFS_NODE_FILE) return XJ_VFS_EISDIR;
    if (offset + size > sizeof(node->data)) return XJ_VFS_ENOSPC;
    memcpy((uint8_t *)node->data + offset, buf, size);
    if (offset + size > node->size) node->size = offset + size;
    return (int)size;
}

int xj_vfs_mkdir(const char *path) {
    xj_vfs_node_t *n = xj_vfs_create(path, XJ_VFS_NODE_DIR);
    return n ? XJ_VFS_OK : XJ_VFS_ENOTFOUND;
}

int xj_vfs_rm(const char *path) {
    xj_vfs_node_t *node = xj_vfs_lookup(path);
    if (!node) return XJ_VFS_ENOTFOUND;
    if (node == xj_root) return XJ_VFS_EINVAL;
    xj_vfs_node_t *parent = node->parent;
    if (!parent) return XJ_VFS_EINVAL;
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == node) {
            parent->children[i] = parent->children[--parent->child_count];
            return XJ_VFS_OK;
        }
    }
    return XJ_VFS_ENOTFOUND;
}

/* For loader compatibility: read a whole file via the VFS layer.
 * The returned buffer is heap-allocated (caller frees) and NUL-terminated. */
void *vfs_read_file(const char *path, size_t *len) {
    if (strcmp(path, "/init") == 0) {
        extern void *test_program_binary(size_t *len);
        /* Prefer a real /init file from a mounted FS if one exists */
        vfs_stat_t st;
        if (vfs_stat("/init", &st) != VFS_OK) {
            return test_program_binary(len);
        }
    }

    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r != VFS_OK) return NULL;

    int fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) return NULL;

    size_t cap = (size_t)st.size + 1;
    if (cap < 64) cap = 64;
    uint8_t *buf = (uint8_t *)lemis_heap_alloc(cap);
    if (!buf) { vfs_close(fd); return NULL; }

    size_t total = 0;
    while (total < cap - 1) {
        uint32_t want = (uint32_t)(cap - 1 - total);
        if (want > 512) want = 512;
        int n = vfs_read(fd, buf + total, want);
        if (n <= 0) break;
        total += (size_t)n;
    }
    vfs_close(fd);
    buf[total] = '\0';
    if (len) *len = total;
    return buf;
}

/* Self test */
void st_xj_vfs_test(void) {
    xj_vfs_init();
    bool pass = true;

    xj_vfs_node_t *f = xj_vfs_create("/test.txt", XJ_VFS_NODE_FILE);
    if (!f) pass = false;

    const char *msg = "hello";
    if (xj_vfs_write(f, 0, 5, msg) != 5) pass = false;

    char buf[16];
    if (xj_vfs_read(f, 0, 5, buf) != 5) pass = false;
    buf[5] = '\0';
    if (strcmp(buf, "hello") != 0) pass = false;

    if (xj_vfs_lookup("/test.txt") != f) pass = false;
    if (xj_vfs_lookup("/nonexistent") != NULL) pass = false;

    if (xj_vfs_mkdir("/dir") != XJ_VFS_OK) pass = false;
    if (xj_vfs_lookup("/dir") == NULL) pass = false;

    if (xj_vfs_rm("/test.txt") != XJ_VFS_OK) pass = false;
    if (xj_vfs_lookup("/test.txt") != NULL) pass = false;

    st_run("xj_vfs", pass);
}

/* ==================== xj_vfs Driver for VFS ==================== */

static int xj_vfs_driver_mount(const char *device, const char *mountpoint, void **fs_data) {
    (void)device; (void)mountpoint;
    int ret = xj_vfs_init();
    *fs_data = NULL;
    return ret == 0 ? VFS_OK : VFS_ERR;
}

static int xj_vfs_driver_unmount(const char *mountpoint) {
    (void)mountpoint;
    return VFS_OK;
}

static int xj_vfs_driver_open(void *fs_data, const char *path, int flags, void **file_handle) {
    (void)fs_data;
    xj_vfs_node_t *node = xj_vfs_lookup(path);
    if (!node) {
        if (flags & VFS_O_CREAT) {
            node = xj_vfs_create(path, XJ_VFS_NODE_FILE);
            if (!node) return VFS_ENOSPC;
        } else {
            return VFS_ENOTFOUND;
        }
    } else if (node->type != XJ_VFS_NODE_FILE) {
        return VFS_EISDIR;
    } else if (flags & VFS_O_TRUNC) {
        node->size = 0;
    }
    *file_handle = node;
    return VFS_OK;
}

static int xj_vfs_driver_close(void *file_handle) {
    (void)file_handle;
    return VFS_OK;
}

static int xj_vfs_driver_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    void *node = file_handle;
    return xj_vfs_read(node, offset, size, buf);
}

static int xj_vfs_driver_write(void *file_handle, const void *buf, uint32_t size, uint32_t offset) {
    void *node = file_handle;
    return xj_vfs_write(node, offset, size, buf);
}

static int xj_vfs_driver_seek(void *file_handle, uint32_t offset, int whence) {
    (void)file_handle; (void)offset; (void)whence;
    /* core tracks the offset; nodes are random-access */
    return VFS_OK;
}

/* Directory iteration cursor (heap-allocated per opendir) */
struct xj_dir_cursor {
    xj_vfs_node_t *node;
    uint32_t idx;
};

static int xj_vfs_driver_opendir(void *fs_data, const char *path, void **dir_handle) {
    (void)fs_data;
    xj_vfs_node_t *node = xj_vfs_lookup(path);
    if (!node) return VFS_ENOTFOUND;
    if (node->type != XJ_VFS_NODE_DIR) return VFS_ENOTDIR;
    struct xj_dir_cursor *cur = (struct xj_dir_cursor *)lemis_heap_alloc(sizeof(*cur));
    if (!cur) return VFS_ERR;
    cur->node = node;
    cur->idx = 0;
    *dir_handle = cur;
    return VFS_OK;
}

static int xj_vfs_driver_readdir(void *dir_handle, char *name, uint32_t *name_len, vfs_node_type_t *type) {
    struct xj_dir_cursor *cur = (struct xj_dir_cursor *)dir_handle;
    if (!cur || !name) return VFS_EINVAL;
    if (cur->idx >= cur->node->child_count) return VFS_ENOTFOUND;

    xj_vfs_node_t *child = cur->node->children[cur->idx++];
    strncpy(name, child->name, VFS_MAX_NAME - 1);
    name[VFS_MAX_NAME - 1] = '\0';
    if (name_len) *name_len = (uint32_t)strlen(name);
    if (type) {
        *type = (child->type == XJ_VFS_NODE_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
    }
    return VFS_OK;
}

static int xj_vfs_driver_closedir(void *dir_handle) {
    if (dir_handle) lemis_heap_free(dir_handle);
    return VFS_OK;
}

static int xj_vfs_driver_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    (void)fs_data;
    xj_vfs_node_t *node = xj_vfs_lookup(path);
    if (!node) return VFS_ENOTFOUND;
    memset(st, 0, sizeof(*st));
    if (node->type == XJ_VFS_NODE_DIR) {
        st->type = VFS_NODE_DIR;
        st->mode = 0755;
        st->size = node->child_count;
    } else {
        st->type = VFS_NODE_FILE;
        st->mode = 0644;
        st->size = node->size;
    }
    return VFS_OK;
}

static int xj_vfs_driver_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)mode;
    void *node = xj_vfs_create(path, XJ_VFS_NODE_DIR);
    return node ? VFS_OK : VFS_ENOSPC;
}

static int xj_vfs_driver_rmdir(void *fs_data, const char *path) {
    (void)fs_data;
    return xj_vfs_rm(path) == 0 ? VFS_OK : VFS_ENOTFOUND;
}

static int xj_vfs_driver_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    return xj_vfs_rm(path) == 0 ? VFS_OK : VFS_ENOTFOUND;
}

static int xj_vfs_driver_rename(void *fs_data, const char *oldpath, const char *newpath) {
    xj_vfs_node_t *node;
    xj_vfs_node_t *parent;
    xj_vfs_node_t *target_parent;
    const char *slash;
    (void)fs_data;

    if (oldpath == NULL || newpath == NULL) return VFS_EINVAL;
    node = xj_vfs_lookup(oldpath);
    if (node == NULL) return VFS_ENOTFOUND;
    if (node == xj_root) return VFS_EINVAL;          /* cannot rename "/" */

    /* SutureOS' ramfs only supports renaming inside one directory (the parents
     * must match), and only to a name that is free. */
    parent = node->parent;
    slash = strrchr(newpath, '/');
    target_parent = (slash != NULL) ? xj_vfs_lookup((slash == newpath) ? "/"
                                                    : ({ static char dir[VFS_MAX_PATH];
                                                         size_t n = (size_t)(slash - newpath);
                                                         if (n >= sizeof(dir)) n = sizeof(dir) - 1;
                                                         memcpy(dir, newpath, n);
                                                         dir[n] = '\0';
                                                         dir; }))
                                    : parent;
    if (target_parent == NULL || parent != target_parent) return VFS_EXDEV;
    if (xj_vfs_lookup(newpath) != NULL) return VFS_ECONFLICT;
    {
        const char *name = (slash != NULL) ? slash + 1 : newpath;
        if (name[0] == '\0') return VFS_EINVAL;
        if (strlen(name) >= XJ_VFS_MAX_NAME) return VFS_EINVAL;
        strncpy(node->name, name, XJ_VFS_MAX_NAME - 1);
        node->name[XJ_VFS_MAX_NAME - 1] = '\0';
    }
    return VFS_OK;
}

static int xj_vfs_driver_sync(void *fs_data) {
    (void)fs_data;
    return VFS_OK;
}

static int xj_vfs_driver_get_free_space(void *fs_data, uint64_t *free_bytes, uint64_t *total_bytes) {
    (void)fs_data;
    /* The ramfs is backed by a static node pool; capacity is measured in
     * node slots (each file node holds up to 64 bytes of inline data). */
    uint64_t total = (uint64_t)XJ_VFS_MAX_NODES * sizeof(((xj_vfs_node_t *)0)->data);
    uint64_t used  = (uint64_t)xj_node_count * sizeof(((xj_vfs_node_t *)0)->data);
    if (free_bytes) *free_bytes = (total > used) ? total - used : 0;
    if (total_bytes) *total_bytes = total;
    return VFS_OK;
}

const vfs_fs_driver_t xj_vfs_driver = {
    .name = "xj_vfs",
    .mount = xj_vfs_driver_mount,
    .unmount = xj_vfs_driver_unmount,
    .open = xj_vfs_driver_open,
    .close = xj_vfs_driver_close,
    .read = xj_vfs_driver_read,
    .write = xj_vfs_driver_write,
    .seek = xj_vfs_driver_seek,
    .opendir = xj_vfs_driver_opendir,
    .readdir = xj_vfs_driver_readdir,
    .closedir = xj_vfs_driver_closedir,
    .stat = xj_vfs_driver_stat,
    .mkdir = xj_vfs_driver_mkdir,
    .rmdir = xj_vfs_driver_rmdir,
    .unlink = xj_vfs_driver_unlink,
    .rename = xj_vfs_driver_rename,
    .sync = xj_vfs_driver_sync,
    .get_free_space = xj_vfs_driver_get_free_space,
};