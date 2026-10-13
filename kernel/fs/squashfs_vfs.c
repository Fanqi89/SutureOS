/* SutureOS: SquashFS filesystem driver for VFS */
#include "vfs.h"
#include <stitch/string.h>
#include <stitch/console.h>
#include "squashfs.h"

static int squashfs_vfs_mount(const char *device, const char *mountpoint, void **fs_data) {
    (void)mountpoint;
    
    int drive = 0;
    uint32_t lba = 2048;
    if (strncmp(device, "/dev/sd", 7) == 0) {
        drive = device[7] - 'a';
    }
    
    int ret = squashfs_mount(drive, lba);
    if (ret != 0) return VFS_ERR;
    
    *fs_data = (void*)(uintptr_t)drive;
    return VFS_OK;
}

static int squashfs_vfs_unmount(const char *mountpoint) {
    (void)mountpoint;
    return squashfs_unmount() == 0 ? VFS_OK : VFS_ERR;
}

static int squashfs_vfs_open(void *fs_data, const char *path, int flags, void **file_handle) {
    (void)fs_data; (void)flags;
    *file_handle = (void*)(uintptr_t)1;
    return VFS_OK;
}

static int squashfs_vfs_close(void *file_handle) {
    (void)file_handle;
    return VFS_OK;
}

static int squashfs_vfs_read(void *file_handle, void *buf, uint32_t size, uint32_t offset) {
    (void)file_handle; (void)buf; (void)size; (void)offset;
    return 0;
}

static int squashfs_vfs_write(void *file_handle, const void *buf, uint32_t size, uint32_t offset) {
    (void)file_handle; (void)buf; (void)size; (void)offset;
    return VFS_EPERM;  /* SquashFS is read-only */
}

static int squashfs_vfs_seek(void *file_handle, uint32_t offset, int whence) {
    (void)file_handle; (void)offset; (void)whence;
    return VFS_OK;
}

static int squashfs_vfs_opendir(void *fs_data, const char *path, void **dir_handle) {
    (void)fs_data; (void)path;
    *dir_handle = (void*)(uintptr_t)1;
    return VFS_OK;
}

static int squashfs_vfs_readdir(void *dir_handle, char *name, uint32_t *name_len, vfs_node_type_t *type) {
    (void)dir_handle; (void)name; (void)name_len; (void)type;
    return VFS_ENOTFOUND;
}

static int squashfs_vfs_closedir(void *dir_handle) {
    (void)dir_handle;
    return VFS_OK;
}

static int squashfs_vfs_stat(void *fs_data, const char *path, struct vfs_stat *st) {
    (void)fs_data; (void)path;
    st->type = VFS_NODE_FILE;
    st->size = 0;
    st->mode = 0444;
    st->uid = 0;
    st->gid = 0;
    st->atime = 0;
    st->mtime = 0;
    st->ctime = 0;
    return VFS_OK;
}

static int squashfs_vfs_mkdir(void *fs_data, const char *path, uint32_t mode) {
    (void)fs_data; (void)path; (void)mode;
    return VFS_EPERM;  /* Read-only */
}

static int squashfs_vfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    return VFS_EPERM;
}

static int squashfs_vfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    return VFS_EPERM;
}

static int squashfs_vfs_rename(void *fs_data, const char *oldpath, const char *newpath) {
    (void)fs_data; (void)oldpath; (void)newpath;
    return VFS_EPERM;
}

static int squashfs_vfs_sync(void *fs_data) {
    (void)fs_data;
    return VFS_OK;
}

static int squashfs_vfs_get_free_space(void *fs_data, uint64_t *free_bytes, uint64_t *total_bytes) {
    (void)fs_data;
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    return VFS_OK;
}

const vfs_fs_driver_t squashfs_driver = {
    .name = "squashfs",
    .mount = squashfs_vfs_mount,
    .unmount = squashfs_vfs_unmount,
    .open = squashfs_vfs_open,
    .close = squashfs_vfs_close,
    .read = squashfs_vfs_read,
    .write = squashfs_vfs_write,
    .seek = squashfs_vfs_seek,
    .opendir = squashfs_vfs_opendir,
    .readdir = squashfs_vfs_readdir,
    .closedir = squashfs_vfs_closedir,
    .stat = squashfs_vfs_stat,
    .mkdir = squashfs_vfs_mkdir,
    .rmdir = squashfs_vfs_rmdir,
    .unlink = squashfs_vfs_unlink,
    .rename = squashfs_vfs_rename,
    .sync = squashfs_vfs_sync,
    .get_free_space = squashfs_vfs_get_free_space,
};