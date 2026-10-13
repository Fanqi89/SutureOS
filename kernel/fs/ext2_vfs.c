/* SutureOS: ext2 filesystem driver for VFS */
#include "vfs.h"
#include <stitch/string.h>
#include <stitch/console.h>
#include "ext2.h"

/* ==================== ext2 Driver Implementation ==================== */

static int ext2_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    (void)mountpoint;
    
    /* Parse device number from /dev/sdX format */
    int drive = 0;
    uint32_t lba = 2048;
    if (strncmp(device, "/dev/sd", 7) == 0) {
        drive = device[7] - 'a';
    }
    
    int ret = ext2_mount(drive, lba);
    if (ret != 0) return VFS_ERR;
    
    *fs_data = (void*)(uintptr_t)drive;  /* Store drive number */
    return VFS_OK;
}

static int ext2_vfs_unmount(const char *mountpoint) {
    (void)mountpoint;
    return ext2_unmount() == 0 ? VFS_OK : VFS_ERR;
}

static int ext2_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    (void)fs_data; (void)flags;
    /* For simplicity, we just validate the path exists */
    uint32_t ino;
    int ret = ext2_lookup(path, &ino);
    if (ret != 0) return VFS_ENOTFOUND;
    *file_handle = (void*)(uintptr_t)ino;
    return VFS_OK;
}

static int ext2_vfs_close(void *file_handle) {
    (void)file_handle;
    return VFS_OK;
}

static int ext2_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    uint32_t ino = (uint32_t)(uintptr_t)file_handle;
    return ext2_read_file(ino, buf, offset, size);
}

static int ext2_vfs_write(void *file_handle, const void *buf, uint32_t size, uint32_t offset) {
    uint32_t ino = (uint32_t)(uintptr_t)file_handle;
    return ext2_write_file(ino, buf, offset, size);
}

static int ext2_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    (void)file_handle;
    (void)offset;
    (void)whence;
    return VFS_OK;  /* Simplified - offset tracking handled by VFS layer */
}

static int ext2_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    (void)fs_data;
    uint32_t ino;
    int ret = ext2_lookup(path, &ino);
    if (ret != 0) return VFS_ENOTFOUND;
    *dir_handle = (void*)(uintptr_t)ino;
    return VFS_OK;
}

static int ext2_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len, vfs_node_type_t *type) {
    (void)dir_handle;
    (void)name;
    (void)name_len;
    (void)type;
    return VFS_ENOTFOUND;  /* Not implemented - would need directory iteration */
}

static int ext2_vfs_closedir(void *dir_handle) {
    (void)dir_handle;
    return VFS_OK;
}

static int ext2_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    (void)fs_data;
    uint32_t ino;
    int ret = ext2_lookup(path, &ino);
    if (ret != 0) return VFS_ENOTFOUND;
    
    st->type = VFS_NODE_FILE;  /* Simplified */
    st->size = 0;
    st->mode = 0644;
    st->uid = 0;
    st->gid = 0;
    st->atime = 0;
    st->mtime = 0;
    st->ctime = 0;
    return VFS_OK;
}

static int ext2_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)path; (void)mode;
    return VFS_EPERM;  /* Read-only for now */
}

static int ext2_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    return VFS_EPERM;
}

static int ext2_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    return VFS_EPERM;
}

static int ext2_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    return VFS_EPERM;
}

static int ext2_vfs_sync(void *fs_data) {
    (void)fs_data;
    return VFS_OK;
}

static int ext2_vfs_get_free_space(void *fs_data, uint64_t *free_bytes, uint64_t *total_bytes) {
    (void)fs_data;
    if (free_bytes) *free_bytes = 1024ULL * 1024 * 1024;  /* 1GB placeholder */
    if (total_bytes) *total_bytes = 2048ULL * 1024 * 1024;  /* 2GB placeholder */
    return VFS_OK;
}

/* ==================== Driver Export ==================== */

const vfs_fs_driver_t ext2_driver = {
    .name = "ext2",
    .mount = ext2_vfs_mount,
    .unmount = ext2_vfs_unmount,
    .open = ext2_vfs_open,
    .close = ext2_vfs_close,
    .read = ext2_vfs_read,
    .write = ext2_vfs_write,
    .seek = ext2_vfs_seek,
    .opendir = ext2_vfs_opendir,
    .readdir = ext2_vfs_readdir,
    .closedir = ext2_vfs_closedir,
    .stat = ext2_vfs_stat,
    .mkdir = ext2_vfs_mkdir,
    .rmdir = ext2_vfs_rmdir,
    .unlink = ext2_vfs_unlink,
    .rename = ext2_vfs_rename,
    .sync = ext2_vfs_sync,
    .get_free_space = ext2_vfs_get_free_space,
};