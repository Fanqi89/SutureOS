/* SutureOS module: virtual file system (xj_ prefix)
 * Ported from: XJ380 (C:\Users\fanqi\Desktop\自研操作系统\XJ380\kernel\fs\vfs.c)
 * Original license: GPL-3.0
 * Changes: symbol prefix xj_; errno convention: 0=success, -1=generic,
 *          -2=NOTFOUND, -3=NOTDIR, -4=ISDIR, -5=pool exhausted, -6=conflict,
 *          -7=INVAL; no dynamic allocation (static node pool); no caching.
 */
#include "vfs.h"
#include <stitch/string.h>
#include <stitch/selftest.h>

static xj_vfs_node_t node_pool[XJ_VFS_MAX_NODES];
static uint32_t node_count = 0;
static xj_vfs_node_t *root = NULL;

int xj_vfs_init(void) {
    node_count = 0;
    root = NULL;
    memset(node_pool, 0, sizeof(node_pool));
    root = xj_vfs_create("/", XJ_VFS_NODE_DIR);
    return root ? XJ_VFS_OK : XJ_VFS_ENOSPC;
}

static xj_vfs_node_t *alloc_node(void) {
    if (node_count >= XJ_VFS_MAX_NODES) return NULL;
    return &node_pool[node_count++];
}

xj_vfs_node_t *xj_vfs_create(const char *path, xj_vfs_node_type_t type) {
    if (!path || path[0] != '/') return NULL;
    if (strcmp(path, "/") == 0) {
        if (root) return NULL;
        root = alloc_node();
        if (!root) return NULL;
        root->name[0] = '/';
        root->name[1] = '\0';
        root->type = XJ_VFS_NODE_DIR;
        root->size = 0;
        root->parent = NULL;
        root->child_count = 0;
        return root;
    }
    /* Find parent. */
    char parent_path[XJ_VFS_MAX_NAME];
    strncpy(parent_path, path, XJ_VFS_MAX_NAME - 1);
    parent_path[XJ_VFS_MAX_NAME - 1] = '\0';
    char *last_slash = strrchr(parent_path, '/');
    if (!last_slash) return NULL;
    *last_slash = '\0';
    xj_vfs_node_t *parent = xj_vfs_lookup(parent_path[0] ? parent_path : "/");
    if (!parent || parent->type != XJ_VFS_NODE_DIR) return NULL;
    if (parent->child_count >= XJ_VFS_MAX_NODES) return NULL;

    xj_vfs_node_t *node = alloc_node();
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
    if (!path || !root) return NULL;
    if (strcmp(path, "/") == 0) return root;
    if (path[0] != '/') return NULL;

    xj_vfs_node_t *cur = root;
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
    if (node == root) return XJ_VFS_EINVAL;
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

/* ---- self test ---- */
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
