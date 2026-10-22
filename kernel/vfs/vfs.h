/* SutureOS module: virtual file system (vfs_ prefix)
 * Unified VFS layer supporting multiple filesystem drivers
 * Supports: xj_vfs (ramfs), ext2, fatfs, squashfs
 */
#ifndef STITCH_VFS_H
#define STITCH_VFS_H

#include <stitch/types.h>

#define VFS_MAX_NAME      128
#define VFS_MAX_PATH      256
#define VFS_MAX_MOUNTS    16
#define VFS_MAX_OPEN_FILES 32

#define VFS_OK            0
#define VFS_ERR           -1
#define VFS_ENOTFOUND     -2
#define VFS_ENOTDIR       -3
#define VFS_EISDIR        -4
#define VFS_ENOSPC        -5
#define VFS_ECONFLICT     -6
#define VFS_EINVAL        -7
#define VFS_EPERM         -8
#define VFS_EIO           -9
#define VFS_EACCES        -10
#define VFS_EBUSY         -11
#define VFS_EXDEV         -12

typedef enum vfs_node_type {
    VFS_NODE_DIR = 0,
    VFS_NODE_FILE,
    VFS_NODE_SYMLINK,
    VFS_NODE_DEVICE,
} vfs_node_type_t;

/* Forward declaration */
struct vfs_stat;

/* Filesystem driver interface */
typedef struct vfs_fs_driver {
    const char *name;
    
    /* Mount/unmount */
    int (*mount)(const char *device, const char *mountpoint, void **fs_data);
    int (*unmount)(const char *mountpoint);
    
    /* File operations */
    int (*open)(void *fs_data, const char *path, int flags, void **file_handle);
    int (*close)(void *file_handle);
    int (*read)(void *file_handle, void *buf, uint32_t size, uint32_t offset);
    int (*write)(void *file_handle, const void *buf, uint32_t size, uint32_t offset);
    int (*seek)(void *file_handle, uint32_t offset, int whence);
    
    /* Directory operations */
    int (*opendir)(void *fs_data, const char *path, void **dir_handle);
    int (*readdir)(void *dir_handle, char *name, uint32_t *name_len, vfs_node_type_t *type);
    int (*closedir)(void *dir_handle);
    
    /* File metadata */
    int (*stat)(void *fs_data, const char *path, struct vfs_stat *st);
    int (*mkdir)(void *fs_data, const char *path, uint32_t mode);
    int (*rmdir)(void *fs_data, const char *path);
    int (*unlink)(void *fs_data, const char *path);
    int (*rename)(void *fs_data, const char *oldpath, const char *newpath);
    
    /* Filesystem info */
    int (*sync)(void *fs_data);
    int (*get_free_space)(void *fs_data, uint64_t *free_bytes, uint64_t *total_bytes);
} vfs_fs_driver_t;

/* File stat structure */
typedef struct vfs_stat {
    vfs_node_type_t type;
    uint64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
} vfs_stat_t;

/* File handle */
typedef struct vfs_file {
    void *fs_data;
    void *file_handle;
    const vfs_fs_driver_t *driver;
    char path[VFS_MAX_PATH];
    uint32_t offset;
    int flags;
    bool in_use;
} vfs_file_t;

/* Directory handle */
typedef struct vfs_dir {
    void *fs_data;
    void *dir_handle;
    const vfs_fs_driver_t *driver;
    char path[VFS_MAX_PATH];
    bool in_use;
} vfs_dir_t;

/* Mount point */
typedef struct vfs_mount {
    char mountpoint[VFS_MAX_PATH];
    char device[VFS_MAX_PATH];
    const vfs_fs_driver_t *driver;
    void *fs_data;
    bool in_use;
} vfs_mount_t;

/* Open flags */
#define VFS_O_RDONLY    0x0001
#define VFS_O_WRONLY    0x0002
#define VFS_O_RDWR      0x0003
#define VFS_O_CREAT     0x0040
#define VFS_O_TRUNC     0x0200
#define VFS_O_APPEND    0x0400

/* Seek origins */
#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

/* ==================== VFS Core API ==================== */

/* Initialize VFS subsystem */
int vfs_init(void);

/* Mount a filesystem */
int vfs_mount(const char *device, const char *mountpoint, const char *fstype);

/* Unmount a filesystem */
int vfs_unmount(const char *mountpoint);

/* File operations */
int vfs_open(const char *path, int flags);
int vfs_close(int fd);
int vfs_read(int fd, void *buf, uint32_t size);
int vfs_write(int fd, const void *buf, uint32_t size);
int vfs_seek(int fd, uint32_t offset, int whence);

/* Directory operations */
int vfs_opendir(const char *path);
int vfs_readdir(int fd, char *name, vfs_node_type_t *type);
int vfs_closedir(int fd);

/* File metadata */
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);

/* Filesystem info */
int vfs_sync(const char *mountpoint);
int vfs_get_free_space(const char *mountpoint, uint64_t *free_bytes, uint64_t *total_bytes);

/* Mount enumeration (for df and diagnostics) */
typedef struct vfs_mount_info {
    char device[64];
    char mountpoint[64];
    const char *fstype;
    uint64_t free_bytes;
    uint64_t total_bytes;
    int space_valid;          /* 1 if free/total were reported by the driver */
} vfs_mount_info_t;

int vfs_list_mounts(vfs_mount_info_t *out, int max);

/* Whole-file convenience helpers */
int vfs_write_file(const char *path, const void *buf, uint32_t len);
int vfs_mkdir_p(const char *path);   /* mkdir -p: create parents, tolerate existing */

/* Register a filesystem driver */
int vfs_register_driver(const vfs_fs_driver_t *driver);

/* Path resolution: find mount point for a path */
vfs_mount_t *vfs_find_mount(const char *path);

/* CWD access */
const char *vfs_get_cwd(void);
void vfs_set_cwd(const char *path);

/* ==================== Legacy xj_vfs API (for compatibility) ==================== */
#define XJ_VFS_MAX_NAME   64
#define XJ_VFS_MAX_NODES  64
/* per-file payload capacity (was a 64-byte uint32_t[16] array, which
 * silently truncated /etc/motd and could not hold /etc/passwd or
 * /etc/shadow - the user database needs real files) */
#define XJ_VFS_MAX_FILE_SIZE 2048

#define XJ_VFS_OK          0
#define XJ_VFS_ERR        -1
#define XJ_VFS_ENOTFOUND  -2
#define XJ_VFS_ENOTDIR    -3
#define XJ_VFS_EISDIR     -4
#define XJ_VFS_ENOSPC     -5
#define XJ_VFS_ECONFLICT  -6
#define XJ_VFS_EINVAL     -7

typedef enum xj_vfs_node_type {
    XJ_VFS_NODE_DIR = 0,
    XJ_VFS_NODE_FILE,
} xj_vfs_node_type_t;

typedef struct xj_vfs_node {
    char name[XJ_VFS_MAX_NAME];
    xj_vfs_node_type_t type;
    uint32_t size;
    uint8_t  data[XJ_VFS_MAX_FILE_SIZE];
    struct xj_vfs_node *parent;
    struct xj_vfs_node *children[XJ_VFS_MAX_NODES];
    uint32_t child_count;
} xj_vfs_node_t;

int xj_vfs_init(void);
xj_vfs_node_t *xj_vfs_create(const char *path, xj_vfs_node_type_t type);
xj_vfs_node_t *xj_vfs_lookup(const char *path);
int xj_vfs_read(xj_vfs_node_t *node, uint32_t offset, uint32_t size, void *buf);
int xj_vfs_write(xj_vfs_node_t *node, uint32_t offset, uint32_t size, const void *buf);
int xj_vfs_mkdir(const char *path);
int xj_vfs_rm(const char *path);

void *vfs_read_file(const char *path, size_t *len);

void st_xj_vfs_test(void);

#endif /* STITCH_VFS_H */
