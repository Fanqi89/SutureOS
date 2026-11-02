/* r3tool.h - the small syscall surface the SutureOS ring-3 file tools share.
 *
 * License: SutureOS is GPL-3.0; this file is part of it.
 * Reference notice: the directory-listing approach was informed by VimtuOS'
 * user/busybox/vimtu_dirent.c (GPL-3.0). Only the *idea* was reused - no code was
 * copied. VimtuOS exposes dir_open/dir_read/dir_close on its own int 0x80 ABI;
 * SutureOS already registers the Linux numbers, so the tools here use
 * open(path, O_DIRECTORY) + getdents64(2) and print the real -errno.
 *
 * Every call below goes through the *Linux-numbered* path (user/lib/syscall.S,
 * the `syscall` instruction). That path returns -errno, which is what lets the
 * tools report the actual reason a file operation failed instead of a blanket
 * -1. printf()/puts() underneath still speak the int 0x80 own ABI.
 *
 * Kernel side (all already registered, see kernel/syscall/syscalls*.c):
 *   2 open, 3 close, 0 read, 1 write, 4 stat, 8 lseek, 74 fsync, 79 getcwd,
 *   82 rename, 83 mkdir, 84 rmdir, 87 unlink, 137 statfs, 217 getdents64.
 */
#ifndef SUTUREOS_R3TOOL_H
#define SUTUREOS_R3TOOL_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* user/lib/syscall.S: Linux-numbered trampoline, returns -errno. */
extern long __v64_syscall(long nr, long a1, long a2, long a3, long a4, long a5);

#define R3_SYS_READ        0
#define R3_SYS_WRITE       1
#define R3_SYS_OPEN        2
#define R3_SYS_CLOSE       3
#define R3_SYS_STAT        4
#define R3_SYS_FSTAT       5
#define R3_SYS_LSEEK       8
#define R3_SYS_FSYNC      74
#define R3_SYS_GETCWD     79
#define R3_SYS_RENAME     82
#define R3_SYS_MKDIR      83
#define R3_SYS_RMDIR      84
#define R3_SYS_UNLINK     87
#define R3_SYS_STATFS    137
#define R3_SYS_GETDENTS64 217

/* Not in user/lib/fcntl.h yet: ask open() for a directory cursor (sys_open hands
 * back vfs_opendir()'s cursor when the flag is set; getdents64 reads it). */
#define R3_O_DIRECTORY 0x10000

/* d_type values the kernel writes (fs-independent, Linux numbering). */
#define R3_DT_DIR 4
#define R3_DT_REG 8

/* Linux' linux_dirent64, byte for byte - kernel/syscall/syscalls2.c fills it. */
struct r3_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

/* ---------------- syscall wrappers (return -errno) ---------------- */

static inline long r3_open(const char *path, int flags, int mode)
{
    return __v64_syscall(R3_SYS_OPEN, (long)(uintptr_t)path, (long)flags,
                         (long)mode, 0, 0);
}

static inline long r3_read(int fd, void *buf, unsigned long len)
{
    return __v64_syscall(R3_SYS_READ, (long)fd, (long)(uintptr_t)buf, (long)len, 0, 0);
}

static inline long r3_write(int fd, const void *buf, unsigned long len)
{
    return __v64_syscall(R3_SYS_WRITE, (long)fd, (long)(uintptr_t)buf, (long)len, 0, 0);
}

static inline long r3_close(int fd)
{
    return __v64_syscall(R3_SYS_CLOSE, (long)fd, 0, 0, 0, 0);
}

static inline long r3_stat(const char *path, struct stat *st)
{
    return __v64_syscall(R3_SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)st, 0, 0, 0);
}

static inline long r3_mkdir(const char *path, int mode)
{
    return __v64_syscall(R3_SYS_MKDIR, (long)(uintptr_t)path, (long)mode, 0, 0, 0);
}

static inline long r3_rmdir(const char *path)
{
    return __v64_syscall(R3_SYS_RMDIR, (long)(uintptr_t)path, 0, 0, 0, 0);
}

static inline long r3_unlink(const char *path)
{
    return __v64_syscall(R3_SYS_UNLINK, (long)(uintptr_t)path, 0, 0, 0, 0);
}

static inline long r3_rename(const char *from, const char *to)
{
    return __v64_syscall(R3_SYS_RENAME, (long)(uintptr_t)from,
                         (long)(uintptr_t)to, 0, 0, 0);
}

static inline long r3_getdents64(int fd, void *buf, unsigned long len)
{
    return __v64_syscall(R3_SYS_GETDENTS64, (long)fd, (long)(uintptr_t)buf,
                         (long)len, 0, 0);
}

static inline long r3_getcwd(char *buf, unsigned long len)
{
    return __v64_syscall(R3_SYS_GETCWD, (long)(uintptr_t)buf, (long)len, 0, 0, 0);
}

/* statfs(2): SutureOS fills two 64-bit words (bytes), see sys2_statfs(). Returns
 * -ENOENT when the mount cannot report free space - the caller must print that
 * instead of inventing numbers. */
static inline long r3_statfs(const char *path, uint64_t *out_free_total)
{
    return __v64_syscall(R3_SYS_STATFS, (long)(uintptr_t)path,
                         (long)(uintptr_t)out_free_total, 0, 0, 0);
}

/* fsync(2) in this kernel flushes every mount (sys2_fsync ignores the fd), which is
 * what keeps the FAT32 FSInfo counter in step after a create/write/unlink. */
static inline long r3_sync(void)
{
    return __v64_syscall(R3_SYS_FSYNC, -1, 0, 0, 0, 0);
}

/* ---------------- helpers ---------------- */

/* errno -> name, so a failure line says something actionable. */
static inline const char *r3_errname(long e)
{
    switch (e) {
    case 1:  return "EPERM";
    case 2:  return "ENOENT";
    case 5:  return "EIO";
    case 9:  return "EBADF";
    case 12: return "ENOMEM";
    case 13: return "EACCES";
    case 16: return "EBUSY";
    case 17: return "EEXIST";
    case 18: return "EXDEV";
    case 20: return "ENOTDIR";
    case 21: return "EISDIR";
    case 22: return "EINVAL";
    case 28: return "ENOSPC";
    case 38: return "ENOSYS";
    case 39: return "ENOTEMPTY";
    default: return "errno";
    }
}

/* "tool: <what>: failed (errno=N NAME)" - every tool's failure path goes here. */
static inline void r3_fail(const char *tool, const char *what, long rc)
{
    const long e = (rc < 0) ? -rc : rc;
    printf("%s: %s: failed (errno=%ld %s)\n", tool, what, e, r3_errname(e));
}

/* dir + "/" + name, collapsing a trailing slash on dir ("/" + "x" -> "/x"). */
static inline void r3_join(char *dst, unsigned long cap, const char *dir,
                           const char *name)
{
    unsigned long n = 0;
    unsigned long i = 0;

    if (cap == 0) return;
    while (dir[i] != '\0' && n + 1 < cap) dst[n++] = dir[i++];
    if (n == 0 || dst[n - 1] != '/') {
        if (n + 1 < cap) dst[n++] = '/';
    }
    i = 0;
    while (name[i] != '\0' && n + 1 < cap) dst[n++] = name[i++];
    dst[n] = '\0';
}

/* Last '/' of a path, or NULL (user/lib/string.h has no strrchr). */
static inline const char *r3_last_slash(const char *s)
{
    const char *last = NULL;

    while (*s != '\0') {
        if (*s == '/') last = s;
        s++;
    }
    return last;
}

/* Directory part of a path ("" and "/a" -> "/", "a" -> "."). */
static inline void r3_dirname(char *dst, unsigned long cap, const char *path)
{
    const char *slash = r3_last_slash(path);
    unsigned long n;

    if (cap == 0) return;
    if (slash == NULL) {
        strcpy(dst, ".");
        return;
    }
    n = (unsigned long)(slash - path);
    if (n == 0) n = 1;                     /* "/a" -> "/" */
    if (n + 1 > cap) n = cap - 1;
    memcpy(dst, path, n);
    dst[n] = '\0';
}

#endif /* SUTUREOS_R3TOOL_H */
