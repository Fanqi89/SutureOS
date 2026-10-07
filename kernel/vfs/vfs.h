/* SutureOS module: virtual file system (xj_ prefix)
 * Ported from: XJ380 (C:\Users\fanqi\Desktop\自研操作系统\XJ380\kernel\fs\vfs.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix xj_; errno convention: 0=success, -1=generic,
 *          -2=NOTFOUND, -3=NOTDIR, -4=ISDIR, -5=pool exhausted, -6=conflict,
 *          -7=INVAL; no dynamic allocation (static node pool); no caching.
 */
#ifndef STITCH_VFS_H
#define STITCH_VFS_H

#include <stitch/types.h>

#define XJ_VFS_MAX_NAME   64
#define XJ_VFS_MAX_NODES  64
#define XJ_VFS_MAX_MOUNTS 8

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
    uint32_t data[16];  /* inline file data (1KB max) */
    struct xj_vfs_node *parent;
    struct xj_vfs_node *children[XJ_VFS_MAX_NODES];
    uint32_t child_count;
} xj_vfs_node_t;

typedef struct xj_vfs_mount {
    char mountpoint[XJ_VFS_MAX_NAME];
    xj_vfs_node_t *root;
} xj_vfs_mount_t;

int  xj_vfs_init(void);
xj_vfs_node_t *xj_vfs_create(const char *path, xj_vfs_node_type_t type);
xj_vfs_node_t *xj_vfs_lookup(const char *path);
int  xj_vfs_read(xj_vfs_node_t *node, uint32_t offset, uint32_t size, void *buf);
int  xj_vfs_write(xj_vfs_node_t *node, uint32_t offset, uint32_t size, const void *buf);
int  xj_vfs_mkdir(const char *path);
int  xj_vfs_rm(const char *path);

void st_xj_vfs_test(void);

#endif /* STITCH_VFS_H */
