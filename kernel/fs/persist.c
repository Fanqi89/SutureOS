/* SutureOS: persistent user data on the installed system volume
 *
 * Why: after `install`, /etc lives in the ramfs (volatile) and only five files
 * were seeded from the FAT32 system volume at boot, so anything the user changed
 * at runtime - accounts added with `useradd`, /etc/motd, /home files - vanished
 * on reboot. This module keeps the user-data mirror on the installed volume and
 * moves it both ways:
 *
 *   boot   persist_init()   PERSIST/{etc,home,root} -> ramfs /etc,/home,/root
 *   flush  persist_flush()  ramfs /etc,/home,/root  -> PERSIST/{etc,home,root}
 *
 * Layout on the FAT32 system volume (created by the installer, format v1):
 *
 *   /SUTUREOS/PERSIST/VERSION        "sutureos-persist 1"
 *   /SUTUREOS/PERSIST/etc/           passwd, shadow, group, sudoers, motd, ...
 *   /SUTUREOS/PERSIST/home/<user>/   the users' home trees
 *   /SUTUREOS/PERSIST/root/          root's files
 *
 * Everything goes through the normal VFS + FAT32 write path (real blk_write),
 * and persist_flush() finishes with vfs_sync("/mnt/sys") so a shutdown right
 * after it cannot lose data. The ramfs stays the root filesystem - this is a
 * mirror, not a switch of root - which keeps the boot presentation and the GUI
 * untouched.
 *
 * Scope / honest limits: the mirror is a *copy*, so deleting a file in the ramfs
 * does not delete it from the volume (the next boot would bring it back); it is
 * bounded (see PERSIST_MAX_*), depth-limited (PERSIST_MAX_DEPTH) and only runs
 * when a system volume is present, so a live-ISO session without an installed
 * disk never writes anything.
 */
#include "persist.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "vfs.h"
#include "heap.h"

#define PERSIST_SYS          "/mnt/sys"
#define PERSIST_ROOT         "/mnt/sys/SUTUREOS/PERSIST"
#define PERSIST_VERSION_FILE PERSIST_ROOT "/VERSION"
#define PERSIST_VERSION_TEXT "sutureos-persist 1\n"

#define PERSIST_MAX_DEPTH    6
#define PERSIST_MAX_ENTRIES  64        /* per directory */
#define PERSIST_MAX_FILES    256       /* per flush / restore */
#define PERSIST_MAX_FILE     (128u * 1024u)   /* ramfs allows 256 KiB; be safe */
#define PERSIST_CHUNK        4096u

static int g_present;                 /* 1 = a writable system volume is mounted */
static int g_restored;                /* files restored at boot */
static int g_flushed;                 /* files written by the last flush */
static int g_flush_count;             /* flush calls */
static int g_last_error;              /* first error of the last flush, 0 = none */

static uint8_t g_buf[PERSIST_CHUNK];

/* ---------------- helpers ---------------- */

static int persist_dir_exists(const char *path) {
    vfs_stat_t st;
    return vfs_stat(path, &st) == VFS_OK && st.type == VFS_NODE_DIR;
}

static int persist_mkdirs(const char *path) {
    return vfs_mkdir_p(path) == VFS_OK ? 0 : -1;
}

/* Copy one file through the VFS. Returns 0 on success. */
static int persist_copy(const char *src, const char *dst) {
    int in = vfs_open(src, VFS_O_RDONLY);
    int out;
    int r = 0;

    if (in < 0) return -1;
    out = vfs_open(dst, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (out < 0) {
        vfs_close(in);
        return -1;
    }
    for (;;) {
        int n = vfs_read(in, g_buf, PERSIST_CHUNK);
        int off = 0;
        if (n < 0) { r = -1; break; }
        if (n == 0) break;
        while (off < n) {
            int w = vfs_write(out, g_buf + off, (uint32_t)(n - off));
            if (w <= 0) { r = -1; break; }
            off += w;
        }
        if (r != 0) break;
    }
    vfs_close(in);
    vfs_close(out);
    return r;
}

/* Recursively mirror src_dir into dst_dir (dst must exist). mode 0 = restore
 * (volume -> ramfs), mode 1 = flush (ramfs -> volume). Counts files. */
static int persist_mirror(const char *src_dir, const char *dst_dir, int mode,
                          int depth, int *counter) {
    char name[VFS_MAX_NAME];
    char src[192];
    char dst[192];
    int fd;
    int r = 0;
    int entries = 0;

    if (depth > PERSIST_MAX_DEPTH) return 0;
    fd = vfs_opendir(src_dir);
    if (fd < 0) return 0;                 /* nothing to mirror */
    for (;;) {
        vfs_node_type_t type = VFS_NODE_FILE;

        (void)mode;                       /* both directions copy src -> dst */
        if (entries++ >= PERSIST_MAX_ENTRIES) break;
        if (vfs_readdir(fd, name, &type) != VFS_OK) break;
        snprintf(src, sizeof(src), "%s/%s", src_dir, name);
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
        if (type == VFS_NODE_DIR) {
            if (persist_mkdirs(dst) != 0) { r = -1; continue; }
            if (persist_mirror(src, dst, mode, depth + 1, counter) != 0) r = -1;
        } else if (type == VFS_NODE_FILE) {
            vfs_stat_t st;
            if (vfs_stat(src, &st) != VFS_OK) continue;
            if (st.size > PERSIST_MAX_FILE) continue;    /* too big for the mirror */
            if (persist_copy(src, dst) != 0) { r = -1; continue; }
            if (counter) (*counter)++;
            if (counter && *counter >= PERSIST_MAX_FILES) break;
        }
    }
    vfs_closedir(fd);
    return r;
}

/* Make sure the persistence skeleton exists on the volume. */
static int persist_prepare_volume(void) {
    if (persist_mkdirs(PERSIST_ROOT) != 0) return -1;
    if (persist_mkdirs(PERSIST_ROOT "/etc") != 0) return -1;
    if (persist_mkdirs(PERSIST_ROOT "/home") != 0) return -1;
    if (persist_mkdirs(PERSIST_ROOT "/root") != 0) return -1;
    if (!persist_dir_exists(PERSIST_VERSION_FILE)) {
        if (vfs_write_file(PERSIST_VERSION_FILE, PERSIST_VERSION_TEXT,
                           (uint32_t)strlen(PERSIST_VERSION_TEXT)) != VFS_OK) {
            return -1;
        }
    }
    return 0;
}

static void persist_mkdir_ramfs(const char *path) {
    if (!persist_dir_exists(path)) (void)vfs_mkdir(path, 0755);
}

/* ---------------- public API ---------------- */

int persist_detect(void) {
    g_present = 0;
    if (!persist_dir_exists(PERSIST_SYS)) return 0;
    if (!persist_dir_exists(PERSIST_SYS "/SUTUREOS")) return 0;
    /* The PERSIST directory may not exist yet on a freshly installed disk: the
     * installer writes it, but an older system volume only has /etc. Treat the
     * installed marker + a writable mount as "persistence available" and create
     * the skeleton on the first flush. */
    g_present = 1;
    return 1;
}

int persist_present(void) { return g_present; }

int persist_format_version(void) {
    size_t len = 0;
    char *data;
    int version = 0;
    if (!g_present) return 0;
    data = vfs_read_file(PERSIST_VERSION_FILE, &len);
    if (!data || len == 0) {
        if (data) lemis_heap_free(data);
        return 0;
    }
    /* "sutureos-persist <n>" */
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= '0' && data[i] <= '9') version = version * 10 + (data[i] - '0');
    }
    lemis_heap_free(data);
    return version;
}

int persist_restore(void) {
    int files = 0;

    g_restored = 0;
    if (!g_present) return 0;

    /* /etc first: accounts must be in place before userdb_init() re-parses. */
    persist_mkdir_ramfs("/etc");
    if (persist_dir_exists(PERSIST_ROOT "/etc")) {
        (void)persist_mirror(PERSIST_ROOT "/etc", "/etc", 0, 0, &files);
    }
    /* /home and /root: dirs plus files. */
    persist_mkdir_ramfs("/home");
    persist_mkdir_ramfs("/root");
    if (persist_dir_exists(PERSIST_ROOT "/home")) {
        (void)persist_mirror(PERSIST_ROOT "/home", "/home", 0, 0, &files);
    }
    if (persist_dir_exists(PERSIST_ROOT "/root")) {
        (void)persist_mirror(PERSIST_ROOT "/root", "/root", 0, 0, &files);
    }
    g_restored = files;
    console_printf("[persist] restored %d file(s) from %s\n",
                   (int64_t)files, PERSIST_ROOT);
    return files;
}

int persist_flush(void) {
    int files = 0;
    int err = 0;

    g_last_error = 0;
    if (!g_present)
        return 0;                     /* no installed system volume: nothing to do */
    if (persist_prepare_volume() != 0) {
        g_last_error = 1;
        return -1;
    }
    if (persist_mirror("/etc", PERSIST_ROOT "/etc", 1, 0, &files) != 0) err = 1;
    if (persist_mirror("/home", PERSIST_ROOT "/home", 1, 0, &files) != 0) err = 1;
    if (persist_mirror("/root", PERSIST_ROOT "/root", 1, 0, &files) != 0) err = 1;
    /* The FAT32 driver keeps its FAT/FSInfo updates in memory: sync now, or a
     * shutdown right after this call loses the writes. */
    (void)vfs_sync(PERSIST_SYS);
    g_flushed = files;
    g_flush_count++;
    g_last_error = err;
    if (err)
        console_printf("[persist] flush FAILED after %d file(s)\n", (int64_t)files);
    else
        console_printf("[persist] flush %d files\n", (int64_t)files);
    return err ? -1 : files;
}

int persist_flushed_files(void) { return g_flushed; }
int persist_restored_files(void) { return g_restored; }
int persist_flush_count(void) { return g_flush_count; }
int persist_last_error(void) { return g_last_error; }
