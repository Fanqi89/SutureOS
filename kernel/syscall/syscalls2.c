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
#include "tty.h"
#include "userdb.h"
#include "vfs.h"

/* These layouts are ABI, not style: a libc indexes statx/termios/winsize by byte
 * offset, so a silent padding change would be a real bug. */
_Static_assert(sizeof(struct linux_statx) == 256, "statx must keep its Linux x86-64 size");
_Static_assert(sizeof(struct termios) == 60, "termios must keep its Linux x86-64 size");
_Static_assert(sizeof(struct winsize) == 8, "winsize must keep its Linux x86-64 size");

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
#define O_WRONLY_        1
#define O_RDWR_          2
#define O_CREAT_         0x40
#define O_TRUNC_         0x200
#define O_CLOEXEC_       0x80000
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

/* Resolve the (dirfd, path) pair of the *at family. AT_FDCWD is the cwd the rest
 * of this file already uses; a real dirfd is looked up in the task fd table and
 * turned into its VFS path, then joined. Returns 0 or a negative errno.
 *
 * Honest limit: SutureOS' VFS has no way to open a directory as a file
 * (vfs_open() answers VFS_EISDIR and there is no O_DIRECTORY), so a *directory*
 * fd can never be obtained by a user program today. The dirfd branch therefore
 * only ever sees a regular file fd and correctly answers ENOTDIR; AT_FDCWD (and
 * absolute paths) is the path that is actually usable. */
static int resolve_path_at(int64_t dirfd, const char *in, char *out, size_t outlen)
{
    if (in == NULL) return -EFAULT_;
    if (in[0] == '/') {
        snprintf(out, outlen, "%s", in);
        return 0;
    }
    if (dirfd == AT_FDCWD_) {
        resolve_path(in, out, outlen);
        return 0;
    }
    {
        char base[VFS_MAX_PATH];
        vfs_stat_t st;
        int vfd;

        if (dirfd < 0 || dirfd > 0x7fffffff) return -EBADF_;
        if (fd_pipe_end((int)dirfd) >= 0) return -ENOTDIR_;
        vfd = fd_vfs_fd((int)dirfd);
        if (vfd < 0) return -EBADF_;
        if (vfs_get_path(vfd, base, sizeof(base)) != VFS_OK) return -EBADF_;
        if (vfs_stat(base, &st) != VFS_OK) return -EBADF_;
        if (st.type != VFS_NODE_DIR) return -ENOTDIR_;
        if (strcmp(base, "/") == 0) {
            snprintf(out, outlen, "/%s", in);
        } else {
            snprintf(out, outlen, "%s/%s", base, in);
        }
        return 0;
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

/* Linux x86-64 struct statx from the VFS stat. Only STATX_BASIC_STATS fields
 * are filled; everything else (including the birth time, which no driver here
 * records) stays zero. */
static void fill_linux_statx(const vfs_stat_t *st, struct linux_statx *out)
{
    memset(out, 0, sizeof(*out));
    out->stx_mask = STATX_BASIC_STATS;
    out->stx_blksize = 4096;
    out->stx_nlink = (st->type == VFS_NODE_DIR) ? 2 : 1;
    out->stx_uid = st->uid;
    out->stx_gid = st->gid;
    switch (st->type) {
    case VFS_NODE_DIR:     out->stx_mode = S_IFDIR_ | 0755; break;
    case VFS_NODE_SYMLINK: out->stx_mode = S_IFLNK_ | 0777; break;
    default:               out->stx_mode = S_IFREG_ | (st->mode & 0777); break;
    }
    out->stx_ino = 1;
    out->stx_size = st->size;
    out->stx_blocks = (st->size + 511) / 512;
    out->stx_atime.tv_sec = (int64_t)st->atime;
    out->stx_ctime.tv_sec = (int64_t)st->ctime;
    out->stx_mtime.tv_sec = (int64_t)st->mtime;
    out->stx_dev_major = 0;
    out->stx_dev_minor = 1;
}

/* ============================ file status ============================ */

/* stat one already-resolved absolute path into a caller struct. */
static uint64_t stat_path(const char *abs, uint64_t st_va)
{
    struct linux_stat *out = (struct linux_stat *)(uintptr_t)st_va;
    vfs_stat_t st;

    if (out == NULL) return err(EFAULT_);
    if (!ptr_ok(st_va, sizeof(*out))) return err(EFAULT_);
    if (vfs_stat(abs, &st) != VFS_OK) return err(ENOENT_);
    fill_linux_stat(&st, out);
    return 0;
}

static uint64_t do_stat(uint64_t path_va, uint64_t st_va, int follow_links)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];

    (void)follow_links;                 /* the VFS has no symlink resolution yet */
    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    return stat_path(abs, st_va);
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

/* The descriptor a syscall receives is a *task-table* fd (sys_open() allocated
 * it), while the VFS speaks its own numbers. lseek/fstat below used to pass the
 * syscall fd straight to the VFS, which only worked while the two tables lined
 * up - and after dup() they no longer do (two task fds share one VFS fd). Resolve
 * through the table first and fall back to the raw value so kernel-internal
 * callers that really do pass a VFS fd (the `systest` harness) keep working. */
static int vfs_fd_of(uint64_t fd)
{
    const int v = fd_vfs_fd((int)fd);
    return (v >= 0) ? v : (int)fd;
}

static uint64_t sys2_fstat(uint64_t fd, uint64_t st_va, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    struct linux_stat *out = (struct linux_stat *)(uintptr_t)st_va;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (out == NULL || !ptr_ok(st_va, sizeof(*out))) return err(EFAULT_);
    if (fd_pipe_end((int)fd) >= 0) {
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
        const int vfd = vfs_fd_of(fd);
        const int end = vfs_seek(vfd, 0, SEEK_END_);
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFREG_ | 0644;
        out->st_nlink = 1;
        out->st_size = (end > 0) ? end : 0;
        out->st_blksize = 4096;
        (void)vfs_seek(vfd, 0, SEEK_SET_);
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
    const int vfd = vfs_fd_of(fd);
    (void)a4; (void)a5; (void)a6;
    if (fd_pipe_end((int)fd) >= 0) return err(ESPIPE_);
    if (vfs_seek(vfd, (uint32_t)offset, (int)whence) < 0) return err(EINVAL_);
    return vfs_seek(vfd, 0, SEEK_CUR_);
}

/* dup(): lowest free descriptor naming the same open file (so both share the
 * file offset, exactly like Linux) or the same pipe end. The fd table and the
 * VFS reference count live in syscalls.c / vfs.c, next to the objects they
 * protect; this is only the syscall-shaped wrapper. */
static uint64_t sys2_dup(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return ok(fd_dup_lowest((int)fd));
}

/* dup2(oldfd, newfd): close newfd when it is open, then make it a duplicate.
 * Works for files and for pipes (a pipe end parked in a low descriptor is what
 * makes `dup2(pipe_write, 1)` real stdout redirection in this kernel). */
static uint64_t sys2_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (oldfd == newfd) {
        return fd_is_open((int)oldfd) ? ok((int)newfd) : err(EBADF_);
    }
    return ok(fd_dup_to((int)oldfd, (int)newfd));
}

/* dup3(oldfd, newfd, flags): dup2 with the extra rule that oldfd == newfd is
 * EINVAL and that only O_CLOEXEC may be passed. */
static uint64_t sys2_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if ((flags & ~(uint64_t)O_CLOEXEC_) != 0) return err(EINVAL_);
    if (oldfd == newfd) return err(EINVAL_);
    if (flags & O_CLOEXEC_) {
        /* Accepted and deliberately not recorded: SutureOS keeps no per-fd
         * descriptor flags and its execve() path (kernel/syscall/ring3.c) does
         * not close descriptors, so pretending to store FD_CLOEXEC here would be
         * a lie. Fixing that needs a flags field in the fd table plus an execve
         * close-on-exec pass; until then the call itself behaves like dup2. */
    }
    return ok(fd_dup_to((int)oldfd, (int)newfd));
}

/* ioctl(): fds 0/1/2 are the console and get the minimal tty surface in
 * kernel/vfs/tty.c (TCGETS/TCSETS/TCSETSW/TCSETSF, TIOCGWINSZ, TCFLSH). Pipes,
 * regular files and directories have no tty to configure and keep answering
 * -ENOTTY. */
static uint64_t sys2_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if ((int64_t)fd < 0) return err(EBADF_);
    if (fd < 3) {
        size_t need = 0;
        if (request == TCGETS || request == TCSETS ||
            request == TCSETSW || request == TCSETSF) {
            need = sizeof(struct termios);
        } else if (request == TIOCGWINSZ) {
            need = sizeof(struct winsize);
        }
        if (need != 0 && (arg == 0 || !ptr_ok(arg, need))) return err(EFAULT_);
        return ok(tty_ioctl((int)fd, request, (void *)(uintptr_t)arg));
    }
    return err(ENOTTY_);
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

/* ---- creat / the *at family / statx ---------------------------------------- */

/* creat(path, mode) is exactly open(path, O_CREAT|O_WRONLY|O_TRUNC, mode): this
 * reuses the existing open path (fd table + VFS), and the mode is still not
 * enforced by the ramfs (files report 0644), same as open(). */
static uint64_t sys2_creat(uint64_t path_va, uint64_t mode, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    return syscall_invoke(SYS_OPEN, (uint64_t)(uintptr_t)abs,
                          (uint64_t)(O_CREAT_ | O_WRONLY_ | O_TRUNC_), mode, 0, 0, 0);
}

static uint64_t sys2_openat(uint64_t dirfd, uint64_t path_va, uint64_t flags,
                            uint64_t mode, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    int r;
    (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    r = resolve_path_at((int64_t)dirfd, path, abs, sizeof(abs));
    if (r < 0) return ok(r);
    return syscall_invoke(SYS_OPEN, (uint64_t)(uintptr_t)abs, flags, mode, 0, 0, 0);
}

static uint64_t sys2_mkdirat(uint64_t dirfd, uint64_t path_va, uint64_t mode,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    int r;
    (void)mode; (void)a4; (void)a5; (void)a6;   /* ramfs directories are 0755 */

    if (path == NULL) return err(EFAULT_);
    r = resolve_path_at((int64_t)dirfd, path, abs, sizeof(abs));
    if (r < 0) return ok(r);
    {
        vfs_stat_t st;
        if (vfs_stat(abs, &st) == VFS_OK) return err(EEXIST_);
    }
    return (vfs_mkdir(abs, 0755) == VFS_OK) ? 0 : err(EIO_);
}

static uint64_t sys2_unlinkat(uint64_t dirfd, uint64_t path_va, uint64_t flags,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    int r;
    (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    if (flags & ~(uint64_t)AT_REMOVEDIR) return err(EINVAL_);
    r = resolve_path_at((int64_t)dirfd, path, abs, sizeof(abs));
    if (r < 0) return ok(r);
    if (flags & AT_REMOVEDIR) {
        return (vfs_rmdir(abs) == VFS_OK) ? 0 : err(ENOENT_);
    }
    return (vfs_unlink(abs) == VFS_OK) ? 0 : err(ENOENT_);
}

static uint64_t sys2_renameat(uint64_t olddirfd, uint64_t old_va, uint64_t newdirfd,
                              uint64_t new_va, uint64_t a5, uint64_t a6)
{
    const char *oldp = str_arg(old_va);
    const char *newp = str_arg(new_va);
    char a[VFS_MAX_PATH], b[VFS_MAX_PATH];
    int r;
    (void)a5; (void)a6;

    if (oldp == NULL || newp == NULL) return err(EFAULT_);
    r = resolve_path_at((int64_t)olddirfd, oldp, a, sizeof(a));
    if (r < 0) return ok(r);
    r = resolve_path_at((int64_t)newdirfd, newp, b, sizeof(b));
    if (r < 0) return ok(r);
    return (vfs_rename(a, b) == VFS_OK) ? 0 : err(EPERM_);
}

static uint64_t sys2_fstatat(uint64_t dirfd, uint64_t path_va, uint64_t st_va,
                             uint64_t flags, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    int r;
    (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    if (flags & AT_EMPTY_PATH) return err(EINVAL_);   /* no fd-only stat variant */
    /* AT_SYMLINK_NOFOLLOW is accepted and has no effect: no VFS driver resolves
     * symlinks, so lstat and stat are the same operation here (as in do_stat). */
    r = resolve_path_at((int64_t)dirfd, path, abs, sizeof(abs));
    if (r < 0) return ok(r);
    return stat_path(abs, st_va);
}

/* statx(dirfd, path, flags, mask, buf): the BASIC_STATS subset of the Linux
 * structure. The `mask` argument is advisory (Linux may return more than was
 * asked for and always sets stx_mask to what it filled); this kernel always
 * fills exactly STATX_BASIC_STATS. */
static uint64_t sys2_statx(uint64_t dirfd, uint64_t path_va, uint64_t flags,
                           uint64_t mask, uint64_t buf_va, uint64_t a6)
{
    const char *path = str_arg(path_va);
    struct linux_statx *out = (struct linux_statx *)(uintptr_t)buf_va;
    char abs[VFS_MAX_PATH];
    vfs_stat_t st;
    int r;
    (void)mask; (void)a6;

    if (path == NULL || out == NULL) return err(EFAULT_);
    if (!ptr_ok(buf_va, sizeof(*out))) return err(EFAULT_);
    if (flags & AT_EMPTY_PATH) return err(EINVAL_);
    r = resolve_path_at((int64_t)dirfd, path, abs, sizeof(abs));
    if (r < 0) return ok(r);
    if (vfs_stat(abs, &st) != VFS_OK) return err(ENOENT_);
    fill_linux_statx(&st, out);
    return 0;
}

/* readlink(): POSIX says -EINVAL when the path is not a symlink, and no VFS
 * driver here can create one (see the registration comments for link/symlink).
 * The one case that cannot be served is a node a future driver *reports* as a
 * symlink: no driver stores a link target, so that answers -ENOSYS instead of
 * copying garbage. */
static uint64_t sys2_readlink(uint64_t path_va, uint64_t buf_va, uint64_t bufsiz,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    const char *path = str_arg(path_va);
    char abs[VFS_MAX_PATH];
    vfs_stat_t st;
    (void)buf_va; (void)bufsiz; (void)a4; (void)a5; (void)a6;

    if (path == NULL) return err(EFAULT_);
    resolve_path(path, abs, sizeof(abs));
    if (vfs_stat(abs, &st) != VFS_OK) return err(ENOENT_);
    if (st.type != VFS_NODE_SYMLINK) return err(EINVAL_);
    return err(ENOSYS_);
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
    reg(SYS_DUP3, sys2_dup3);           /* Linux number 292 (was registered as 243) */
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
    reg(SYS_CREAT, sys2_creat);         /* open(O_CREAT|O_WRONLY|O_TRUNC) */
    reg(SYS_CHMOD, sys2_chmod);
    reg(SYS_FCHMOD, sys2_chmod);
    reg(SYS_UMASK, sys2_umask);
    /* Hard links and symlinks are NOT stubs by omission: the VFS driver
     * interface (kernel/vfs/vfs.h) has no link()/symlink() hook, every node
     * model is path-keyed with a single parent (ramfs: xj_vfs_node_t.children +
     * parent; FAT32: one 8.3 directory entry per file; ext2/squashfs are
     * read-only), and no node stores a link target. A hard link would need
     * inode-style link counts shared by several names, a symlink needs a target
     * string per node, and both would have to be implemented in each driver
     * plus the VFS. Until that exists these two keep answering -ENOSYS. */
    reg(SYS_LINK, sys2_enosys);
    reg(SYS_SYMLINK, sys2_enosys);
    reg(SYS_READLINK, sys2_readlink);   /* real: -EINVAL for a non-symlink */

    /* the *at family: same VFS operations as above, with dirfd resolution */
    reg(SYS_OPENAT, sys2_openat);
    reg(SYS_MKDIRAT, sys2_mkdirat);
    reg(SYS_FSTATAT, sys2_fstatat);
    reg(SYS_UNLINKAT, sys2_unlinkat);
    reg(SYS_RENAMEAT, sys2_renameat);
    reg(SYS_STATX, sys2_statx);

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

    /* sockets: Linux x86-64 numbers 41..55 (no SOCKETPAIR) over the in-kernel
     * socket ABI in kernel/net/net.c. kernel/syscall/sockets.c is the ABI
     * boundary: user-pointer validation, sockaddr byte-order conversion and the
     * socket descriptor kind in the fd table. OFD/socket pairs are not offered. */
    {
        extern uint64_t sys_socket(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_connect(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_accept(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_sendto(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_recvfrom(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_sendmsg(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_recvmsg(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_sock_shutdown(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_bind(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_listen(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_getsockname(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_getpeername(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_setsockopt(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        extern uint64_t sys_getsockopt(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

        reg(SYS_SOCKET, sys_socket);
        reg(SYS_CONNECT, sys_connect);
        reg(SYS_ACCEPT, sys_accept);
        reg(SYS_SENDTO, sys_sendto);
        reg(SYS_RECVFROM, sys_recvfrom);
        reg(SYS_SENDMSG, sys_sendmsg);
        reg(SYS_RECVMSG, sys_recvmsg);
        reg(SYS_SHUTDOWN, sys_sock_shutdown);
        reg(SYS_BIND, sys_bind);
        reg(SYS_LISTEN, sys_listen);
        reg(SYS_GETSOCKNAME, sys_getsockname);
        reg(SYS_GETPEERNAME, sys_getpeername);
        reg(SYS_SETSOCKOPT, sys_setsockopt);
        reg(SYS_GETSOCKOPT, sys_getsockopt);
    }

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

/* ==================== acceptance: the new syscall surface ====================
 * Exercises dup/dup2/dup3, creat, the *at family, statx and the tty ioctl
 * surface through syscall_invoke() - the same dispatch path a ring-3 program
 * takes. Silent on success by design: the boot log must stay identical, so
 * failures are collected and printed (as `[sys2] new syscalls FAIL ...`) only
 * after every descriptor the test touched has been closed. That ordering also
 * means a half-finished test can never leave stdout redirected into a pipe. */

#define NEW_FAIL_MAX 16

static const char *g_new_fail[NEW_FAIL_MAX];
static uint32_t g_new_fail_n;

static void new_fail(const char *what)
{
    if (g_new_fail_n < NEW_FAIL_MAX) g_new_fail[g_new_fail_n] = what;
    g_new_fail_n++;
}

static int new_close(int fd)
{
    if (fd >= 0) (void)syscall_invoke(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
    return -1;
}

/* Drop a descriptor the test installed in a low slot, but only when it was free
 * before the test ran (so a real caller's descriptor is never stolen). */
static void new_restore_fd(int fd, int was_open)
{
    if (!was_open && fd_is_open(fd)) new_close(fd);
}

static int file_equals(const char *path, const char *want, size_t len)
{
    size_t got = 0;
    void *p = vfs_read_file(path, &got);
    const int same = (p != NULL && got == len && memcmp(p, want, len) == 0);
    if (p != NULL) lemis_heap_free(p);
    return same;
}

static int new_surface_selftest(void)
{
    char saved_cwd[VFS_MAX_PATH];
    int fd = -1, dup_fd = -1, fd2 = -1;
    int pipefd[2] = { -1, -1 };
    int pipe_dup = -1;
    int was_open[32];
    uint32_t i;

    g_new_fail_n = 0;
    snprintf(saved_cwd, sizeof(saved_cwd), "%s", vfs_get_cwd());
    vfs_set_cwd("/");
    for (i = 0; i < 32; i++) was_open[i] = fd_is_open((int)i);

    /* ---- dup(): a second descriptor on the same open file ---- */
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2dup.txt", 0, 0, 0, 0, 0);
    fd = (int)syscall_invoke(SYS_OPEN, (uint64_t)(uintptr_t)"/sys2dup.txt",
                             (uint64_t)(O_RDWR_ | O_CREAT_ | O_TRUNC_), 0644, 0, 0, 0);
    if (fd < 0) {
        new_fail("dup: open /sys2dup.txt");
    } else {
        if ((int)syscall_invoke(SYS_WRITE, (uint64_t)fd,
                                (uint64_t)(uintptr_t)"AAAA", 4, 0, 0, 0) != 4)
            new_fail("dup: write through the original fd");
        dup_fd = (int)syscall_invoke(SYS_DUP, (uint64_t)fd, 0, 0, 0, 0, 0);
        if (dup_fd < 0) {
            new_fail("dup: dup() answered an error");
        } else if (dup_fd == fd) {
            new_fail("dup: dup() returned the same descriptor");
            dup_fd = new_close(dup_fd);
        } else {
            /* the duplicate shares the file offset, so this lands after "AAAA" */
            if ((int)syscall_invoke(SYS_WRITE, (uint64_t)dup_fd,
                                    (uint64_t)(uintptr_t)"BBBB", 4, 0, 0, 0) != 4)
                new_fail("dup: write through the duplicate fd");
            /* closing one descriptor must not close the shared open file either */
            fd = new_close(fd);
            if (!file_equals("/sys2dup.txt", "AAAABBBB", 8))
                new_fail("dup: one open file, shared offset, refcounted close");
            /* and the duplicate is a full descriptor on that open file: it can
             * seek and read the shared content back */
            if ((int)syscall_invoke(SYS_LSEEK, (uint64_t)dup_fd, 0, SEEK_SET_,
                                    0, 0, 0) != 0) {
                new_fail("dup: lseek on the duplicate fd");
            } else {
                char rb[9];
                memset(rb, 0, sizeof(rb));
                if ((int)syscall_invoke(SYS_READ, (uint64_t)dup_fd,
                                        (uint64_t)(uintptr_t)rb, 8, 0, 0, 0) != 8 ||
                    memcmp(rb, "AAAABBBB", 8) != 0)
                    new_fail("dup: read the shared file through the duplicate");
            }
            dup_fd = new_close(dup_fd);
        }
    }
    fd = new_close(fd);
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2dup.txt", 0, 0, 0, 0, 0);
    if ((int)syscall_invoke(SYS_DUP, 77, 0, 0, 0, 0, 0) != -EBADF_)
        new_fail("dup: a closed descriptor answers -EBADF");

    /* ---- dup2() to a fixed descriptor ---- */
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2dup2.txt", 0, 0, 0, 0, 0);
    fd = (int)syscall_invoke(SYS_OPEN, (uint64_t)(uintptr_t)"/sys2dup2.txt",
                             (uint64_t)(O_RDWR_ | O_CREAT_ | O_TRUNC_), 0644, 0, 0, 0);
    if (fd < 0) {
        new_fail("dup2: open /sys2dup2.txt");
    } else {
        if ((int)syscall_invoke(SYS_DUP2, (uint64_t)fd, 20, 0, 0, 0, 0) != 20) {
            new_fail("dup2: did not land on the requested descriptor");
        } else {
            if ((int)syscall_invoke(SYS_WRITE, 20, (uint64_t)(uintptr_t)"CCCC",
                                    4, 0, 0, 0) != 4)
                new_fail("dup2: write through the fixed descriptor");
            new_close(20);
        }
        fd = new_close(fd);
        if (!file_equals("/sys2dup2.txt", "CCCC", 4))
            new_fail("dup2: content written through the duplicate");

        /* ---- dup3(): O_CLOEXEC accepted, bad flags / equal fds are EINVAL ---- */
        fd = (int)syscall_invoke(SYS_OPEN, (uint64_t)(uintptr_t)"/sys2dup2.txt",
                                 (uint64_t)O_RDWR_, 0, 0, 0, 0);
        if (fd < 0) {
            new_fail("dup3: reopen /sys2dup2.txt");
        } else {
            if ((int)syscall_invoke(SYS_DUP3, (uint64_t)fd, 21,
                                    (uint64_t)O_CLOEXEC_, 0, 0, 0) != 21)
                new_fail("dup3: O_CLOEXEC duplicate to a fixed descriptor");
            new_close(21);
            if ((int)syscall_invoke(SYS_DUP3, (uint64_t)fd, 22, 0x1234, 0, 0, 0) != -EINVAL_)
                new_fail("dup3: unknown flags -> -EINVAL");
            if ((int)syscall_invoke(SYS_DUP3, (uint64_t)fd, (uint64_t)fd,
                                    0, 0, 0, 0) != -EINVAL_)
                new_fail("dup3: oldfd == newfd -> -EINVAL");
            fd = new_close(fd);
        }
    }
    fd = new_close(fd);
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2dup2.txt", 0, 0, 0, 0, 0);

    /* ---- openat(AT_FDCWD, ...) behaves like open(...) ---- */
    fd = (int)syscall_invoke(SYS_OPEN, (uint64_t)(uintptr_t)"/etc/motd", 0, 0, 0, 0, 0);
    fd2 = (int)syscall_invoke(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD_,
                              (uint64_t)(uintptr_t)"/etc/motd", 0, 0, 0, 0);
    if (fd < 0 || fd2 < 0) {
        new_fail("openat(AT_FDCWD, \"/etc/motd\") == open(\"/etc/motd\")");
    } else {
        char a = 0, b = 0;
        const int na = (int)syscall_invoke(SYS_READ, (uint64_t)fd,
                                           (uint64_t)(uintptr_t)&a, 1, 0, 0, 0);
        const int nb = (int)syscall_invoke(SYS_READ, (uint64_t)fd2,
                                           (uint64_t)(uintptr_t)&b, 1, 0, 0, 0);
        if (na != 1 || nb != 1 || a != b)
            new_fail("openat(AT_FDCWD): same bytes as open()");
    }
    fd = new_close(fd);
    fd2 = new_close(fd2);
    /* relative to the cwd (open() itself does not resolve, openat() does) */
    vfs_set_cwd("/etc");
    fd = (int)syscall_invoke(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD_,
                             (uint64_t)(uintptr_t)"motd", 0, 0, 0, 0);
    if (fd < 0) new_fail("openat: relative path resolved against the cwd");
    fd = new_close(fd);
    vfs_set_cwd("/");

    /* ---- fstatat / statx on the same file ---- */
    {
        struct linux_stat st;
        struct linux_statx sx;
        memset(&st, 0, sizeof(st));
        memset(&sx, 0, sizeof(sx));
        if ((int)syscall_invoke(SYS_FSTATAT, (uint64_t)(int64_t)AT_FDCWD_,
                                (uint64_t)(uintptr_t)"/etc/motd",
                                (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0 ||
            (st.st_mode & S_IFMT_) != S_IFREG_ || st.st_size <= 0)
            new_fail("fstatat(AT_FDCWD): a regular file with size > 0");
        if ((int)syscall_invoke(SYS_STATX, (uint64_t)(int64_t)AT_FDCWD_,
                                (uint64_t)(uintptr_t)"/etc/motd", 0,
                                (uint64_t)STATX_BASIC_STATS,
                                (uint64_t)(uintptr_t)&sx, 0) != 0 ||
            (sx.stx_mask & STATX_BASIC_STATS) != STATX_BASIC_STATS ||
            sx.stx_size == 0 || (sx.stx_mode & S_IFMT_) != S_IFREG_)
            new_fail("statx(AT_FDCWD, \"/etc/motd\"): BASIC_STATS with size > 0");
    }

    /* ---- creat / mkdirat / renameat / unlinkat ---- */
    fd = (int)syscall_invoke(SYS_CREAT, (uint64_t)(uintptr_t)"/sys2creat.txt",
                             0644, 0, 0, 0, 0);
    if (fd < 0) {
        new_fail("creat: create /sys2creat.txt");
    } else {
        if ((int)syscall_invoke(SYS_WRITE, (uint64_t)fd,
                                (uint64_t)(uintptr_t)"X", 1, 0, 0, 0) != 1)
            new_fail("creat: write through the new descriptor");
        fd = new_close(fd);
        if (!file_equals("/sys2creat.txt", "X", 1))
            new_fail("creat: O_CREAT|O_WRONLY|O_TRUNC through the open path");
    }
    fd = new_close(fd);
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2creat.txt", 0, 0, 0, 0, 0);

    if ((int)syscall_invoke(SYS_MKDIRAT, (uint64_t)(int64_t)AT_FDCWD_,
                            (uint64_t)(uintptr_t)"/sys2at_dir", 0755, 0, 0, 0) != 0) {
        new_fail("mkdirat(AT_FDCWD)");
    } else if ((int)syscall_invoke(SYS_RENAMEAT, (uint64_t)(int64_t)AT_FDCWD_,
                                   (uint64_t)(uintptr_t)"/sys2at_dir",
                                   (uint64_t)(int64_t)AT_FDCWD_,
                                   (uint64_t)(uintptr_t)"/sys2at_dir2", 0, 0) != 0) {
        new_fail("renameat(AT_FDCWD, AT_FDCWD)");
    } else if ((int)syscall_invoke(SYS_UNLINKAT, (uint64_t)(int64_t)AT_FDCWD_,
                                   (uint64_t)(uintptr_t)"/sys2at_dir2",
                                   (uint64_t)AT_REMOVEDIR, 0, 0, 0) != 0) {
        new_fail("unlinkat(AT_FDCWD, AT_REMOVEDIR)");
    }
    (void)syscall_invoke(SYS_UNLINKAT, (uint64_t)(int64_t)AT_FDCWD_,
                         (uint64_t)(uintptr_t)"/sys2at_dir", (uint64_t)AT_REMOVEDIR,
                         0, 0, 0);
    (void)syscall_invoke(SYS_UNLINKAT, (uint64_t)(int64_t)AT_FDCWD_,
                         (uint64_t)(uintptr_t)"/sys2at_dir2", (uint64_t)AT_REMOVEDIR,
                         0, 0, 0);

    /* ---- the console tty ioctl surface on fd 1 ---- */
    {
        struct termios tio, back, set;
        struct winsize ws;
        memset(&tio, 0, sizeof(tio));
        memset(&back, 0, sizeof(back));
        memset(&ws, 0, sizeof(ws));
        if ((int)syscall_invoke(SYS_IOCTL, 1, (uint64_t)TCGETS,
                                (uint64_t)(uintptr_t)&tio, 0, 0, 0) != 0) {
            new_fail("ioctl(1, TCGETS)");
        } else {
            set = tio;
            set.c_lflag ^= TTY_LFLAG_ECHO;      /* make the round trip visible */
            if ((int)syscall_invoke(SYS_IOCTL, 1, (uint64_t)TCSETS,
                                    (uint64_t)(uintptr_t)&set, 0, 0, 0) != 0) {
                new_fail("ioctl(1, TCSETS)");
            } else if ((int)syscall_invoke(SYS_IOCTL, 1, (uint64_t)TCGETS,
                                           (uint64_t)(uintptr_t)&back, 0, 0, 0) != 0 ||
                       back.c_lflag != set.c_lflag) {
                new_fail("ioctl: termios round trip (TCSETS then TCGETS)");
            }
            (void)syscall_invoke(SYS_IOCTL, 1, (uint64_t)TCSETS,
                                 (uint64_t)(uintptr_t)&tio, 0, 0, 0);
        }
        if ((int)syscall_invoke(SYS_IOCTL, 1, (uint64_t)TIOCGWINSZ,
                                (uint64_t)(uintptr_t)&ws, 0, 0, 0) != 0 ||
            ws.ws_row == 0 || ws.ws_col == 0)
            new_fail("ioctl(1, TIOCGWINSZ) reports non-zero rows/cols");
        if ((int)syscall_invoke(SYS_IOCTL, 1, 0x1234, 0, 0, 0, 0) != -ENOTTY_)
            new_fail("ioctl: an unknown request still answers -ENOTTY");
    }

    /* ---- a duplicated pipe end keeps EOF semantics right ---- */
    if ((int)syscall_invoke(SYS_PIPE, (uint64_t)(uintptr_t)pipefd, 0, 0, 0, 0, 0) != 0) {
        new_fail("dup: pipe()");
    } else {
        pipe_dup = (int)syscall_invoke(SYS_DUP, (uint64_t)pipefd[1], 0, 0, 0, 0, 0);
        if (pipe_dup < 0) {
            new_fail("dup: duplicate a pipe write end");
        } else {
            /* closing the original write end must not produce EOF: one writer
             * (the duplicate) is still open */
            pipefd[1] = new_close(pipefd[1]);
            if ((int)syscall_invoke(SYS_WRITE, (uint64_t)pipe_dup,
                                    (uint64_t)(uintptr_t)"Z", 1, 0, 0, 0) != 1)
                new_fail("dup: write through the duplicated pipe end");
            {
                char c = 0;
                if ((int)syscall_invoke(SYS_READ, (uint64_t)pipefd[0],
                                        (uint64_t)(uintptr_t)&c, 1, 0, 0, 0) != 1 ||
                    c != 'Z')
                    new_fail("dup: read the byte back from the pipe");
            }
            pipe_dup = new_close(pipe_dup);
            {
                char c = 0;
                if ((int)syscall_invoke(SYS_READ, (uint64_t)pipefd[0],
                                        (uint64_t)(uintptr_t)&c, 1, 0, 0, 0) != 0)
                    new_fail("dup: EOF once the last write end closed");
            }
        }
    }

    /* ---- leave nothing behind, then report ---- */
    fd = new_close(fd);
    fd2 = new_close(fd2);
    dup_fd = new_close(dup_fd);
    pipe_dup = new_close(pipe_dup);
    pipefd[0] = new_close(pipefd[0]);
    pipefd[1] = new_close(pipefd[1]);
    for (i = 0; i < 32; i++) new_restore_fd((int)i, was_open[i]);
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2dup.txt", 0, 0, 0, 0, 0);
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2dup2.txt", 0, 0, 0, 0, 0);
    (void)syscall_invoke(SYS_UNLINK, (uint64_t)(uintptr_t)"/sys2creat.txt", 0, 0, 0, 0, 0);
    (void)syscall_invoke(SYS_UNLINKAT, (uint64_t)(int64_t)AT_FDCWD_,
                         (uint64_t)(uintptr_t)"/sys2at_dir", (uint64_t)AT_REMOVEDIR,
                         0, 0, 0);
    (void)syscall_invoke(SYS_UNLINKAT, (uint64_t)(int64_t)AT_FDCWD_,
                         (uint64_t)(uintptr_t)"/sys2at_dir2", (uint64_t)AT_REMOVEDIR,
                         0, 0, 0);
    vfs_set_cwd(saved_cwd);

    for (i = 0; i < g_new_fail_n && i < NEW_FAIL_MAX; i++) {
        console_printf("[sys2] new syscalls FAIL: %s\n", g_new_fail[i]);
    }
    if (g_new_fail_n > NEW_FAIL_MAX) {
        console_printf("[sys2] new syscalls FAIL: %u more\n",
                       (uint64_t)(g_new_fail_n - NEW_FAIL_MAX));
    }
    return (int)g_new_fail_n;
}

/* Boot-time entry point: silent when everything works, so the serial log keeps
 * exactly the lines the frozen boot presentation has always printed. */
int syscalls2_boot_selftest(void)
{
    int fails = new_surface_selftest();

    /* The socket ABI boundary (descriptor kind, bind/getsockname round trip,
     * getsockopt(SO_TYPE), family/type refusals, close()) plus the pure DNS and
     * DHCP packet builders. All three are silent on success. The socket part
     * needs the NIC to be up - net_socket() would otherwise call net_init() and
     * print lines - so it is skipped (not faked) when the link is down. */
    {
        extern int sockets_boot_selftest(void);
        extern int dns_selftest(void);
        extern int dhcp_selftest(void);
        extern int net_ready(void);

        fails += dns_selftest();
        fails += dhcp_selftest();
        if (net_ready())
            fails += sockets_boot_selftest();
    }
    return fails;
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

    /* --- batch 3: dup/dup2/dup3, the *at family, statx, the tty ioctl surface.
     * Silent on success like at boot; a failure prints `[sys2] ... FAIL` lines
     * from the check itself, so this one line is just the tally. --- */
    st_report("dup/dup2/dup3, *at, statx, tty ioctl",
              new_surface_selftest() == 0, NULL);

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
