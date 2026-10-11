/* SutureOS module: Linux syscall ABI compatibility layer
 * Provides Linux/x86_64 syscall ABI compatibility for running Linux binaries
 * Maps Linux syscall numbers and calling conventions to SutureOS native syscalls
 */
#ifndef STITCH_LINUX_COMPAT_H
#define STITCH_LINUX_COMPAT_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux x86_64 syscall numbers (from Linux kernel headers) */
#define LINUX_SYS_READ            0
#define LINUX_SYS_WRITE           1
#define LINUX_SYS_OPEN            2
#define LINUX_SYS_CLOSE           3
#define LINUX_SYS_STAT            4
#define LINUX_SYS_FSTAT           5
#define LINUX_SYS_LSTAT           6
#define LINUX_SYS_POLL            7
#define LINUX_SYS_LSEEK           8
#define LINUX_SYS_MMAP            9
#define LINUX_SYS_MPROTECT       10
#define LINUX_SYS_MUNMAP         11
#define LINUX_SYS_BRK            12
#define LINUX_SYS_RT_SIGACTION   13
#define LINUX_SYS_RT_SIGPROCMASK 14
#define LINUX_SYS_RT_SIGRETURN   15
#define LINUX_SYS_IOCTL          16
#define LINUX_SYS_PREAD64        17
#define LINUX_SYS_PWRITE64       18
#define LINUX_SYS_READV          19
#define LINUX_SYS_WRITEV         20
#define LINUX_SYS_ACCESS         21
#define LINUX_SYS_PIPE           22
#define LINUX_SYS_SELECT         23
#define LINUX_SYS_SCHED_YIELD    24
#define LINUX_SYS_MREMAP         25
#define LINUX_SYS_MSYNC          26
#define LINUX_SYS_MINCORE        27
#define LINUX_SYS_MADVISE        28
#define LINUX_SYS_SHMGET         29
#define LINUX_SYS_SHMAT          30
#define LINUX_SYS_SHMCTL         31
#define LINUX_SYS_DUP            32
#define LINUX_SYS_DUP2           33
#define LINUX_SYS_PAUSE          34
#define LINUX_SYS_NANOSLEEP      35
#define LINUX_SYS_GETITIMER      36
#define LINUX_SYS_ALARM          37
#define LINUX_SYS_SETITIMER      38
#define LINUX_SYS_GETPID         39
#define LINUX_SYS_SENDFILE       40
#define LINUX_SYS_SOCKET         41
#define LINUX_SYS_CONNECT        42
#define LINUX_SYS_ACCEPT         43
#define LINUX_SYS_SENDTO         44
#define LINUX_SYS_RECVFROM       45
#define LINUX_SYS_SENDMSG        46
#define LINUX_SYS_RECVMSG        47
#define LINUX_SYS_SHUTDOWN       48
#define LINUX_SYS_BIND           49
#define LINUX_SYS_LISTEN         50
#define LINUX_SYS_GETSOCKNAME    51
#define LINUX_SYS_GETPEERNAME    52
#define LINUX_SYS_SOCKETPAIR     53
#define LINUX_SYS_SETSOCKOPT     54
#define LINUX_SYS_GETSOCKOPT     55
#define LINUX_SYS_CLONE          56
#define LINUX_SYS_FORK           57
#define LINUX_SYS_VFORK          58
#define LINUX_SYS_EXECVE         59
#define LINUX_SYS_EXIT           60
#define LINUX_SYS_WAIT4          61
#define LINUX_SYS_KILL           62
#define LINUX_SYS_UNAME          63
#define LINUX_SYS_SEMGET         64
#define LINUX_SYS_SEMOP          65
#define LINUX_SYS_SEMCTL         66
#define LINUX_SYS_SHMDT          67
#define LINUX_SYS_MSGGET         68
#define LINUX_SYS_MSGSND         69
#define LINUX_SYS_MSGRCV         70
#define LINUX_SYS_MSGCTL         71
#define LINUX_SYS_FCNTL          72
#define LINUX_SYS_FLOCK          73
#define LINUX_SYS_FSYNC          74
#define LINUX_SYS_FDATASYNC      75
#define LINUX_SYS_TRUNCATE       76
#define LINUX_SYS_FTRUNCATE      77
#define LINUX_SYS_GETDENTS       77
#define LINUX_SYS_GETCWD         79
#define LINUX_SYS_CHDIR          80
#define LINUX_SYS_FCHDIR         81
#define LINUX_SYS_RENAME         82
#define LINUX_SYS_MKDIR          83
#define LINUX_SYS_RMDIR          84
#define LINUX_SYS_CREAT          85
#define LINUX_SYS_LINK           86
#define LINUX_SYS_UNLINK         87
#define LINUX_SYS_SYMLINK        88
#define LINUX_SYS_READLINK       89
#define LINUX_SYS_CHMOD          90
#define LINUX_SYS_FCHMOD         91
#define LINUX_SYS_CHOWN          92
#define LINUX_SYS_FCHOWN         93
#define LINUX_SYS_LCHOWN         94
#define LINUX_SYS_UMASK          95
#define LINUX_SYS_GETTIMEOFDAY   96
#define LINUX_SYS_GETRLIMIT      97
#define LINUX_SYS_GETRUSAGE      98
#define LINUX_SYS_SYSINFO        99
#define LINUX_SYS_TIMES          100
#define LINUX_SYS_PTRACE         101
#define LINUX_SYS_GETUID         102
#define LINUX_SYS_SYSLOG         103
#define LINUX_SYS_GETGID         104
#define LINUX_SYS_SETUID         105
#define LINUX_SYS_SETGID         106
#define LINUX_SYS_GETEUID        107
#define LINUX_SYS_GETEGID        108
#define LINUX_SYS_SETPGID        109
#define LINUX_SYS_GETPPID        110
#define LINUX_SYS_GETPGRP        111
#define LINUX_SYS_SETSID         112
#define LINUX_SYS_SETREUID       113
#define LINUX_SYS_SETREGID       114
#define LINUX_SYS_GETGROUPS      115
#define LINUX_SYS_SETGROUPS      116
#define LINUX_SYS_SETRESUID      117
#define LINUX_SYS_GETRESUID      118
#define LINUX_SYS_SETRESGID      119
#define LINUX_SYS_GETRESGID      120
#define LINUX_SYS_GETPGID        121
#define LINUX_SYS_SETFSUID       122
#define LINUX_SYS_SETFSGID       123
#define LINUX_SYS_GETSID         124
#define LINUX_SYS_CAPGET         125
#define LINUX_SYS_CAPSET         126
#define LINUX_SYS_RT_SIGPENDING  127
#define LINUX_SYS_RT_SIGTIMEDWAIT 128
#define LINUX_SYS_RT_SIGQUEUEINFO 129
#define LINUX_SYS_RT_SIGSUSPEND  130
#define LINUX_SYS_SIGALTSTACK    131
#define LINUX_SYS_UTIME          132
#define LINUX_SYS_MKNOD          133
#define LINUX_SYS_USELIB         134
#define LINUX_SYS_PERSONALITY    135
#define LINUX_SYS_USTAT          136
#define LINUX_SYS_STATFS         137
#define LINUX_SYS_FSTATFS        138
#define LINUX_SYS_SYSFS          139
#define LINUX_SYS_GETPRIORITY    140
#define LINUX_SYS_SETPRIORITY    141
#define LINUX_SYS_SCHED_SETPARAM 142
#define LINUX_SYS_SCHED_GETPARAM 143
#define LINUX_SYS_SCHED_SETSCHEDULER 144
#define LINUX_SYS_SCHED_GETSCHEDULER 145
#define LINUX_SYS_SCHED_GET_PRIORITY_MAX 146
#define LINUX_SYS_SCHED_GET_PRIORITY_MIN 147
#define LINUX_SYS_SCHED_RR_GET_INTERVAL 148
#define LINUX_SYS_MLOCK          149
#define LINUX_SYS_MUNLOCK        150
#define LINUX_SYS_MLOCKALL       151
#define LINUX_SYS_MUNLOCKALL     152
#define LINUX_SYS_VHANGUP        153
#define LINUX_SYS_MODIFY_LDT     154
#define LINUX_SYS_PIVOT_ROOT     155
#define LINUX_SYS__SYSCTL        156
#define LINUX_SYS_PRCTL          157
#define LINUX_SYS_ARCH_PRCTL     158
#define LINUX_SYS_ADJTIMEX       159
#define LINUX_SYS_SETRLIMIT      160
#define LINUX_SYS_CHROOT         161
#define LINUX_SYS_SYNC           162
#define LINUX_SYS_ACCT           163
#define LINUX_SYS_SETTIMEOFDAY   164
#define LINUX_SYS_MOUNT          165
#define LINUX_SYS_UMOUNT2        166
#define LINUX_SYS_SWAPON         167
#define LINUX_SYS_SWAPOFF        168
#define LINUX_SYS_REBOOT         169
#define LINUX_SYS_SETHOSTNAME    170
#define LINUX_SYS_SETDOMAINNAME  171
#define LINUX_SYS_IOPL           172
#define LINUX_SYS_IOPERM         173
#define LINUX_SYS_CREATE_MODULE  174
#define LINUX_SYS_INIT_MODULE    175
#define LINUX_SYS_DELETE_MODULE  176
#define LINUX_SYS_GET_KERNEL_SYMS 177
#define LINUX_SYS_QUERY_MODULE   178
#define LINUX_SYS_QUOTACTL       179
#define LINUX_SYS_NFSSERVCTL     180
#define LINUX_SYS_GETPMSG        181
#define LINUX_SYS_PUTPMSG        182
#define LINUX_SYS_AFS_SYSCALL    183
#define LINUX_SYS_TUXCALL        184
#define LINUX_SYS_SECURITY       185
#define LINUX_SYS_GETTID         186
#define LINUX_SYS_READAHEAD      187
#define LINUX_SYS_SETXATTR       188
#define LINUX_SYS_LSETXATTR      189
#define LINUX_SYS_FSETXATTR      190
#define LINUX_SYS_GETXATTR       191
#define LINUX_SYS_LGETXATTR      192
#define LINUX_SYS_FGETXATTR      193
#define LINUX_SYS_LISTXATTR      194
#define LINUX_SYS_LLISTXATTR     195
#define LINUX_SYS_FLISTXATTR     196
#define LINUX_SYS_REMOVEXATTR    197
#define LINUX_SYS_LREMOVEXATTR   198
#define LINUX_SYS_FREMOVEXATTR   199
#define LINUX_SYS_TKILL          200
#define LINUX_SYS_TIME           201
#define LINUX_SYS_FUTEX          202
#define LINUX_SYS_SCHED_SETAFFINITY 203
#define LINUX_SYS_SCHED_GETAFFINITY 204
#define LINUX_SYS_SET_THREAD_AREA 205
#define LINUX_SYS_IO_SETUP       206
#define LINUX_SYS_IO_DESTROY     207
#define LINUX_SYS_IO_GETEVENTS   208
#define LINUX_SYS_IO_SUBMIT      209
#define LINUX_SYS_IO_CANCEL      210
#define LINUX_SYS_GET_THREAD_AREA 211
#define LINUX_SYS_LOOKUP_DCOOKIE 212
#define LINUX_SYS_EPOLL_CREATE   213
#define LINUX_SYS_EPOLL_CTL      214
#define LINUX_SYS_EPOLL_WAIT     215
#define LINUX_SYS_REMAP_FILE_PAGES 216
#define LINUX_SYS_GETDENTS64     217
#define LINUX_SYS_SET_TID_ADDRESS 218
#define LINUX_SYS_RESTART_SYSCALL 219
#define LINUX_SYS_SEMTIMEDOP     220
#define LINUX_SYS_FADVISE64      221
#define LINUX_SYS_TIMER_CREATE   222
#define LINUX_SYS_TIMER_SETTIME  223
#define LINUX_SYS_TIMER_GETTIME  224
#define LINUX_SYS_TIMER_GETOVERRUN 225
#define LINUX_SYS_TIMER_DELETE   226
#define LINUX_SYS_CLOCK_SETTIME  227
#define LINUX_SYS_CLOCK_GETTIME  228
#define LINUX_SYS_CLOCK_GETRES   229
#define LINUX_SYS_CLOCK_NANOSLEEP 230
#define LINUX_SYS_EXIT_GROUP     231
#define LINUX_SYS_EPOLL_PWAIT    232
#define LINUX_SYS_EPOLL_CREATE1  233
#define LINUX_SYS_DUP3           243
#define LINUX_SYS_PIPE2          293
#define LINUX_SYS_INOTIFY_INIT1  294
#define LINUX_SYS_PREADV         295
#define LINUX_SYS_PWRITEV        296
#define LINUX_SYS_RT_TGSIGQUEUEINFO 297
#define LINUX_SYS_PERF_EVENT_OPEN 298
#define LINUX_SYS_RECVMMSG       299
#define LINUX_SYS_FANOTIFY_INIT  300
#define LINUX_SYS_FANOTIFY_MARK  301
#define LINUX_SYS_PRLIMIT64      302
#define LINUX_SYS_NAME_TO_HANDLE_AT 303
#define LINUX_SYS_OPEN_BY_HANDLE_AT 304
#define LINUX_SYS_CLOCK_ADJTIME  305
#define LINUX_SYS_SYNCFS         306
#define LINUX_SYS_SENDMMSG       307
#define LINUX_SYS_SETNS          308
#define LINUX_SYS_GETCPU         309
#define LINUX_SYS_PROCESS_VM_READV 310
#define LINUX_SYS_PROCESS_VM_WRITEV 311
#define LINUX_SYS_KCMP           312
#define LINUX_SYS_FINIT_MODULE   313
#define LINUX_SYS_SCHED_SETATTR  314
#define LINUX_SYS_SCHED_GETATTR  315
#define LINUX_SYS_RENAMEAT2      316
#define LINUX_SYS_SECCOMP        317
#define LINUX_SYS_GETRANDOM      318
#define LINUX_SYS_MEMFD_CREATE   319
#define LINUX_SYS_KEXEC_FILE_LOAD 320
#define LINUX_SYS_BPF            321
#define LINUX_SYS_EXECVEAT       322
#define LINUX_SYS_USERFAULTFD    323
#define LINUX_SYS_MEMBARRIER     324
#define LINUX_SYS_MLOCK2         325
#define LINUX_SYS_COPY_FILE_RANGE 326
#define LINUX_SYS_PREADV2        327
#define LINUX_SYS_PWRITEV2       328
#define LINUX_SYS_PKEY_MKEY_GET  329
#define LINUX_SYS_PKEY_ALLOC     330
#define LINUX_SYS_PKEY_FREE      331
#define LINUX_SYS_STATX          332
#define LINUX_SYS_IO_PGETEVENTS  333
#define LINUX_SYS_RSEQ           334

/* Linux x86_64 syscall calling convention:
 * rax = syscall number
 * rdi, rsi, rdx, r10, r8, r9 = arguments 1-6
 * return value in rax
 * Negative return = -errno
 */

/* Linux error codes (match SutureOS errno) */
#define LINUX_EPERM            1
#define LINUX_ENOENT           2
#define LINUX_ESRCH            3
#define LINUX_EINTR            4
#define LINUX_EIO              5
#define LINUX_ENXIO            6
#define LINUX_E2BIG            7
#define LINUX_ENOEXEC          8
#define LINUX_EBADF            9
#define LINUX_ECHILD          10
#define LINUX_EAGAIN          11
#define LINUX_ENOMEM          12
#define LINUX_EACCES          13
#define LINUX_EFAULT          14
#define LINUX_ENOTBLK         15
#define LINUX_EBUSY           16
#define LINUX_EEXIST          17
#define LINUX_EXDEV           18
#define LINUX_ENODEV          19
#define LINUX_ENOTDIR         20
#define LINUX_EISDIR          21
#define LINUX_EINVAL          22
#define LINUX_ENFILE          23
#define LINUX_EMFILE          24
#define LINUX_ENOTTY          25
#define LINUX_ETXTBSY         26
#define LINUX_EFBIG           27
#define LINUX_ENOSPC          28
#define LINUX_ESPIPE          29
#define LINUX_EROFS           30
#define LINUX_EMLINK          31
#define LINUX_EPIPE           32
#define LINUX_EDOM            33
#define LINUX_ERANGE          34

/* Linux stat structure */
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atimensec;
    uint64_t st_mtime;
    uint64_t st_mtimensec;
    uint64_t st_ctime;
    uint64_t st_ctimensec;
    int64_t  __unused[3];
};

/* Linux utsname structure */
struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* Linux rlimit structure */
struct linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* Linux timeval structure */
struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* Linux timespec structure */
struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* Linux rusage structure */
struct linux_rusage {
    struct linux_timeval ru_utime;
    struct linux_timeval ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

/* Initialize Linux compatibility layer */
int linux_compat_init(void);

/* Translate Linux syscall number to SutureOS syscall number */
int linux_to_suture_syscall(int linux_nr);

/* Translate Linux errno to SutureOS errno */
int linux_to_suture_errno(int linux_errno);

/* Translate SutureOS errno to Linux errno */
int suture_to_linux_errno(int suture_errno);

/* Convert Linux stat to SutureOS stat */
int linux_stat_to_suture(const void* linux_stat, void* suture_stat);

/* Convert SutureOS utsname to Linux utsname */
void suture_utsname_to_linux(const void* suture_utsname, void* linux_utsname);

/* Linux syscall entry point (called from assembly) */
uint64_t linux_syscall_entry(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6);

/* Register Linux syscall handler */
int linux_register_syscall(int linux_nr, uint64_t (*fn)(uint64_t, uint64_t, uint64_t,
                                                        uint64_t, uint64_t, uint64_t));

#ifdef __cplusplus
}
#endif

#endif /* STITCH_LINUX_COMPAT_H */