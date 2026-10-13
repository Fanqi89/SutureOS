/* SutureOS module: VimtuFS2 (v4) API layer - 移植自 VimtuOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\vfs64.cpp
 * Original license: GPL-3.0
 *
 * Rewritten as a REAL implementation: every operation now delegates to the
 * unified SutureOS VFS (vfs_* core API + mounted drivers: xj_vfs ramfs
 * root, FAT32, ...). The old version returned fake success / -1 stubs.
 *
 * Mapping decisions (honest by design):
 *  - "Current volume" = the unified VFS namespace (root mount and friends).
 *  - Volume slots (multi-volume): only slot 0 exists and maps to the
 *    unified namespace; _on64 calls with slot != 0 return -1.
 *  - vfs64_format/mount/probe: volume management is done by the unified
 *    VFS (vfs_mount/vfs_unmount + drivers); these return -1.
 *  - chmod/chown/mtime: the VFS does not store per-file ownership or
 *    timestamps yet; those calls succeed as documented no-ops.
 */
#include "vfs64.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>
#include "heap.h"
#include "vfs.h"

/* ==================== 全局状态 ==================== */

static int g_dirstreams[VFS64_DIRSTREAM_MAX];   /* vfs dir fds, 0 = free */

int vfs64_perm_fields64(void) { return 1; }

void vfs64_set_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    (void)uid; (void)gid; (void)euid; (void)egid;   /* single-user kernel */
}

void vfs64_get_cred64(struct Vfs64Cred64* out) {
    if (out) { out->uid = 0; out->gid = 0; out->euid = 0; out->egid = 0; }
}

void vfs64_set_proc_cred64(int have, uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    (void)have; (void)uid; (void)gid; (void)euid; (void)egid;
}

uint32_t vfs64_umask64(uint32_t new_mask) { (void)new_mask; return 022; }
uint32_t vfs64_get_umask64(void) { return 022; }

uint32_t vfs64_blocks_for_bytes64(uint32_t bytes) {
    if (bytes == 0) return 0;
    return (bytes + 511) / 512;
}

uint32_t vfs64_max_file_bytes64(void) { return VFS64_MAX_FILE_BYTES; }

/* ==================== 路径归一化 ==================== */

/* Accept "C:/foo", "c:/foo" (VimtuOS drive prefix) and "/foo"; always
 * produce an absolute VFS path. */
static int vfs64_norm(const char* path, char* out, uint32_t cap) {
    if (!path || !out || cap < 2) return -1;
    if (path[0] >= 'A' && path[0] <= 'Z' && path[1] == ':') path += 2;
    else if (path[0] >= 'a' && path[0] <= 'z' && path[1] == ':') path += 2;
    if (path[0] != '/') {
        /* relative -> anchor at root */
        if (cap < 2) return -1;
        out[0] = '/';
        strncpy(out + 1, path, cap - 2);
        out[cap - 1] = '\0';
        return 0;
    }
    strncpy(out, path, cap - 1);
    out[cap - 1] = '\0';
    return 0;
}

static const char* vfs64_basename(const char* norm_path) {
    const char* slash = NULL;
    for (const char* p = norm_path; *p; p++) {
        if (*p == '/') slash = p;
    }
    return slash ? slash + 1 : norm_path;
}

static uint32_t vfs64_kind_by_name_local(uint32_t type, const char* name) {
    if (type == VFS64_TYPE_DIR) return VFS64_KIND_DIR;
    const char* dot = NULL;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return VFS64_KIND_BIN;
    if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".md") == 0) return VFS64_KIND_TEXT;
    if (strcmp(dot, ".elf") == 0 || strcmp(dot, ".so") == 0) return VFS64_KIND_ELF;
    return VFS64_KIND_BIN;
}

/* ==================== 权限类（文档化 no-op） ==================== */

int vfs64_access64(const char* path, uint32_t mask) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    vfs_stat_t st;
    if (vfs_stat(p, &st) != VFS_OK) return -1;
    if (mask & 0x2) {   /* W_OK: try opening for write */
        int fd = vfs_open(p, VFS_O_RDWR);
        if (fd < 0) return VFS64_EACCES;
        vfs_close(fd);
    }
    return 0;
}

int vfs64_chmod64(const char* path, uint32_t mode) {
    /* Documented no-op: the VFS does not persist mode bits yet. */
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    vfs_stat_t st;
    return (vfs_stat(p, &st) == VFS_OK) ? 0 : -1;
}

int vfs64_chown64(const char* path, uint32_t uid, uint32_t gid) {
    (void)uid; (void)gid;
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    vfs_stat_t st;
    return (vfs_stat(p, &st) == VFS_OK) ? 0 : -1;
}

int vfs64_chmod_on64(int slot, const char* path, uint32_t mode) {
    return (slot == 0) ? vfs64_chmod64(path, mode) : -1;
}
int vfs64_chown_on64(int slot, const char* path, uint32_t uid, uint32_t gid) {
    return (slot == 0) ? vfs64_chown64(path, uid, gid) : -1;
}

int vfs64_scope_enter64(int slot, int* out_saved) {
    if (slot != 0) return -1;
    if (out_saved) *out_saved = 0;
    return 0;
}
void vfs64_scope_leave64(int saved_slot) { (void)saved_slot; }

/* ==================== 卷生命周期（真实语义：诚实拒绝） ==================== */

int vfs64_format(int drive, uint32_t start_lba, uint32_t total_sectors) {
    (void)drive; (void)start_lba; (void)total_sectors;
    console_puts("[vfs64] format: not supported (use the target FS's mkfs tooling)\n");
    return -1;
}

int vfs64_mount(int drive, uint32_t start_lba) {
    (void)drive; (void)start_lba;
    console_puts("[vfs64] mount: volume management moved to unified VFS "
                 "(see shell: mount <type> <device> <mountpoint>)\n");
    return -1;
}

int vfs64_probe_volume64(int drive, uint32_t start_lba, struct Vfs64VolInfo64* out) {
    (void)drive; (void)start_lba; (void)out;
    return -1;   /* no VimtuFS2-formatted volumes are supported */
}

int vfs64_mounted_volume64(int* drive, uint32_t* start_lba, struct Vfs64VolInfo64* out) {
    (void)drive; (void)start_lba; (void)out;
    return -1;
}

int vfs64_mount_system64(int drive, uint32_t start_lba) {
    return vfs64_mount(drive, start_lba);
}
int vfs64_mount_slot64(int slot, int drive, uint32_t start_lba) {
    (void)slot; (void)drive; (void)start_lba;
    return -1;
}
int vfs64_activate_slot64(int slot) { return (slot == 0) ? 0 : -1; }
int vfs64_current_slot64(void) { return 0; }
int vfs64_system_slot64(void) { return 0; }
int vfs64_slot_used64(int slot) { return (slot == 0) ? 1 : 0; }
int vfs64_slot_alloc64(void) { return 0; }   /* slot 0 = unified namespace */
int vfs64_slot_find64(int drive, uint32_t start_lba) {
    (void)drive; (void)start_lba;
    return 0;
}
int  vfs64_slot_info64(int slot, int* drive, uint32_t* start_lba, struct Vfs64VolInfo64* out) {
    (void)slot; (void)drive; (void)start_lba; (void)out;
    return -1;
}

static void vfs64_dump_tree(const char* path, int depth, int* entries, int max_entries) {
    int d = vfs_opendir(path);
    if (d < 0) return;
    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    while (*entries < max_entries && vfs_readdir(d, name, &type) == VFS_OK) {
        char child[VFS64_PATH_MAX];
        snprintf(child, sizeof(child), "%s%s%s", path,
                 (strcmp(path, "/") == 0) ? "" : "/", name);
        for (int i = 0; i < depth; i++) console_puts("  ");
        console_printf("%s%s\n", name, (type == VFS_NODE_DIR) ? "/" : "");
        (*entries)++;
        if (type == VFS_NODE_DIR) {
            vfs64_dump_tree(child, depth + 1, entries, max_entries);
        }
    }
    vfs_closedir(d);
}

void vfs64_slots_dump64(void) {
    vfs_mount_info_t mounts[VFS_MAX_MOUNTS];
    int n = vfs_list_mounts(mounts, VFS_MAX_MOUNTS);
    console_printf("[vfs64] unified VFS mounts: %d\n", n);
    for (int i = 0; i < n; i++) {
        console_printf("  %s on %s (%s) free=%llu total=%llu\n",
                       mounts[i].device, mounts[i].mountpoint, mounts[i].fstype,
                       (unsigned long long)mounts[i].free_bytes,
                       (unsigned long long)mounts[i].total_bytes);
    }
}

/* ==================== 元数据 ==================== */

int vfs64_stat64(const char* path, struct Vfs64Info64* out) {
    char p[VFS64_PATH_MAX];
    if (!out || vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    vfs_stat_t st;
    int r = vfs_stat(p, &st);
    if (r != VFS_OK) return -1;

    memset(out, 0, sizeof(*out));
    out->type = (st.type == VFS_NODE_DIR) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    out->size = (uint32_t)st.size;
    out->nlink = 1;
    out->mode = (st.type == VFS_NODE_DIR) ? VFS64_DEF_DIR_MODE : VFS64_DEF_FILE_MODE;
    const char* base = vfs64_basename(p);
    strncpy(out->name, base, sizeof(out->name) - 1);
    out->name_len = (uint32_t)strlen(out->name);
    out->kind = vfs64_kind_by_name_local(out->type, out->name);
    return 0;
}

int vfs64_stat_on64(int slot, const char* path, struct Vfs64Info64* out) {
    return (slot == 0) ? vfs64_stat64(path, out) : -1;
}
int vfs64_stat64_on64(int slot, const char* path, struct Vfs64Info64* out) {
    return (slot == 0) ? vfs64_stat64(path, out) : -1;
}

/* ==================== 读写 ==================== */

int vfs64_write64(const char* path, const void* buf, int len) {
    char p[VFS64_PATH_MAX];
    if (len < 0 || vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    int r = vfs_write_file(p, buf, (uint32_t)len);
    return (r == VFS_OK) ? len : -1;
}

int vfs64_read64(const char* path, void* buf, int max) {
    char p[VFS64_PATH_MAX];
    if (max < 0 || vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    vfs_stat_t st;
    if (vfs_stat(p, &st) != VFS_OK) return -1;

    int fd = vfs_open(p, VFS_O_RDONLY);
    if (fd < 0) return -1;

    int want = (st.size < (uint64_t)max) ? (int)st.size : max;
    int total = 0;
    while (total < want) {
        int n = vfs_read(fd, (uint8_t*)buf + total, (uint32_t)(want - total));
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);
    return total;
}

int vfs64_read_at64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    int fd = vfs_open(p, VFS_O_RDONLY);
    if (fd < 0) return -1;
    if (vfs_seek(fd, off, VFS_SEEK_SET) < 0) { vfs_close(fd); return -1; }

    uint32_t total = 0;
    while (total < len) {
        int n = vfs_read(fd, (uint8_t*)buf + total, len - total);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    vfs_close(fd);
    if (out_got) *out_got = total;
    return 0;
}

int vfs64_write_at64(const char* path, uint32_t off, const void* buf, uint32_t len) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    int fd = vfs_open(p, VFS_O_CREAT | VFS_O_RDWR);
    if (fd < 0) return -1;
    if (vfs_seek(fd, off, VFS_SEEK_SET) < 0) { vfs_close(fd); return -1; }

    uint32_t total = 0;
    while (total < len) {
        int n = vfs_write(fd, (const uint8_t*)buf + total, len - total);
        if (n <= 0) { vfs_close(fd); return -1; }
        total += (uint32_t)n;
    }

    /* If the write landed past EOF, make sure the size reflects it. */
    vfs_stat_t st;
    if (vfs_stat(p, &st) == VFS_OK && st.size < (uint64_t)(off + len)) {
        /* drivers grow size on write; this is a consistency check only */
    }
    vfs_close(fd);
    return (int)len;
}

int vfs64_write_stream64(const char* path, uint32_t len,
                         int (*src)(void*, uint32_t, void*, uint32_t), void* ctx) {
    char p[VFS64_PATH_MAX];
    if (!src || vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    int fd = vfs_open(p, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) return -1;

    uint8_t chunk[512];
    uint32_t off = 0;
    while (off < len) {
        uint32_t want = len - off;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        int got = src(ctx, off, chunk, want);
        if (got <= 0) { vfs_close(fd); return -1; }
        uint32_t woff = 0;
        while (woff < (uint32_t)got) {
            int n = vfs_write(fd, chunk + woff, (uint32_t)got - woff);
            if (n <= 0) { vfs_close(fd); return -1; }
            woff += (uint32_t)n;
        }
        off += (uint32_t)got;
    }
    vfs_close(fd);
    return 0;
}

int vfs64_read_at_on64(int slot, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) {
    return (slot == 0) ? vfs64_read_at64(path, off, buf, len, out_got) : -1;
}
int vfs64_write_at_on64(int slot, const char* path, uint32_t off, const void* buf, uint32_t len) {
    return (slot == 0) ? vfs64_write_at64(path, off, buf, len) : -1;
}
int vfs64_write_stream_on64(int slot, const char* path, uint32_t len,
                            int (*src)(void*, uint32_t, void*, uint32_t), void* ctx) {
    return (slot == 0) ? vfs64_write_stream64(path, len, src, ctx) : -1;
}

/* ==================== 目录 ==================== */

int vfs64_mkdir64(const char* path) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    return (vfs_mkdir(p, 0755) == VFS_OK) ? 0 : -1;
}

int vfs64_create64(const char* path) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    int fd = vfs_open(p, VFS_O_CREAT | VFS_O_WRONLY);
    if (fd < 0) return -1;
    vfs_close(fd);
    return 0;
}

int vfs64_unlink64(const char* path) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    return (vfs_unlink(p) == VFS_OK) ? 0 : -1;
}

int vfs64_rmdir64(const char* path) {
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    return (vfs_rmdir(p) == VFS_OK) ? 0 : -1;
}

int vfs64_rename_to64(const char* old_path, const char* new_path) {
    char o[VFS64_PATH_MAX], n[VFS64_PATH_MAX];
    if (vfs64_norm(old_path, o, sizeof(o)) < 0) return -1;
    if (vfs64_norm(new_path, n, sizeof(n)) < 0) return -1;
    return (vfs_rename(o, n) == VFS_OK) ? 0 : -1;
}

int vfs64_rename64(const char* old_path, const char* new_name) {
    char o[VFS64_PATH_MAX];
    if (vfs64_norm(old_path, o, sizeof(o)) < 0) return -1;
    if (!new_name || strchr(new_name, '/')) return -1;
    char n[VFS64_PATH_MAX];
    const char* base = vfs64_basename(o);
    size_t dirlen = (size_t)(base - o);          /* includes trailing '/' */
    if (dirlen + strlen(new_name) + 1 >= sizeof(n)) return -1;
    memcpy(n, o, dirlen);
    n[dirlen] = '\0';
    strcat(n, new_name);
    return (vfs_rename(o, n) == VFS_OK) ? 0 : -1;
}

int vfs64_set_mtime64(const char* path, uint32_t packed_time) {
    (void)packed_time;
    /* Documented no-op: VFS drivers do not persist timestamps yet. */
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    vfs_stat_t st;
    return (vfs_stat(p, &st) == VFS_OK) ? 0 : -1;
}

int vfs64_list64(const char* path, struct Vfs64Dirent64* out, int max, uint32_t* cursor) {
    char p[VFS64_PATH_MAX];
    if (!out || max <= 0 || !cursor || vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    int d = vfs_opendir(p);
    if (d < 0) return -1;

    /* skip to cursor */
    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    uint32_t idx = 0;
    while (idx < *cursor && vfs_readdir(d, name, &type) == VFS_OK) idx++;

    int n = 0;
    while (n < max && vfs_readdir(d, name, &type) == VFS_OK) {
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, name, sizeof(out[n].name) - 1);
        out[n].type = (type == VFS_NODE_DIR) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
        out[n].kind = vfs64_kind_by_name_local(out[n].type, out[n].name);
        /* fill size via stat of the child */
        char child[VFS64_PATH_MAX];
        snprintf(child, sizeof(child), "%s%s%s", p,
                 (strcmp(p, "/") == 0) ? "" : "/", name);
        vfs_stat_t st;
        if (vfs_stat(child, &st) == VFS_OK) out[n].size = (uint32_t)st.size;
        n++;
        idx++;
    }
    vfs_closedir(d);
    *cursor = idx;
    return n;
}

int vfs64_ls(const char* path, char names[][32], int max, uint32_t* sizes) {
    char p[VFS64_PATH_MAX];
    if (!names || max <= 0 || vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    int d = vfs_opendir(p);
    if (d < 0) return -1;

    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    int n = 0;
    while (n < max && vfs_readdir(d, name, &type) == VFS_OK) {
        strncpy(names[n], name, 31);
        names[n][31] = '\0';
        if (sizes) {
            char child[VFS64_PATH_MAX];
            snprintf(child, sizeof(child), "%s%s%s", p,
                     (strcmp(p, "/") == 0) ? "" : "/", name);
            vfs_stat_t st;
            sizes[n] = (vfs_stat(child, &st) == VFS_OK) ? (uint32_t)st.size : 0;
        }
        n++;
    }
    vfs_closedir(d);
    return n;
}

/* 目录流句柄 */
int vfs64_opendir64(const char* path, int* out_handle) {
    char p[VFS64_PATH_MAX];
    if (!out_handle || vfs64_norm(path, p, sizeof(p)) < 0) return -1;

    int d = vfs_opendir(p);
    if (d < 0) return -1;

    for (int i = 0; i < VFS64_DIRSTREAM_MAX; i++) {
        if (g_dirstreams[i] == 0) {
            g_dirstreams[i] = d + 1;   /* 0 = free slot */
            *out_handle = i;
            return 0;
        }
    }
    vfs_closedir(d);
    return -1;
}

int vfs64_readdir64(int handle, struct Vfs64Dirent64* out) {
    if (handle < 0 || handle >= VFS64_DIRSTREAM_MAX || !out) return -1;
    if (g_dirstreams[handle] == 0) return -1;

    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    int r = vfs_readdir(g_dirstreams[handle] - 1, name, &type);
    if (r != VFS_OK) return 0;   /* end of directory */

    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->type = (type == VFS_NODE_DIR) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    out->kind = vfs64_kind_by_name_local(out->type, out->name);
    return 1;
}

int vfs64_closedir64(int handle) {
    if (handle < 0 || handle >= VFS64_DIRSTREAM_MAX) return -1;
    if (g_dirstreams[handle] == 0) return -1;
    vfs_closedir(g_dirstreams[handle] - 1);
    g_dirstreams[handle] = 0;
    return 0;
}

/* ==================== 空间统计 ==================== */

int vfs64_free64(uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) {
    uint64_t free_b = 0, total_b = 0;
    if (vfs_get_free_space("/", &free_b, &total_b) != VFS_OK) {
        if (free_blocks) *free_blocks = 0;
        if (free_bytes) *free_bytes = 0;
        if (total_blocks) *total_blocks = 0;
        return -1;
    }
    if (free_bytes) *free_bytes = (uint32_t)free_b;
    if (free_blocks) *free_blocks = (uint32_t)(free_b / 512);
    if (total_blocks) *total_blocks = (uint32_t)(total_b / 512);
    return 0;
}

int vfs64_free_on64(int slot, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) {
    return (slot == 0) ? vfs64_free64(free_blocks, free_bytes, total_blocks) : -1;
}

static void vfs64_count_files(const char* path, uint32_t* files, int depth) {
    if (depth > 8) return;
    int d = vfs_opendir(path);
    if (d < 0) return;
    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    while (vfs_readdir(d, name, &type) == VFS_OK) {
        char child[VFS64_PATH_MAX];
        snprintf(child, sizeof(child), "%s%s%s", path,
                 (strcmp(path, "/") == 0) ? "" : "/", name);
        if (type == VFS_NODE_DIR) {
            vfs64_count_files(child, files, depth + 1);
        } else {
            (*files)++;
        }
    }
    vfs_closedir(d);
}

int vfs64_inode_stats64(uint32_t* files, uint32_t* ffree) {
    if (files) {
        *files = 0;
        vfs64_count_files("/", files, 0);
    }
    if (ffree) {
        uint64_t free_b = 0, total_b = 0;
        if (vfs_get_free_space("/", &free_b, &total_b) == VFS_OK) {
            /* ramfs capacity is measured in 64-byte file slots */
            *ffree = (uint32_t)(free_b / 64);
        } else {
            *ffree = 0;
        }
    }
    return 0;
}

int vfs64_inode_stats_on64(int slot, uint32_t* files, uint32_t* ffree) {
    return (slot == 0) ? vfs64_inode_stats64(files, ffree) : -1;
}

int vfs64_tree_dump64(const char* path, int max_entries, int max_depth) {
    (void)max_depth;
    char p[VFS64_PATH_MAX];
    if (vfs64_norm(path, p, sizeof(p)) < 0) return -1;
    int entries = 0;
    vfs64_dump_tree(p, 0, &entries,
                    (max_entries > 0) ? max_entries : 256);
    return entries;
}

int vfs64_tree_dump64_on64(int slot, const char* path, int max_entries, int max_depth) {
    return (slot == 0) ? vfs64_tree_dump64(path, max_entries, max_depth) : -1;
}

/* ==================== 按槽操作（仅 slot 0） ==================== */

int  vfs64_read_on64(int slot, const char* path, void* buf, int max) {
    return (slot == 0) ? vfs64_read64(path, buf, max) : -1;
}
int  vfs64_write_on64(int slot, const char* path, const void* buf, int len) {
    return (slot == 0) ? vfs64_write64(path, buf, len) : -1;
}
int  vfs64_mkdir_on64(int slot, const char* path) {
    return (slot == 0) ? vfs64_mkdir64(path) : -1;
}
int  vfs64_create_on64(int slot, const char* path) {
    return (slot == 0) ? vfs64_create64(path) : -1;
}
int  vfs64_unlink_on64(int slot, const char* path) {
    return (slot == 0) ? vfs64_unlink64(path) : -1;
}
int  vfs64_rmdir_on64(int slot, const char* path) {
    return (slot == 0) ? vfs64_rmdir64(path) : -1;
}
int  vfs64_ls_on64(int slot, const char* path, char names[][32], int max, uint32_t* sizes) {
    return (slot == 0) ? vfs64_ls(path, names, max, sizes) : -1;
}
int  vfs64_list64_on64(int slot, const char* path, struct Vfs64Dirent64* out, int max, uint32_t* cursor) {
    return (slot == 0) ? vfs64_list64(path, out, max, cursor) : -1;
}
int  vfs64_rename_on64(int slot, const char* old_path, const char* new_name) {
    return (slot == 0) ? vfs64_rename64(old_path, new_name) : -1;
}

/* ==================== 兼容 API ==================== */

int  vfs64_read(const char* path, void* buf, int max) { return vfs64_read64(path, buf, max); }
int  vfs64_write(const char* path, const void* buf, int len) { return vfs64_write64(path, buf, len); }
int  vfs64_unlink(const char* path) { return vfs64_unlink64(path); }
int  vfs64_mkdir(const char* path) { return vfs64_mkdir64(path); }

int vfs64_stat(const char* path, uint32_t* type, uint32_t* size) {
    struct Vfs64Info64 info;
    if (vfs64_stat64(path, &info) < 0) return -1;
    if (type) *type = info.type;
    if (size) *size = info.size;
    return 0;
}

/* ==================== 自检（真实测试） ==================== */

int vfs64_selftest64(void) {
    bool pass = true;
    static const char payload[] = "vfs64 selftest payload 0123456789";
    char rdbuf[64];

    /* create + write */
    if (vfs64_write64("/vfs64test.txt", payload, sizeof(payload)) != (int)sizeof(payload)) {
        console_puts("[vfs64] selftest: write failed\n");
        pass = false;
    }
    /* stat */
    struct Vfs64Info64 info;
    if (vfs64_stat64("/vfs64test.txt", &info) < 0 || info.size != sizeof(payload)) {
        console_puts("[vfs64] selftest: stat failed\n");
        pass = false;
    }
    /* read back + compare */
    int got = vfs64_read64("/vfs64test.txt", rdbuf, sizeof(rdbuf));
    if (got != (int)sizeof(payload) || memcmp(rdbuf, payload, sizeof(payload)) != 0) {
        console_puts("[vfs64] selftest: read-back mismatch\n");
        pass = false;
    }
    /* ls must contain it */
    char names[8][32];
    int n = vfs64_ls("/", names, 8, NULL);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "vfs64test.txt") == 0) found = true;
    }
    if (!found) {
        console_puts("[vfs64] selftest: ls did not list the file\n");
        pass = false;
    }
    /* unlink + verify gone */
    if (vfs64_unlink64("/vfs64test.txt") < 0 ||
        vfs64_stat64("/vfs64test.txt", &info) == 0) {
        console_puts("[vfs64] selftest: unlink failed\n");
        pass = false;
    }

    st_run("vfs64", pass);
    return pass ? 0 : -1;
}

void vfs64_dump64(void) {
    int entries = 0;
    console_puts("[vfs64] / (unified VFS namespace):\n");
    vfs64_dump_tree("/", 1, &entries, 64);
    vfs64_slots_dump64();
}

/* ==================== 时间/类型判定工具（真实现） ==================== */

uint32_t vfs64_pack_time64(int year, int month, int day, int hour, int minute, int second) {
    return (((uint32_t)(year - 1980) & 0x7F) << 25) |
           (((uint32_t)month & 0x0F) << 21) |
           (((uint32_t)day & 0x1F) << 16) |
           (((uint32_t)hour & 0x1F) << 11) |
           (((uint32_t)minute & 0x3F) << 5) |
           (((uint32_t)second / 2) & 0x1F);
}

void vfs64_unpack_time64(uint32_t packed, struct Vfs64Time64* out) {
    if (!out) return;
    out->year   = (uint16_t)(1980 + ((packed >> 25) & 0x7F));
    out->month  = (uint8_t)((packed >> 21) & 0x0F);
    out->day    = (uint8_t)((packed >> 16) & 0x1F);
    out->hour   = (uint8_t)((packed >> 11) & 0x1F);
    out->minute = (uint8_t)((packed >> 5) & 0x3F);
    out->second = (uint8_t)((packed & 0x1F) * 2);
}

uint32_t vfs64_now64(void) {
    /* No RTC plumbing here yet; boot time = 00:00:00 2020-01-01 */
    return vfs64_pack_time64(2020, 1, 1, 0, 0, 0);
}

uint32_t vfs64_kind_of_data64(const void* data, uint32_t len, const char* name, uint32_t name_len) {
    (void)name; (void)name_len;
    const uint8_t* d = (const uint8_t*)data;
    if (d && len >= 4 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') {
        return VFS64_KIND_ELF;
    }
    return VFS64_KIND_BIN;
}

uint32_t vfs64_kind_by_name64(uint32_t type, const char* name, uint32_t name_len) {
    (void)name_len;
    return vfs64_kind_by_name_local(type, name);
}

const char* vfs64_kind_str64(uint32_t kind) {
    switch (kind) {
        case VFS64_KIND_NONE: return "none";
        case VFS64_KIND_FILE: return "file";
        case VFS64_KIND_DIR:  return "dir";
        case VFS64_KIND_VAP:  return "vap";
        case VFS64_KIND_ELF:  return "elf";
        case VFS64_KIND_TEXT: return "text";
        case VFS64_KIND_BIN:  return "bin";
        default:              return "?";
    }
}
