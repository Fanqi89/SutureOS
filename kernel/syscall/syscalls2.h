/* SutureOS: the second half of the system call surface.
 *
 * The audit found only 22 syscalls registered (read/write/open/close/brk/mmap/
 * munmap, rt_sigaction, fork, execve, exit, sched_yield, clock_gettime,
 * getrandom, arch_prctl, set_tid_address, futex, madvise, fcntl,
 * exit_group), so nothing resembling a libc could run: no stat, no lseek, no dup, no pipe, no getpid, no chdir, no
 * mkdir/unlink. This file implements the rest of the POSIX subset that does not
 * need userland, on top of the VFS / credential / pipe / RTC layers that already
 * exist. Numbers are Linux x86-64 (see syscall.h).
 *
 * Deliberately NOT implemented (they return -ENOSYS and are listed in the boot
 * log): semaphores, message queues, shared memory, epoll, io_uring, netlink,
 * ptrace, modules, xattr, quotas, keyrings. Those belong to a later batch; giving
 * them a fake success would be worse than saying no.
 */
#ifndef SUTUREOS_SYSCALLS2_H
#define SUTUREOS_SYSCALLS2_H

#include <stitch/types.h>

/* Register every syscall implemented in syscalls2.c. Called from kmain after the
 * VFS, credentials and RTC are up. Returns the number registered. */
int syscalls2_register(void);

/* Linux x86-64 struct stat, byte-exact (144 bytes) so a real libc can use it. */
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    uint64_t st_atime_nsec;
    int64_t  st_mtime;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

/* Acceptance: exercises every syscall added here through syscall_invoke(), the
 * same path a user program takes, and prints "systest: ... PASS/FAIL" lines.
 * Returns 0 when all checks passed. Run from the shell as `systest`. */
int syscalls2_selftest(void);

/* Count of syscalls currently registered (for `systest`/boot log). */
int syscalls_registered_count(void);

#endif /* SUTUREOS_SYSCALLS2_H */
