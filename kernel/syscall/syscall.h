/* SutureOS: System call subsystem header */
#ifndef STITCH_SYSCALL_H
#define STITCH_SYSCALL_H

#include <stitch/types.h>

void syscall_init(void);
void syscall_register(uint64_t nr, uint64_t (*fn)(uint64_t, uint64_t, uint64_t,
                                                 uint64_t, uint64_t, uint64_t));

/* Direct dispatch with explicit arguments (used by the Linux ABI layer) */
uint64_t syscall_invoke(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
int syscall_is_registered(uint64_t nr);

/* ==================== per-task fd table primitives ====================
 * The table itself lives in syscalls.c next to read/write/open/close. dup(),
 * dup2() and dup3() operate on it, and the *at family needs to tell a VFS fd
 * from a pipe end and to learn which VFS fd a duck-typed fd stands for. */
int fd_dup_lowest(int oldfd);          /* new lowest free fd, or -errno */
int fd_dup_to(int oldfd, int newfd);   /* install the duplicate at newfd, or -errno */
int fd_pipe_end(int fd);               /* pipe end behind fd, or -1 */
int fd_is_open(int fd);                /* 1 when fd names a file, pipe end or socket */
int fd_vfs_fd(int fd);                 /* VFS fd behind fd, or -1 */
int fd_sock_fd(int fd);                /* kernel/net socket behind fd, or -1 */
int fd_install_socket(int sock);       /* give a net socket its lowest free fd */

/* fork()/exit() need the whole table, not one descriptor: a child inherits every
 * open file (one more reference each, exactly like dup()), and a reaped process
 * drops every descriptor it still held. The slot is an index into the same array
 * (process slots live above the kernel task slots, see process.h PROC_SLOT_BASE). */
int  fd_table_clone(int from_slot, int to_slot);   /* 0, or -1 on a full table */
void fd_table_release(int slot);                   /* close every open descriptor */

/* syscall numbers - Linux/x86_64 兼容 */
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_LSTAT           6
#define SYS_POLL            7
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT       10
#define SYS_MUNMAP         11
#define SYS_BRK            12
#define SYS_RT_SIGACTION   13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN   15
#define SYS_IOCTL          16
#define SYS_PREAD64        17
#define SYS_PWRITE64       18
#define SYS_READV          19
#define SYS_WRITEV         20
#define SYS_ACCESS         21
#define SYS_PIPE           22
#define SYS_SELECT         23
#define SYS_YIELD          24
#define SYS_MREMAP         25
#define SYS_MSYNC          26
#define SYS_MINCORE        27
#define SYS_MADVISE        28
#define SYS_SHMGET         29
#define SYS_SHMAT          30
#define SYS_SHMCTL         31
#define SYS_DUP            32
#define SYS_DUP2           33
#define SYS_PAUSE          34
#define SYS_NANOSLEEP      35
#define SYS_GETITIMER      36
#define SYS_ALARM          37
#define SYS_SETITIMER      38
#define SYS_GETPID         39
#define SYS_SENDFILE       40
#define SYS_SOCKET         41
#define SYS_CONNECT        42
#define SYS_ACCEPT         43
#define SYS_SENDTO         44
#define SYS_RECVFROM       45
#define SYS_SENDMSG        46
#define SYS_RECVMSG        47
#define SYS_SHUTDOWN       48
#define SYS_BIND           49
#define SYS_LISTEN         50
#define SYS_GETSOCKNAME    51
#define SYS_GETPEERNAME    52
#define SYS_SOCKETPAIR     53
#define SYS_SETSOCKOPT     54
#define SYS_GETSOCKOPT     55
#define SYS_CLONE          56
#define SYS_FORK           57
#define SYS_VFORK          58
#define SYS_EXECVE         59
#define SYS_EXIT           60
#define SYS_WAIT4          61
#define SYS_KILL           62
#define SYS_UNAME          63
#define SYS_SEMGET         64
#define SYS_SEMOP          65
#define SYS_SEMCTL         66
#define SYS_SHMDT          67
#define SYS_MSGGET         68
#define SYS_MSGSND         69
#define SYS_MSGRCV         70
#define SYS_MSGCTL         71
#define SYS_FCNTL          72
#define SYS_FLOCK          73
#define SYS_FSYNC          74
#define SYS_FDATASYNC      75
#define SYS_TRUNCATE       76
#define SYS_FTRUNCATE      77
#define SYS_GETDENTS       78
#define SYS_GETCWD         79
#define SYS_CHDIR          80
#define SYS_FCHDIR         81
#define SYS_RENAME         82
#define SYS_MKDIR          83
#define SYS_RMDIR          84
#define SYS_CREAT          85
#define SYS_LINK           86
#define SYS_UNLINK         87
#define SYS_SYMLINK        88
#define SYS_READLINK       89
#define SYS_CHMOD          90
#define SYS_FCHMOD         91
#define SYS_CHOWN          92
#define SYS_FCHOWN         93
#define SYS_LCHOWN         94
#define SYS_UMASK          95
#define SYS_GETTIMEOFDAY   96
#define SYS_GETRLIMIT      97
#define SYS_GETRUSAGE      98
#define SYS_SYSINFO        99
#define SYS_TIMES          100
#define SYS_PTRACE         101
#define SYS_GETUID         102
#define SYS_SYSLOG         103
#define SYS_GETGID         104
#define SYS_SETUID         105
#define SYS_SETGID         106
#define SYS_GETEUID        107
#define SYS_GETEGID        108
#define SYS_SETPGID        109
#define SYS_GETPPID        110
#define SYS_GETPGRP        111
#define SYS_SETSID         112
#define SYS_SETREUID       113
#define SYS_SETREGID       114
#define SYS_GETGROUPS      115
#define SYS_SETGROUPS      116
#define SYS_SETRESUID      117
#define SYS_GETRESUID      118
#define SYS_SETRESGID      119
#define SYS_GETRESGID      120
#define SYS_GETPGID        121
#define SYS_SETFSUID       122
#define SYS_SETFSGID       123
#define SYS_GETSID         124
#define SYS_CAPGET         125
#define SYS_CAPSET         126
#define SYS_RT_SIGPENDING  127
#define SYS_RT_SIGTIMEDWAIT 128
#define SYS_RT_SIGQUEUEINFO 129
#define SYS_RT_SIGSUSPEND  130
#define SYS_SIGALTSTACK    131
#define SYS_UTIME          132
#define SYS_MKNOD          133
#define SYS_USELIB         134
#define SYS_PERSONALITY    135
#define SYS_USTAT          136
#define SYS_STATFS         137
#define SYS_FSTATFS        138
#define SYS_SYSFS          139
#define SYS_GETPRIORITY    140
#define SYS_SETPRIORITY    141
#define SYS_SCHED_SETPARAM 142
#define SYS_SCHED_GETPARAM 143
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_SCHED_GET_PRIORITY_MAX 146
#define SYS_SCHED_GET_PRIORITY_MIN 147
#define SYS_SCHED_RR_GET_INTERVAL 148
#define SYS_MLOCK          149
#define SYS_MUNLOCK        150
#define SYS_MLOCKALL       151
#define SYS_MUNLOCKALL     152
#define SYS_VHANGUP        153
#define SYS_MODIFY_LDT     154
#define SYS_PIVOT_ROOT     155
#define SYS__SYSCTL        156
#define SYS_PRCTL          157
#define SYS_ARCH_PRCTL     158
#define SYS_ADJTIMEX       159
#define SYS_SETRLIMIT      160
#define SYS_CHROOT         161
#define SYS_SYNC           162
#define SYS_ACCT           163
#define SYS_SETTIMEOFDAY   164
#define SYS_MOUNT          165
#define SYS_UMOUNT2        166
#define SYS_SWAPON         167
#define SYS_SWAPOFF        168
#define SYS_REBOOT         169
#define SYS_SETHOSTNAME    170
#define SYS_SETDOMAINNAME  171
#define SYS_IOPL           172
#define SYS_IOPERM         173
#define SYS_CREATE_MODULE  174
#define SYS_INIT_MODULE    175
#define SYS_DELETE_MODULE  176
#define SYS_GET_KERNEL_SYMS 177
#define SYS_QUERY_MODULE   178
#define SYS_QUOTACTL       179
#define SYS_NFSSERVCTL     180
#define SYS_GETPMSG        181
#define SYS_PUTPMSG        182
#define SYS_AFS_SYSCALL    183
#define SYS_TUXCALL        184
#define SYS_SECURITY       185
#define SYS_GETTID         186
#define SYS_READAHEAD      187
#define SYS_SETXATTR       188
#define SYS_LSETXATTR      189
#define SYS_FSETXATTR      190
#define SYS_GETXATTR       191
#define SYS_LGETXATTR      192
#define SYS_FGETXATTR      193
#define SYS_LISTXATTR      194
#define SYS_LLISTXATTR     195
#define SYS_FLISTXATTR     196
#define SYS_REMOVEXATTR    197
#define SYS_LREMOVEXATTR   198
#define SYS_FREMOVEXATTR   199
#define SYS_TKILL          200
#define SYS_TIME           201
#define SYS_FUTEX          202
#define SYS_SCHED_SETAFFINITY 203
#define SYS_SCHED_GETAFFINITY 204
#define SYS_SET_THREAD_AREA 205
#define SYS_IO_SETUP       206
#define SYS_IO_DESTROY     207
#define SYS_IO_GETEVENTS   208
#define SYS_IO_SUBMIT      209
#define SYS_IO_CANCEL      210
#define SYS_GET_THREAD_AREA 211
#define SYS_LOOKUP_DCOOKIE 212
#define SYS_EPOLL_CREATE   213
#define SYS_EPOLL_CTL      214
#define SYS_EPOLL_WAIT     215
#define SYS_REMAP_FILE_PAGES 216
#define SYS_GETDENTS64     217
#define SYS_SET_TID_ADDRESS 218
#define SYS_RESTART_SYSCALL 219
#define SYS_SEMTIMEDOP     220
#define SYS_FADVISE64      221
#define SYS_TIMER_CREATE   222
#define SYS_TIMER_SETTIME  223
#define SYS_TIMER_GETTIME  224
#define SYS_TIMER_GETOVERRUN 225
#define SYS_TIMER_DELETE   226
#define SYS_CLOCK_SETTIME  227
#define SYS_CLOCK_GETTIME  228
#define SYS_CLOCK_GETRES   229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP     231
#define SYS_EPOLL_PWAIT    232
#define SYS_EPOLL_CREATE1  233
/* *at family (Linux x86-64): 234..256 are unused numbers there, the directory
 * variants start at 257. SYS_FSTATAT is Linux' newfstatat. */
#define SYS_OPENAT         257
#define SYS_MKDIRAT        258
#define SYS_FSTATAT        262
#define SYS_UNLINKAT       263
#define SYS_RENAMEAT       264
#define SYS_DUP3           292     /* was 243, which is not dup3 on Linux x86-64 */
#define SYS_PIPE2          293
#define SYS_INOTIFY_INIT1  294
#define SYS_PREADV         295
#define SYS_PWRITEV        296
#define SYS_RT_TGSIGQUEUEINFO 297
#define SYS_PERF_EVENT_OPEN 298
#define SYS_RECVMMSG       299
#define SYS_FANOTIFY_INIT  300
#define SYS_FANOTIFY_MARK  301
#define SYS_PRLIMIT64      302
#define SYS_NAME_TO_HANDLE_AT 303
#define SYS_OPEN_BY_HANDLE_AT 304
#define SYS_CLOCK_ADJTIME  305
#define SYS_SYNCFS         306
#define SYS_SENDMMSG       307
#define SYS_SETNS          308
#define SYS_GETCPU         309
#define SYS_PROCESS_VM_READV 310
#define SYS_PROCESS_VM_WRITEV 311
#define SYS_KCMP           312
#define SYS_FINIT_MODULE   313
#define SYS_SCHED_SETATTR  314
#define SYS_SCHED_GETATTR  315
#define SYS_RENAMEAT2      316
#define SYS_SECCOMP        317
#define SYS_GETRANDOM      318
#define SYS_MEMFD_CREATE   319
#define SYS_KEXEC_FILE_LOAD 320
#define SYS_BPF            321
#define SYS_EXECVEAT       322
#define SYS_USERFAULTFD    323
#define SYS_MEMBARRIER     324
#define SYS_MLOCK2         325
#define SYS_COPY_FILE_RANGE 326
#define SYS_PREADV2        327
#define SYS_PWRITEV2       328
#define SYS_PKEY_MKEY_GET  329
#define SYS_PKEY_ALLOC     330
#define SYS_PKEY_FREE      331
#define SYS_STATX          332
#define SYS_IO_PGETEVENTS  333
#define SYS_RSEQ           334

/* musl 特有：__NR_clock_gettime=228, __NR_getrandom=318, __NR_futex=202,
   __NR_set_tid_address=218, __NR_arch_prctl=158, __NR_fcntl=72,
   __NR_madvise=28, __NR_rt_sigaction=13, __NR_rt_sigreturn=15 */

#endif