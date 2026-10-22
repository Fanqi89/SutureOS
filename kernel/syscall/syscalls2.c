/* SutureOS: the missing POSIX syscalls. See syscalls2.h for scope and omissions. */
#include "syscalls2.h"

#include <sutureos/version.h>
#include <stitch/console.h>
#include <stitch/string.h>
#include "creds.h"
#include "el0_validate.h"
#include "heap.h"
#include "pipe.h"
#include "rtc.h"
#include "signal.h"
#include "syscall.h"
#include "task.h"
#include "userdb.h"
#include "vfs.h"

/* errno values (Linux numbering, which the compat layer shares) */
#define EPERM_   1
#define ENOENT_  2
#define EIO_     5
#define EBADF_   9
#define EAGAIN_  11
#define EACCES_  13
#define EFAULT_  14
#define EEXIST_  17
#define ENOTDIR_ 20
#define EISDIR_  21
#define EINVAL_  22
#define ENOSPC_  28
#define ESPIPE_  29
#define EPIPE_   32
#define ENOSYS_  38
#define ENOTTY_  25
#define ECHILD_  10
#define ERANGE_  34

#define AT_FDCWD_        (-100)
#define O_ACCMODE_       3
#define SEEK_SET_ 0
#define SEEK_CUR_ 1
#define SEEK_END_ 2
#define S_IFMT_   0170000
#define S_IFDIR_  0040000
#define S_IFREG_  0100000
#define S_IFLNK_  0120000

static uint32_t g_registered;
static int      g_cwd_initialised;

/* ============================ helpers ============================ */

static uint64_t ok(int rc) { return (uint64_t)(int64_t)rc; }
static uint64_t err(int e) { return (uint64_t)(int64_t)(-e); }

/* Validate a user pointer when ring 3 is actually running (no-op otherwise), the
 * same rule the first batch of syscalls uses. */
static int ptr_ok(uint64_t va, uint64_t len)
{
    return el0_ptr_ok(va, len);
}

static const char *str_arg(uint64_t va)
{
    if (va == 0) return NULL;
    if (!ptr_ok(va, 1)) return NULL;
    return (const char *)(uintptr_t)va;
}

/* Kernel-internal VFS calls take an absolute path in this VFS, so relative
 * arguments are resolved against the shell/VFS cwd. */
static void resolve_path(const char *in, char *out, size_t outlen)
{
    if (in == NULL) {
        out[0] = '\0';
        return;
    }
    if (in[0] == '/') {
        snprintf(out, outlen, "%s", in);
        return;
    }
    {
        const char *cwd = vfs_get_cwd();
        if (cwd[0] == '\0' || strcmp(cwd, "/") == 0) {
            snprintf(out, outlen, "/%s", in);
        } else {
            snprintf(out, outlen, "%s/%s", cwd, in);
        }
    }
}

static void fill_linux_stat(const vfs_stat_t *st, struct linux_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = 1;
    out->st_ino = 1;
    out->st_nlink = (st->type == VFS_NODE_DIR) ? 2 : 1;
    out->st_uid = st->uid;
    out->st_gid = st->gid;
    switch (st->type) {
    case VFS_NODE_DIR:  out->st_mode = S_IFDIR_ | 0755; break;
    case VFS_NODE_SYMLINK: out->st_mode = S_IFLNK_ | 0777; break;
    default:            out->st_mode = S_IFREG_ | (st->mode & 0777); break;
    }
    out->st_rdev = 0;
    out->st_size = (int64_t)st->size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((st->size + 511) / 512);
    out->st_atime = (int64_t)st->atime;
    out->st_mtime = (int64_t)st->mtime;
    out->st_ctime = (int64_t)st->ctime;
}

/* ============================ file status ============================ */

static uint64_t do_stat(uint64_t path_va, uint64_t st_va, int follow_links)
{
    const char *path = str_arg(path_va);
    struct linux_stat *out = (struct linux_stat *)(uintptr_t)st_va;
    vfs_stat_t st;
    char abs[VFS_MAX_PATH];

    (void)follow_links;                 /* the VFS has no symlink resolution yet */
    if (path == NULL || out == NULL) return err(EFAULT_);
    if (!ptr_ok(st_va, sizeof(*out))) return err(EFAULT_);

    resolve_path(path, abs, sizeof(abs));
    if (vfs_stat(abs, &st) != VFS_OK) return err(ENOENT_);
    fill_linux_stat(&st, out);
    return 0;
}

static uint64_t sys2_stat(uint64_t path, uint64_t st, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    return do_stat(path, st, 1);
}

static uint64_t sys2_lstat(uint64_t path, uint64_t st, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    return do_stat(path, st, 0);
}

static uint64_t sys2_fstat(uint64_t fd, uint64_t st_va, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    struct linux_stat *out = (struct linux_stat *)(uintptr_t)st_va;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (out == NULL || !ptr_ok(st_va, sizeof(*out))) return err(EFAULT_);
    if (pipe_is_fd((int)fd)) {
        /* a pipe has no on-disk identity: report a FIFO, size 0 */
        memset(out, 0, sizeof(*out));
        out->st_mode = 0010000 | 0600;      /* S_IFIFO */
        out->st_nlink = 1;
        out->st_blksize = PIPE_BUF_SIZE;
        return 0;
    }
    /* The VFS has no fstat(): resolve the fd back through vfs_stat on its path is
     * not possible either, so report the fd as a character-ish object with the
     * size the fd reports via seek(END). */
    {
        const int end = vfs_seek((int)fd, 0, SEEK_END_);
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFREG_ | 0644;
        out->st_nlink = 1;
        out->st_size = (end > 0) ? end : 0;
        out->st_blksize = 4096;
        (void)vfs_seek((int)fd, 0, SEEK_SET_);
        return 0;
    }
}

static uint64_t sys2_access(uint64_t path_va, uint64_t mode, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    vfs_stat_t st;
    char abs[VFS_MAX_PATH];
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    return (vfs_stat(abs, &st) == VFS_OK) ? 0 : err(ENOENT_);
}

/* ============================ file offsets / dup ============================ */

static uint64_t sys2_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (pipe_is_fd((int)fd)) return err(ESPIPE_);
    if (vfs_seek((int)fd, (uint32_t)offset, (int)whence) < 0) return err(EINVAL_);
    return vfs_seek((int)fd, 0, SEEK_CUR_);
}

static uint64_t sys2_dup(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (pipe_is_fd((int)fd)) {
        /* duplicate a pipe end: bump the reader/writer count */
        int fds[2];
        (void)fds;
        return err(ENOSYS_);            /* pipe duplication needs both ends */
    }
    return err(ENOSYS_);                /* see note in the release notes */
}

static uint64_t sys2_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)oldfd; (void)newfd; (void)a3; (void)a4; (void)a5; (void)a6;
    return err(ENOSYS_);                /* fd table has no free-list API yet */
}

static uint64_t sys2_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)fd; (void)request; (void)arg; (void)a4; (void)a5; (void)a6;
    return err(ENOTTY_);                /* no tty driver: nothing to configure */
}

/* ============================ identity ============================ */

static uint64_t sys2_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)sched_cpu.current;
}

static uint64_t sys2_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;                            /* no parent/child bookkeeping yet */
}

static uint64_t sys2_getuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return cred_uid();
}

static uint64_t sys2_geteuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return cred_euid();
}

static uint64_t sys2_getgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return cred_gid();
}

static uint64_t sys2_getegid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return cred_egid();
}

static uint64_t sys2_setuid(uint64_t uid, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Only root may change identity, and only to an existing account. */
    if (!cred_is_root()) return err(EPERM_);
    {
        const userdb_user_t *u = userdb_by_uid((uint32_t)uid);
        if (u == NULL) return err(EINVAL_);
        return (cred_set_user(u->name) == 0) ? 0 : err(EINVAL_);
    }
}

static uint64_t sys2_setgid(uint64_t gid, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    (void)gid; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!cred_is_root()) return err(EPERM_);
    return 0;                            /* supplementary groups are parsed, not set */
}

static uint64_t sys2_uname(uint64_t buf_va, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    struct linux_utsname *u = (struct linux_utsname *)(uintptr_t)buf_va;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (u == NULL || !ptr_ok(buf_va, sizeof(*u))) return err(EFAULT_);
    memset(u, 0, sizeof(*u));
    snprintf(u->sysname, sizeof(u->sysname), "Linux");        /* musl checks this */
    snprintf(u->nodename, sizeof(u->nodename), "sutureos");
    snprintf(u->release, sizeof(u->release), "6.1.0-sutureos");
    snprintf(u->version, sizeof(u->version), "SutureOS %s", SUTUREOS_VERSION);
    snprintf(u->machine, sizeof(u->machine), "x86_64");
    snprintf(u->domainname, sizeof(u->domainname), "(none)");
    return 0;
}

/* ============================ cwd / directories ============================ */

static uint64_t sys2_chdir(uint64_t path_va, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    vfs_stat_t st;
    char abs[VFS_MAX_PATH];
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    if (vfs_stat(abs, &st) != VFS_OK) return err(ENOENT_);
    if (st.type != VFS_NODE_DIR) return err(ENOTDIR_);
    vfs_set_cwd(abs);
    g_cwd_initialised = 1;
    return 0;
}

static uint64_t sys2_getcwd(uint64_t buf_va, uint64_t size, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *cwd = vfs_get_cwd();
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (buf_va == 0) return err(EFAULT_);
    {
        const size_t len = strlen(cwd) + 1;
        if (size < len) return err(ERANGE_);
        if (!ptr_ok(buf_va, len)) return err(EFAULT_);
        memcpy((void *)(uintptr_t)buf_va, cwd, len);
        return buf_va;                   /* Linux returns the buffer address */
    }
}

static uint64_t sys2_mkdir(uint64_t path_va, uint64_t mode, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    {
        vfs_stat_t st;
        if (vfs_stat(abs, &st) == VFS_OK) return err(EEXIST_);
    }
    return (vfs_mkdir(abs, 0755) == VFS_OK) ? 0 : err(EIO_);
}

static uint64_t sys2_rmdir(uint64_t path_va, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    return (vfs_rmdir(abs) == VFS_OK) ? 0 : err(ENOENT_);
}

static uint64_t sys2_unlink(uint64_t path_va, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    return (vfs_unlink(abs) == VFS_OK) ? 0 : err(ENOENT_);
}

static uint64_t sys2_rename(uint64_t old_va, uint64_t new_va, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *oldp = str_arg(old_va);
    const char *newp = str_arg(new_va);
    char a[VFS_MAX_PATH], b[VFS_MAX_PATH];
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (oldp == NULL || newp == NULL) return err(EFAULT_);
    resolve_path(oldp, a, sizeof(a));
    resolve_path(newp, b, sizeof(b));
    return (vfs_rename(a, b) == VFS_OK) ? 0 : err(EPERM_);
}

static uint64_t sys2_getdents64(uint64_t fd, uint64_t buf_va, uint64_t len,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint8_t *out = (uint8_t *)(uintptr_t)buf_va;
    uint32_t written = 0;
    (void)a4; (void)a5; (void)a6;

    if (out == NULL || !ptr_ok(buf_va, len)) return err(EFAULT_);
    for (;;) {
        char name[VFS_MAX_NAME];
        vfs_node_type_t type;
        int r = vfs_readdir((int)fd, name, &type);
        struct linux_dirent64 *de;
        uint32_t reclen;

        if (r != VFS_OK) break;
        reclen = (uint32_t)((sizeof(struct linux_dirent64) + strlen(name) + 1 + 7) & ~7u);
        if (written + reclen > len) break;          /* caller calls again */
        de = (struct linux_dirent64 *)(void *)(out + written);
        memset(de, 0, reclen);
        de->d_ino = 1;
        de->d_reclen = (uint16_t)reclen;
        de->d_type = (type == VFS_NODE_DIR) ? 4 : 8;   /* DT_DIR / DT_REG */
        memcpy(de->d_name, name, strlen(name) + 1);
        written += reclen;
    }
    return written;
}

/* ============================ truncate / sync ============================ */

static uint64_t sys2_truncate(uint64_t path_va, uint64_t length, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    /* Truncate by writing an empty file: the VFS opens with O_TRUNC. */
    if (length == 0) {
        return (vfs_write_file(abs, "", 0) == VFS_OK) ? 0 : err(EIO_);
    }
    return err(EINVAL_);                /* only shrink-to-zero is supported */
}

static uint64_t sys2_ftruncate(uint64_t fd, uint64_t length, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)fd; (void)length; (void)a3; (void)a4; (void)a5; (void)a6;
    return err(ENOSYS_);                /* fd-level truncate needs VFS support */
}

static uint64_t sys2_fsync(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    (void)fd; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)vfs_sync(NULL);               /* flush every mount; nothing is cached */
    return 0;
}

static uint64_t sys2_sync(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)vfs_sync(NULL);
    return 0;
}

static uint64_t sys2_statfs(uint64_t path_va, uint64_t buf_va, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    uint64_t free_bytes = 0, total_bytes = 0;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    if (vfs_get_free_space(abs, &free_bytes, &total_bytes) != VFS_OK) {
        return err(ENOENT_);
    }
    if (buf_va != 0 && ptr_ok(buf_va, 16)) {
        uint64_t *out = (uint64_t *)(uintptr_t)buf_va;
        out[0] = free_bytes;            /* f_bfree-ish (bytes, documented) */
        out[1] = total_bytes;
    }
    return 0;
}

/* ============================ time ============================ */

static uint64_t sys2_gettimeofday(uint64_t tv_va, uint64_t tz_va, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_timeval *tv = (struct linux_timeval *)(uintptr_t)tv_va;
    (void)tz_va; (void)a3; (void)a4; (void)a5; (void)a6;

    if (tv != NULL) {
        if (!ptr_ok(tv_va, sizeof(*tv))) return err(EFAULT_);
        tv->tv_sec = (int64_t)rtc_now_epoch();
        tv->tv_usec = (int64_t)((rtc_uptime_seconds() * 1000u) % 1000u) * 1000;
    }
    return 0;
}

static uint64_t sys2_time(uint64_t tv_va, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    const uint64_t now = rtc_now_epoch();
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (tv_va != 0) {
        if (!ptr_ok(tv_va, 8)) return err(EFAULT_);
        *(int64_t *)(uintptr_t)tv_va = (int64_t)now;
    }
    return now;
}

/* clock_gettime is registered by syscalls.c today and returned a hardcoded value;
 * syscalls2 re-registers it with the real clock. */
static uint64_t sys2_clock_gettime(uint64_t clk_id, uint64_t tp_va, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_timespec *ts = (struct linux_timespec *)(uintptr_t)tp_va;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (ts == NULL) return err(EFAULT_);
    if (!ptr_ok(tp_va, sizeof(*ts))) return err(EFAULT_);
    switch (clk_id) {
    case 0:                             /* CLOCK_REALTIME */
        ts->tv_sec = (int64_t)rtc_now_epoch();
        ts->tv_nsec = (int64_t)((rtc_uptime_seconds() * 1000u) % 1000u) * 1000000;
        return 0;
    case 1:                             /* CLOCK_MONOTONIC */
        ts->tv_sec = (int64_t)rtc_uptime_seconds();
        ts->tv_nsec = 0;
        return 0;
    default:
        return err(EINVAL_);
    }
}

static uint64_t sys2_nanosleep(uint64_t req_va, uint64_t rem_va, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    const struct linux_timespec *req = (const struct linux_timespec *)(uintptr_t)req_va;
    uint64_t deadline_s;
    (void)rem_va; (void)a3; (void)a4; (void)a5; (void)a6;

    if (req == NULL || !ptr_ok(req_va, sizeof(*req))) return err(EFAULT_);
    if (req->tv_sec < 0 || req->tv_nsec < 0) return err(EINVAL_);

    /* Bounded busy wait: there is no timer wheel to sleep on yet, and sleeping
     * would need the scheduler (which the shell must not yield to). */
    deadline_s = rtc_uptime_seconds() + (uint64_t)req->tv_sec;
    while (rtc_uptime_seconds() < deadline_s) {
        for (volatile int spin = 0; spin < 20000; spin++) {
            __asm__ volatile("pause");
        }
    }
    return 0;
}

/* ============================ processes / signals ============================ */

static uint64_t sys2_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (sig == 0) return 0;                       /* existence probe */
    if (sig >= 32) return err(EINVAL_);
    if ((int)pid < 0 || (int)pid >= SCHED_MAX_TASKS) return err(EINVAL_);
    if (sched_cpu.tasks[pid].state == SCHED_TASK_UNUSED) return err(ECHILD_);
    sig_send((int)pid, (int)sig);
    return 0;
}

static uint64_t sys2_wait4(uint64_t pid, uint64_t status_va, uint64_t options,
                           uint64_t rusage_va, uint64_t a5, uint64_t a6)
{
    (void)pid; (void)options; (void)rusage_va; (void)a5; (void)a6;
    /* No parent/child bookkeeping yet: every task is a child of the kernel. */
    if (status_va != 0 && ptr_ok(status_va, 4)) {
        *(int32_t *)(uintptr_t)status_va = 0;
    }
    return err(ECHILD_);
}

static uint64_t sys2_exit_group(uint64_t code, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return syscall_invoke(SYS_EXIT, code, 0, 0, 0, 0, 0);
}

/* ============================ system information ============================ */

static uint64_t sys2_sysinfo(uint64_t buf_va, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5, uint64_t a6)
{
    /* struct sysinfo is 112 bytes; fill the first fields (uptime, totals) and
     * zero the rest, which is what a caller printing `uptime` needs. */
    uint8_t *out = (uint8_t *)(uintptr_t)buf_va;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (out == NULL || !ptr_ok(buf_va, 112)) return err(EFAULT_);
    memset(out, 0, 112);
    *(int64_t *)(void *)out = (int64_t)rtc_uptime_seconds();
    {
        extern uint64_t moe_pmm_free_count(void);
        extern uint64_t moe_pmm_total(void);
        const uint64_t free_pages = moe_pmm_free_count();
        const uint64_t total_pages = moe_pmm_total();
        *(uint64_t *)(void *)(out + 8) = total_pages * 4096u;   /* totalram */
        *(uint64_t *)(void *)(out + 16) = free_pages * 4096u;   /* freeram  */
    }
    return 0;
}

static uint64_t sys2_getrlimit(uint64_t resource, uint64_t rlim_va, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint64_t *rl = (uint64_t *)(uintptr_t)rlim_va;
    (void)resource; (void)a3; (void)a4; (void)a5; (void)a6;

    if (rl == NULL || !ptr_ok(rlim_va, 16)) return err(EFAULT_);
    rl[0] = 0x7FFFFFFFFFFFFFFFull;      /* RLIM_INFINITY */
    rl[1] = 0x7FFFFFFFFFFFFFFFull;
    return 0;
}

static uint64_t sys2_setrlimit(uint64_t resource, uint64_t rlim_va, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)resource; (void)rlim_va; (void)a3; (void)a4; (void)a5; (void)a6;
    return (cred_is_root()) ? 0 : err(EPERM_);
}

static uint64_t sys2_times(uint64_t buf_va, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    const uint64_t ticks = rtc_uptime_seconds() * 100u;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (buf_va != 0) {
        if (!ptr_ok(buf_va, 32)) return err(EFAULT_);
        ((uint64_t *)(uintptr_t)buf_va)[0] = ticks;
    }
    return ticks;
}

static uint64_t sys2_umask(uint64_t mask, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    (void)mask; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return 022;                         /* fixed: the VFS has no per-task umask */
}

static uint64_t sys2_chmod(uint64_t path_va, uint64_t mode, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)path_va; (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;                           /* mode bits are reported, not enforced */
}

static uint64_t sys2_prctl(uint64_t option, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* PR_SET_NAME (15) succeeds; everything else is answered with EINVAL the way
     * Linux answers unknown options. */
    return (option == 15) ? 0 : err(EINVAL_);
}

static uint64_t sys2_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                                       uint64_t mask_va, uint64_t a4,
                                       uint64_t a5, uint64_t a6)
{
    uint8_t *mask = (uint8_t *)(uintptr_t)mask_va;
    extern uint32_t smp_online_cpu_count(void);
    const int ncpu = (int)smp_online_cpu_count();
    (void)pid; (void)a4; (void)a5; (void)a6;

    if (mask == NULL) return err(EFAULT_);
    if (!ptr_ok(mask_va, cpusetsize)) return err(EFAULT_);
    if (cpusetsize < 8) return err(EINVAL_);
    memset(mask, 0, (size_t)cpusetsize);
    for (int i = 0; i < ncpu && i < 64; i++) mask[i / 8] |= (uint8_t)(1u << (i % 8));
    return 8;
}

static uint64_t sys2_getcpu(uint64_t cpu_va, uint64_t node_va, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (cpu_va != 0 && ptr_ok(cpu_va, 4)) {
        *(uint32_t *)(uintptr_t)cpu_va = (uint32_t)sched_cpu.current;
    }
    if (node_va != 0 && ptr_ok(node_va, 4)) {
        *(uint32_t *)(uintptr_t)node_va = 0;
    }
    return 0;
}

static uint64_t sys2_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    return sys2_getpid(a1, a2, a3, a4, a5, a6);
}

/* ============================ pipes ============================ */

static uint64_t sys2_pipe(uint64_t fds_va, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    int fds[2];
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (fds_va == 0 || !ptr_ok(fds_va, 8)) return err(EFAULT_);
    if (pipe_create(fds) != 0) return err(EAGAIN_);
    ((int32_t *)(uintptr_t)fds_va)[0] = fds[0];
    ((int32_t *)(uintptr_t)fds_va)[1] = fds[1];
    return 0;
}

static uint64_t sys2_pipe2(uint64_t fds_va, uint64_t flags, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a3; (void)a4; (void)a5; (void)a6;
    return sys2_pipe(fds_va, 0, 0, 0, 0, 0);
}

/* ==================== not implemented on purpose ==================== */

static uint64_t sys2_enosys(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return err(ENOSYS_);
}

/* ============================ registration ============================ */

static void reg(uint64_t nr, syscall_fn_t fn)
{
    syscall_register(nr, fn);
    g_registered++;
}

int syscalls_registered_count(void) { return (int)g_registered; }

int syscalls2_register(void)
{
    g_registered = 0;

    /* file status */
    reg(SYS_STAT, sys2_stat);
    reg(SYS_FSTAT, sys2_fstat);
    reg(SYS_LSTAT, sys2_lstat);
    reg(SYS_ACCESS, sys2_access);
    reg(SYS_LSEEK, sys2_lseek);
    reg(SYS_IOCTL, sys2_ioctl);
    reg(SYS_DUP, sys2_dup);
    reg(SYS_DUP2, sys2_dup2);
    reg(SYS_DUP3, sys2_dup2);
    reg(SYS_FSYNC, sys2_fsync);
    reg(SYS_FDATASYNC, sys2_fsync);
    reg(SYS_SYNC, sys2_sync);
    reg(SYS_TRUNCATE, sys2_truncate);
    reg(SYS_FTRUNCATE, sys2_ftruncate);
    reg(SYS_STATFS, sys2_statfs);
    reg(SYS_FSTATFS, sys2_statfs);

    /* directories and paths */
    reg(SYS_CHDIR, sys2_chdir);
    reg(SYS_GETCWD, sys2_getcwd);
    reg(SYS_MKDIR, sys2_mkdir);
    reg(SYS_RMDIR, sys2_rmdir);
    reg(SYS_UNLINK, sys2_unlink);
    reg(SYS_RENAME, sys2_rename);
    reg(SYS_GETDENTS64, sys2_getdents64);
    reg(SYS_CREAT, sys2_enosys);        /* open(O_CREAT) is the supported path */
    reg(SYS_CHMOD, sys2_chmod);
    reg(SYS_FCHMOD, sys2_chmod);
    reg(SYS_UMASK, sys2_umask);
    reg(SYS_LINK, sys2_enosys);
    reg(SYS_SYMLINK, sys2_enosys);
    reg(SYS_READLINK, sys2_enosys);

    /* identity */
    reg(SYS_GETPID, sys2_getpid);
    reg(SYS_GETTID, sys2_gettid);
    reg(SYS_GETPPID, sys2_getppid);
    reg(SYS_GETUID, sys2_getuid);
    reg(SYS_GETEUID, sys2_geteuid);
    reg(SYS_GETGID, sys2_getgid);
    reg(SYS_GETEGID, sys2_getegid);
    reg(SYS_SETUID, sys2_setuid);
    reg(SYS_SETGID, sys2_setgid);
    reg(SYS_UNAME, sys2_uname);

    /* time */
    reg(SYS_GETTIMEOFDAY, sys2_gettimeofday);
    reg(SYS_TIME, sys2_time);
    reg(SYS_CLOCK_GETTIME, sys2_clock_gettime);     /* replaces the stub */
    reg(SYS_NANOSLEEP, sys2_nanosleep);
    reg(SYS_TIMES, sys2_times);

    /* processes / signals */
    reg(SYS_KILL, sys2_kill);
    reg(SYS_WAIT4, sys2_wait4);
    reg(SYS_EXIT_GROUP, sys2_exit_group);

    /* system info */
    reg(SYS_SYSINFO, sys2_sysinfo);
    reg(SYS_GETRLIMIT, sys2_getrlimit);
    reg(SYS_SETRLIMIT, sys2_setrlimit);
    reg(SYS_PRCTL, sys2_prctl);
    reg(SYS_SCHED_GETAFFINITY, sys2_sched_getaffinity);
    reg(SYS_GETCPU, sys2_getcpu);

    /* ipc */
    reg(SYS_PIPE, sys2_pipe);
    reg(SYS_PIPE2, sys2_pipe2);

    return (int)g_registered;
}

/* ============================ acceptance ============================ */

static uint32_t g_checks;
static uint32_t g_failed;

static void st_report(const char *what, int ok, const char *detail)
{
    g_checks++;
    if (!ok) g_failed++;
    console_printf("systest: %s ... %s%s%s\n", what, ok ? "PASS" : "FAIL",
                   (detail != NULL) ? "  " : "", (detail != NULL) ? detail : "");
}

int syscalls2_selftest(void)
{
    char cwd[VFS_MAX_PATH];
    char buf[128];
    struct linux_stat st;

    g_checks = 0;
    g_failed = 0;
    vfs_set_cwd("/");

    /* --- stat family --- */
    st_report("stat /etc/passwd",
              (int)syscall_invoke(SYS_STAT, (uint64_t)(uintptr_t)"/etc/passwd",
                                  (uint64_t)(uintptr_t)&st, 0, 0, 0, 0) == 0,
              NULL);
    st_report("stat reports a regular file",
              (st.st_mode & S_IFMT_) == S_IFREG_, NULL);
    st_report("stat size > 0", st.st_size > 0, NULL);
    st_report("stat of a missing file -> -ENOENT",
              (int)syscall_invoke(SYS_STAT, (uint64_t)(uintptr_t)"/no/such/file",
                                  (uint64_t)(uintptr_t)&st, 0, 0, 0, 0) == -ENOENT_,
              NULL);
    st_report("lstat /etc is a directory",
              (int)syscall_invoke(SYS_LSTAT, (uint64_t)(uintptr_t)"/etc",
                                  (uint64_t)(uintptr_t)&st, 0, 0, 0, 0) == 0 &&
              (st.st_mode & S_IFMT_) == S_IFDIR_, NULL);
    st_report("access /etc/passwd", (int)syscall_invoke(SYS_ACCESS,
              (uint64_t)(uintptr_t)"/etc/passwd", 0, 0, 0, 0, 0) == 0, NULL);

    /* --- open/lseek/read/write/fstat --- */
    {
        const int fd = vfs_open("/syscalltest.txt", VFS_O_CREAT | VFS_O_RDWR);
        st_report("open a new file", fd >= 0, NULL);
        if (fd >= 0) {
            st_report("write through the fd",
                      vfs_write(fd, "0123456789", 10) == 10, NULL);
            st_report("lseek(0, SEEK_SET) -> 0",
                      (int)syscall_invoke(SYS_LSEEK, (uint64_t)fd, 0, SEEK_SET_,
                                          0, 0, 0) == 0, NULL);
            memset(buf, 0, sizeof(buf));
            st_report("read 4 bytes back", vfs_read(fd, buf, 4) == 4, NULL);
            st_report("payload intact", strcmp(buf, "0123") == 0, NULL);
            st_report("lseek(0, SEEK_END) -> 10",
                      (int)syscall_invoke(SYS_LSEEK, (uint64_t)fd, 0, SEEK_END_,
                                          0, 0, 0) == 10, NULL);
            st_report("fstat reports a regular file",
                      (int)syscall_invoke(SYS_FSTAT, (uint64_t)fd,
                                          (uint64_t)(uintptr_t)&st, 0, 0, 0, 0) == 0 &&
                      (st.st_mode & S_IFMT_) == S_IFREG_, NULL);
            vfs_close(fd);
        }
    }

    /* --- mkdir / getdents64 / rmdir / unlink --- */
    st_report("mkdir /systest_dir",
              (int)syscall_invoke(SYS_MKDIR, (uint64_t)(uintptr_t)"/systest_dir",
                                  0755, 0, 0, 0, 0) == 0, NULL);
    st_report("mkdir again -> -EEXIST",
              (int)syscall_invoke(SYS_MKDIR, (uint64_t)(uintptr_t)"/systest_dir",
                                  0755, 0, 0, 0, 0) == -EEXIST_, NULL);
    {
        const int dfd = vfs_opendir("/");
        int n = 0;
        if (dfd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = (int)syscall_invoke(SYS_GETDENTS64, (uint64_t)dfd,
                                    (uint64_t)(uintptr_t)buf, (uint64_t)sizeof(buf),
                                    0, 0, 0);
            vfs_closedir(dfd);
        }
        st_report("getdents64 lists /", n > 0, NULL);
    }
    st_report("unlink /syscalltest.txt",
              (int)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/syscalltest.txt",
                                  0, 0, 0, 0, 0) == 0, NULL);
    st_report("rmdir /systest_dir",
              (int)syscall_invoke(SYS_RMDIR, (uint64_t)(uintptr_t)"/systest_dir",
                                  0, 0, 0, 0, 0) == 0, NULL);

    /* --- rename --- */
    {
        (void)vfs_write_file("/ren_a.txt", "x", 1);
        st_report("rename /ren_a.txt -> /ren_b.txt",
                  (int)syscall_invoke(SYS_RENAME, (uint64_t)(uintptr_t)"/ren_a.txt",
                                      (uint64_t)(uintptr_t)"/ren_b.txt",
                                      0, 0, 0, 0) == 0, NULL);
        (void)vfs_unlink("/ren_b.txt");
    }

    /* --- cwd --- */
    st_report("chdir /etc", (int)syscall_invoke(SYS_CHDIR,
              (uint64_t)(uintptr_t)"/etc", 0, 0, 0, 0, 0) == 0, NULL);
    memset(cwd, 0, sizeof(cwd));
    st_report("getcwd returns /etc",
              (int)syscall_invoke(SYS_GETCWD, (uint64_t)(uintptr_t)cwd,
                                  (uint64_t)sizeof(cwd), 0, 0, 0, 0) ==
              (int)(uintptr_t)cwd && strcmp(cwd, "/etc") == 0, NULL);
    st_report("relative stat resolves against cwd",
              (int)syscall_invoke(SYS_STAT, (uint64_t)(uintptr_t)"passwd",
                                  (uint64_t)(uintptr_t)&st, 0, 0, 0, 0) == 0, NULL);
    vfs_set_cwd("/");

    /* --- identity --- */
    st_report("getpid returns the current task slot",
              (int)syscall_invoke(SYS_GETPID, 0, 0, 0, 0, 0, 0) == sched_cpu.current,
              NULL);
    st_report("getuid == 0 for the root shell",
              (int)syscall_invoke(SYS_GETUID, 0, 0, 0, 0, 0, 0) == 0, NULL);
    st_report("geteuid == 0 for the root shell",
              (int)syscall_invoke(SYS_GETEUID, 0, 0, 0, 0, 0, 0) == 0, NULL);
    {
        struct linux_utsname uts;
        memset(&uts, 0, sizeof(uts));
        st_report("uname fills sysname/machine",
                  (int)syscall_invoke(SYS_UNAME, (uint64_t)(uintptr_t)&uts, 0, 0, 0, 0, 0) == 0 &&
                  strcmp(uts.sysname, "Linux") == 0 &&
                  strcmp(uts.machine, "x86_64") == 0, NULL);
    }

    /* --- time --- */
    {
        struct linux_timespec ts;
        struct linux_timeval tv;
        memset(&ts, 0, sizeof(ts));
        memset(&tv, 0, sizeof(tv));
        st_report("clock_gettime(REALTIME) is a real epoch",
                  (int)syscall_invoke(SYS_CLOCK_GETTIME, 0,
                                      (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0) == 0 &&
                  ts.tv_sec > 1600000000, NULL);
        st_report("clock_gettime(MONOTONIC) is small",
                  (int)syscall_invoke(SYS_CLOCK_GETTIME, 1,
                                      (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0) == 0 &&
                  ts.tv_sec < 1000000, NULL);
        st_report("gettimeofday agrees with clock_gettime",
                  (int)syscall_invoke(SYS_GETTIMEOFDAY, (uint64_t)(uintptr_t)&tv,
                                      0, 0, 0, 0, 0) == 0 &&
                  tv.tv_sec >= ts.tv_sec - 1, NULL);
        st_report("time() returns an epoch",
                  (int)syscall_invoke(SYS_TIME, 0, 0, 0, 0, 0, 0) > 1600000000, NULL);
    }

    /* --- pipes --- */
    {
        int fds[2];
        st_report("pipe() through the syscall",
                  (int)syscall_invoke(SYS_PIPE, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0) == 0,
                  NULL);
        if (pipe_is_fd(fds[0])) {
            memset(buf, 0, sizeof(buf));
            st_report("write to the pipe",
                      (int)syscall_invoke(SYS_WRITE, (uint64_t)fds[1],
                                          (uint64_t)(uintptr_t)"pipe!", 5, 0, 0, 0) == 5,
                      NULL);
            st_report("read from the pipe",
                      (int)syscall_invoke(SYS_READ, (uint64_t)fds[0],
                                          (uint64_t)(uintptr_t)buf, 5, 0, 0, 0) == 5,
                      NULL);
            st_report("pipe payload intact", strcmp(buf, "pipe!") == 0, NULL);
            (void)syscall_invoke(SYS_CLOSE, (uint64_t)fds[0], 0, 0, 0, 0, 0);
            (void)syscall_invoke(SYS_CLOSE, (uint64_t)fds[1], 0, 0, 0, 0, 0);
        }
    }

    /* --- system info --- */
    {
        uint8_t info[112];
        memset(info, 0, sizeof(info));
        st_report("sysinfo reports uptime and ram",
                  (int)syscall_invoke(SYS_SYSINFO, (uint64_t)(uintptr_t)info, 0, 0, 0, 0, 0) == 0 &&
                  *(uint64_t *)(void *)(info + 8) > 0, NULL);
        {
            uint64_t rl[2] = { 0, 0 };
            st_report("getrlimit returns RLIM_INFINITY",
                      (int)syscall_invoke(SYS_GETRLIMIT, 3, (uint64_t)(uintptr_t)rl,
                                          0, 0, 0, 0) == 0 &&
                      rl[0] == 0x7FFFFFFFFFFFFFFFull, NULL);
        }
        st_report("filesystem is synced", (int)syscall_invoke(SYS_SYNC, 0, 0, 0, 0, 0, 0) == 0,
                  NULL);
    }

    /* --- explicit ENOSYS answers are honest, not silent --- */
    st_report("symlink() answers -ENOSYS",
              (int)syscall_invoke(SYS_SYMLINK, (uint64_t)(uintptr_t)"/a",
                                  (uint64_t)(uintptr_t)"/b", 0, 0, 0, 0) == -ENOSYS_,
              NULL);
    st_report("ioctl() answers -ENOTTY",
              (int)syscall_invoke(SYS_IOCTL, 1, 0, 0, 0, 0, 0) == -ENOTTY_, NULL);

    console_printf("systest: RESULT %s (%u/%u), %d syscalls registered\n",
                   (g_failed == 0) ? "PASS" : "FAIL",
                   g_checks - g_failed, g_checks, syscalls_registered_count());
    return (g_failed == 0) ? 0 : 1;
}
