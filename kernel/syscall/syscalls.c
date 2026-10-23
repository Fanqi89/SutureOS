/* SutureOS: 基础系统调用实现 */
#include <stitch/user_context.h>
#include <stitch/console.h>
#include <stitch/types.h>
#include <stitch/string.h>
#include "task.h"
#include "vfs.h"
#include "pipe.h"        /* pipe fds are not VFS fds (kernel/ipc) */
#include "el0_validate.h"
#include "mmu.h"
#include "heap.h"

/* 文件描述符表 (每任务静态) - 存储 VFS fd
 *
 * A slot names either a VFS open file (vfs_fd), a pipe end (pipe_fd) or a
 * kernel/net socket (sock_fd). Pipes normally live in their own encoded number
 * space (kernel/ipc/pipe.c, fds >= PIPE_FD_BASE), but dup()/dup2() have to hand
 * out ordinary low descriptor numbers, so a slot can also hold one of those
 * encoded pipe fds. A socket slot holds the index of the net socket object
 * (kernel/net/net.c), which is a different number space from the descriptor. */
#define MAX_FDS 32
struct fd_entry {
    int vfs_fd;   /* VFS layer file descriptor, -1 when unused */
    int pipe_fd;  /* pipe end behind this slot (dup of a pipe), -1 otherwise */
    int sock_fd;  /* kernel/net socket slot behind this fd, -1 otherwise */
    uint32_t flags;  /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
};

static struct fd_entry fd_tables[SCHED_MAX_TASKS][MAX_FDS];

/* defined below; used by fd_table_release() */
static void fd_close_slot(int tid, int fd);

/* Initialize fd tables - call from task creation */
void fd_tables_init(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        for (int j = 0; j < MAX_FDS; j++) {
            fd_tables[i][j].vfs_fd = -1;
            fd_tables[i][j].pipe_fd = -1;
            fd_tables[i][j].sock_fd = -1;
            fd_tables[i][j].flags = 0;
        }
    }
}

/* 分配空闲 fd */
static int fd_alloc(int tid) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_tables[tid][i].vfs_fd < 0 && fd_tables[tid][i].pipe_fd < 0 &&
            fd_tables[tid][i].sock_fd < 0) return i;
    }
    return -1;
}

/* Which row of fd_tables describes the caller.
 *
 * Before fork() every kernel task had its own row and that was the whole story.
 * Now a ring-3 process is not a scheduler task: the processes of one session all
 * run under the same kernel task, so indexing by sched_cpu.current would make a
 * child share its parent's descriptors (closing fd 3 in the child would close it
 * in the parent). Processes therefore use their own rows above the kernel task
 * range (process.h PROC_SLOT_BASE), and everything that touches fd_tables goes
 * through this one function. */
static int fd_slot(void)
{
    extern sched_cpu_t sched_cpu;
    extern int proc_current_slot(void);
    const int slot = proc_current_slot();

    if (slot >= 0 && slot < SCHED_MAX_TASKS) return slot;
    return sched_cpu.current;
}

/* fork(): the child gets a copy of the parent's descriptors, each holding one more
 * reference on the same open file / pipe end - exactly what dup() does, which is
 * also what fork() means in POSIX. Sockets are plain handles (see fd_dup_lowest)
 * and are copied as such. */
int fd_table_clone(int from_slot, int to_slot)
{
    if (from_slot < 0 || from_slot >= SCHED_MAX_TASKS) return -1;
    if (to_slot < 0 || to_slot >= SCHED_MAX_TASKS) return -1;
    for (int i = 0; i < MAX_FDS; i++) {
        struct fd_entry *dst = &fd_tables[to_slot][i];
        struct fd_entry *src = &fd_tables[from_slot][i];

        dst->vfs_fd = -1;
        dst->pipe_fd = -1;
        dst->sock_fd = -1;
        dst->flags = 0;
        if (src->sock_fd >= 0) {
            dst->sock_fd = src->sock_fd;
        } else if (src->pipe_fd >= 0) {
            if (pipe_ref_fd(src->pipe_fd) != 0) return -1;
            dst->pipe_fd = src->pipe_fd;
        } else if (src->vfs_fd >= 0) {
            if (vfs_ref(src->vfs_fd) != VFS_OK) return -1;
            dst->vfs_fd = src->vfs_fd;
        }
        dst->flags = src->flags;
    }
    return 0;
}

/* A reaped process drops every descriptor it still held (closing one reference of
 * each shared open file). */
void fd_table_release(int slot)
{
    if (slot < 0 || slot >= SCHED_MAX_TASKS) return;
    for (int i = 0; i < MAX_FDS; i++) fd_close_slot(slot, i);
}

/* ---------------- fd table primitives shared with syscalls2.c ---------------- */

/* Pipe end behind `fd`: an encoded pipe fd (>= PIPE_FD_BASE) is returned as it
 * is, an ordinary slot holding a duplicated pipe end resolves through the
 * table. -1 when `fd` is neither. */
int fd_pipe_end(int fd)
{
    extern sched_cpu_t sched_cpu;
    const int tid = fd_slot();

    if (pipe_is_fd(fd)) return fd;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -1;
    if (fd_tables[tid][fd].pipe_fd < 0) return -1;
    return fd_tables[tid][fd].pipe_fd;
}

/* VFS fd behind a descriptor slot, or -1 (pipe ends have none). */
int fd_vfs_fd(int fd)
{
    extern sched_cpu_t sched_cpu;
    const int tid = fd_slot();

    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -1;
    return fd_tables[tid][fd].vfs_fd;
}

/* Kernel/net socket behind a descriptor slot, or -1 (defined below). */
int fd_sock_fd(int fd);

int fd_is_open(int fd)
{
    if (fd_pipe_end(fd) >= 0) return 1;
    if (fd_vfs_fd(fd) >= 0) return 1;
    return fd_sock_fd(fd) >= 0;
}

/* Kernel/net socket behind a descriptor slot, or -1. */
int fd_sock_fd(int fd)
{
    extern sched_cpu_t sched_cpu;
    const int tid = fd_slot();

    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -1;
    return fd_tables[tid][fd].sock_fd;
}

/* Install a kernel/net socket in the lowest free descriptor slot and return that
 * descriptor, or -EMFILE when the table is full. The caller owns the net socket;
 * on failure it must net_close() it. */
int fd_install_socket(int sock)
{
    extern sched_cpu_t sched_cpu;
    const int tid = fd_slot();
    int fd;

    if (sock < 0) return -9;                                 /* -EBADF */
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -9;
    fd = fd_alloc(tid);
    if (fd < 0) return -24;                                  /* -EMFILE */
    fd_tables[tid][fd].vfs_fd = -1;
    fd_tables[tid][fd].pipe_fd = -1;
    fd_tables[tid][fd].sock_fd = sock;
    fd_tables[tid][fd].flags = 0;
    return fd;
}

/* Drop whatever a slot holds (used by dup2() before it overwrites it). */
static void fd_close_slot(int tid, int fd)
{
    extern int net_close(int fd);

    if (tid < 0 || tid >= SCHED_MAX_TASKS) return;
    if (fd < 0 || fd >= MAX_FDS) return;
    if (fd_tables[tid][fd].sock_fd >= 0) {
        (void)net_close(fd_tables[tid][fd].sock_fd);
    } else if (fd_tables[tid][fd].pipe_fd >= 0) {
        (void)pipe_close_fd(fd_tables[tid][fd].pipe_fd);
    } else if (fd_tables[tid][fd].vfs_fd >= 0) {
        (void)vfs_close(fd_tables[tid][fd].vfs_fd);
    }
    fd_tables[tid][fd].vfs_fd = -1;
    fd_tables[tid][fd].pipe_fd = -1;
    fd_tables[tid][fd].sock_fd = -1;
    fd_tables[tid][fd].flags = 0;
}

/* dup(): the lowest free descriptor naming the same open file (same offset) or
 * the same pipe end. Returns the new fd or -errno. */
int fd_dup_lowest(int oldfd)
{
    extern sched_cpu_t sched_cpu;
    const int tid = fd_slot();
    int pfd, newfd;

    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -9;       /* -EBADF */
    pfd = fd_pipe_end(oldfd);
    if (pfd >= 0) {
        newfd = fd_alloc(tid);
        if (newfd < 0) return -24;                          /* -EMFILE */
        if (pipe_ref_fd(pfd) != 0) return -9;
        fd_tables[tid][newfd].vfs_fd = -1;
        fd_tables[tid][newfd].pipe_fd = pfd;
        fd_tables[tid][newfd].sock_fd = -1;
        fd_tables[tid][newfd].flags = 0;
        return newfd;
    }
    if (oldfd < 0 || oldfd >= MAX_FDS) return -9;
    if (fd_tables[tid][oldfd].sock_fd >= 0) {
        /* A socket slot is a plain handle, not a refcounted open file: the
         * duplicate names the same net socket and the first close() retires it
         * (a second close then sees a closed slot). Documented, not pretended. */
        newfd = fd_alloc(tid);
        if (newfd < 0) return -24;
        fd_tables[tid][newfd].vfs_fd = -1;
        fd_tables[tid][newfd].pipe_fd = -1;
        fd_tables[tid][newfd].sock_fd = fd_tables[tid][oldfd].sock_fd;
        fd_tables[tid][newfd].flags = 0;
        return newfd;
    }
    if (fd_tables[tid][oldfd].vfs_fd < 0) return -9;
    newfd = fd_alloc(tid);
    if (newfd < 0) return -24;
    if (vfs_ref(fd_tables[tid][oldfd].vfs_fd) != VFS_OK) return -5;  /* -EIO */
    fd_tables[tid][newfd].vfs_fd = fd_tables[tid][oldfd].vfs_fd;
    fd_tables[tid][newfd].pipe_fd = -1;
    fd_tables[tid][newfd].sock_fd = -1;
    fd_tables[tid][newfd].flags = fd_tables[tid][oldfd].flags;
    return newfd;
}

/* dup2()/dup3(): close newfd when open, then make it a duplicate of oldfd.
 * newfd must be an ordinary descriptor slot: the encoded pipe numbers are a
 * fixed function of (pipe slot, end), so they cannot be remapped to a requested
 * number. Returns newfd or -errno. */
int fd_dup_to(int oldfd, int newfd)
{
    extern sched_cpu_t sched_cpu;
    const int tid = fd_slot();
    int pfd;

    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -9;       /* -EBADF */
    if (newfd < 0 || newfd >= MAX_FDS) return -9;
    if (oldfd == newfd) return fd_is_open(oldfd) ? newfd : -9;
    pfd = fd_pipe_end(oldfd);
    if (pfd >= 0) {
        fd_close_slot(tid, newfd);
        if (pipe_ref_fd(pfd) != 0) return -9;
        fd_tables[tid][newfd].vfs_fd = -1;
        fd_tables[tid][newfd].pipe_fd = pfd;
        fd_tables[tid][newfd].sock_fd = -1;
        fd_tables[tid][newfd].flags = 0;
        return newfd;
    }
    if (oldfd < 0 || oldfd >= MAX_FDS) return -9;
    if (fd_tables[tid][oldfd].sock_fd >= 0) {
        fd_close_slot(tid, newfd);
        fd_tables[tid][newfd].vfs_fd = -1;
        fd_tables[tid][newfd].pipe_fd = -1;
        fd_tables[tid][newfd].sock_fd = fd_tables[tid][oldfd].sock_fd;
        fd_tables[tid][newfd].flags = 0;
        return newfd;
    }
    if (fd_tables[tid][oldfd].vfs_fd < 0) return -9;
    fd_close_slot(tid, newfd);
    if (vfs_ref(fd_tables[tid][oldfd].vfs_fd) != VFS_OK) return -5;
    fd_tables[tid][newfd].vfs_fd = fd_tables[tid][oldfd].vfs_fd;
    fd_tables[tid][newfd].pipe_fd = -1;
    fd_tables[tid][newfd].sock_fd = -1;
    fd_tables[tid][newfd].flags = fd_tables[tid][oldfd].flags;
    return newfd;
}

uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = fd_slot();
    /* pipes live in their own fd space (kernel/ipc/pipe.c), and dup()/dup2() can
     * also park a pipe end in an ordinary descriptor slot */
    {
        const int pfd = fd_pipe_end((int)fd);
        if (pfd >= 0) {
            EL0_CHECK(buf, len);
            return (uint64_t)(int64_t)pipe_read_fd(pfd, (void *)(uintptr_t)buf,
                                                  (uint32_t)len);
        }
    }
    if (fd >= MAX_FDS) return (uint64_t)-1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return (uint64_t)(int64_t)(-9);
    /* A socket descriptor behaves like read(2) on a connected socket. */
    if (fd_tables[tid][fd].sock_fd >= 0) {
        extern int net_recv(int, void *, size_t, int);
        EL0_CHECK(buf, len);
        return (uint64_t)(int64_t)net_recv(fd_tables[tid][fd].sock_fd,
                                           (void *)(uintptr_t)buf, (size_t)len, 0);
    }
    struct fd_entry *f = &fd_tables[tid][fd];
    if (f->vfs_fd < 0) return (uint64_t)-1;
    EL0_CHECK(buf, len);
    int ret = vfs_read(f->vfs_fd, (void *)(uintptr_t)buf, (uint32_t)len);
    return ret;
}

uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = fd_slot();
    {
        const int pfd = fd_pipe_end((int)fd);
        if (pfd >= 0) {
            EL0_CHECK(buf, len);
            return (uint64_t)(int64_t)pipe_write_fd(pfd, (const void *)(uintptr_t)buf,
                                                   (uint32_t)len);
        }
    }
    if (fd >= MAX_FDS) return (uint64_t)-1;
    /* fd 1/2 are the console whenever they are not a real opened file (or a
     * dup'ed pipe end, which has to win: `dup2(pipe_write, 1)` is redirection).
     * This has to be decided before indexing the per-task table: the kernel
     * itself makes syscalls with sched_cpu.current == -1 (e.g. the boot-time
     * ring-3 run), and fd_tables[-1] used to read out of bounds and swallow the
     * output. */
    {
        const int valid_tid = (tid >= 0 && tid < SCHED_MAX_TASKS);
        if ((fd == 1 || fd == 2) &&
            (!valid_tid ||
             (fd_tables[tid][fd].vfs_fd < 0 && fd_tables[tid][fd].pipe_fd < 0))) {
            EL0_CHECK(buf, len);
            const char *s = (const char *)(uintptr_t)buf;
            for (uint64_t i = 0; i < len; i++) {
                if (s[i] == '\0') break;
                console_putc(s[i]);
            }
            return len;
        }
        if (!valid_tid) return (uint64_t)(int64_t)(-9);      /* -EBADF */
    }
    /* A socket descriptor behaves like write(2) on a connected socket. */
    if (fd_tables[tid][fd].sock_fd >= 0) {
        extern int net_send(int, const void *, size_t, int);
        EL0_CHECK(buf, len);
        return (uint64_t)(int64_t)net_send(fd_tables[tid][fd].sock_fd,
                                           (const void *)(uintptr_t)buf,
                                           (size_t)len, 0);
    }
    struct fd_entry *f = &fd_tables[tid][fd];
    if (f->vfs_fd < 0) return (uint64_t)-1;
    EL0_CHECK(buf, len);
    int ret = vfs_write(f->vfs_fd, (const void *)(uintptr_t)buf, (uint32_t)len);
    return ret;
}

uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = fd_slot();
    EL0_STR((const char *)(uintptr_t)path_ptr);
    const char *path = (const char *)(uintptr_t)path_ptr;
    
    int vfs_flags = VFS_O_RDONLY;
    if ((flags & 3) == 1) vfs_flags = VFS_O_WRONLY;
    else if ((flags & 3) == 2) vfs_flags = VFS_O_RDWR;
    if (flags & 0x40) vfs_flags |= VFS_O_CREAT;  /* O_CREAT */
    if (flags & 0x200) vfs_flags |= VFS_O_TRUNC; /* O_TRUNC */
    
    int vfs_fd = vfs_open(path, vfs_flags);
    if (vfs_fd < 0) return (uint64_t)-2;  /* ENOENT */
    
    int fd = fd_alloc(fd_slot());
    if (fd < 0) {
        vfs_close(vfs_fd);
        return (uint64_t)-1;  /* EMFILE */
    }
    fd_tables[tid][fd].vfs_fd = vfs_fd;
    fd_tables[tid][fd].pipe_fd = -1;
    fd_tables[tid][fd].sock_fd = -1;
    fd_tables[tid][fd].flags = flags & 3;
    return fd;
}

uint64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = fd_slot();
    {
        const int pfd = fd_pipe_end((int)fd);
        if (pfd >= 0) {
            const int r = pipe_close_fd(pfd);
            /* a slot-held pipe end (dup of a pipe) also frees its descriptor */
            if (fd < MAX_FDS && !pipe_is_fd((int)fd) &&
                tid >= 0 && tid < SCHED_MAX_TASKS) {
                fd_tables[tid][fd].vfs_fd = -1;
                fd_tables[tid][fd].pipe_fd = -1;
                fd_tables[tid][fd].sock_fd = -1;
                fd_tables[tid][fd].flags = 0;
            }
            return (uint64_t)(int64_t)r;
        }
    }
    if (fd >= MAX_FDS) return (uint64_t)-1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return (uint64_t)(int64_t)(-9);
    /* close() must retire a socket descriptor too: net_close() shuts the TCP
     * connection or drops the datagram staging slot, then the descriptor frees. */
    if (fd_tables[tid][fd].sock_fd >= 0) {
        extern int net_close(int fd);
        const int r = net_close(fd_tables[tid][fd].sock_fd);
        fd_tables[tid][fd].vfs_fd = -1;
        fd_tables[tid][fd].pipe_fd = -1;
        fd_tables[tid][fd].sock_fd = -1;
        fd_tables[tid][fd].flags = 0;
        return (uint64_t)(int64_t)r;
    }
    if (fd_tables[tid][fd].vfs_fd < 0) return (uint64_t)-1;
    int ret = vfs_close(fd_tables[tid][fd].vfs_fd);
    fd_tables[tid][fd].vfs_fd = -1;
    fd_tables[tid][fd].pipe_fd = -1;
    fd_tables[tid][fd].sock_fd = -1;
    fd_tables[tid][fd].flags = 0;
    return ret;
}

/* fork(2): the real thing, implemented in kernel/syscall/process.c.
 *
 * The old body below created a *kernel sched task* and copied cinux_cpu_context_t
 * fields, which cannot work for a ring-3 process: a task starts at
 * sched_trampoline() from nothing, while a forked child has to resume inside the
 * parent's syscall frame with rax = 0 and its own copy-on-write address space.
 * process.c owns the process table, the kernel-stack switch and the COW window.
 *
 * The parent's user_context_t comes from the `syscall` gate (syscall_current_ctx),
 * so fork() returns the child's pid here and 0 in the child without either side
 * needing a special return path. */
uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    extern user_context_t *syscall_current_ctx(void);
    extern int proc_fork(user_context_t *parent_ctx);
    user_context_t *ctx = syscall_current_ctx();

    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* fork() is only meaningful for a ring-3 process; the kernel task has no user
     * image to copy (and no frame to return through). */
    if (ctx == NULL || (ctx->cs & 3u) != 3u) return (uint64_t)(int64_t)(-38); /* -ENOSYS */

    return (uint64_t)(int64_t)proc_fork(ctx);
}

uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    extern int ring3_active(void);
    extern void ring3_exit_user(uint64_t code);
    int tid = sched_cpu.current;
    if (tid >= 0 && tid < SCHED_MAX_TASKS) {
        sched_cpu.tasks[tid].state = SCHED_TASK_ZOMBIE;
    }
    /* A ring-3 program exiting must come back to its kernel launcher, not power
     * the machine off. */
    if (ring3_active()) {
        ring3_exit_user(code);          /* does not return */
    }
    sched_cpu.shutdown = true;
    return code;
}

uint64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    extern int proc_current_pid(void);
    extern void proc_yield(void);

    /* A ring-3 process must yield to its *siblings*, not to the kernel scheduler:
     * kernel/sched/task.c holds no context for a process, and its cooperative
     * round-robin would resume a task whose stack no longer exists (the historical
     * "sched_yield jumps to 0x3" failure). */
    if (proc_current_pid() >= 0) {
        proc_yield();
        return 0;
    }
    sched_yield();
    return 0;
}

/* 简易 execve：加载 ELF 用户程序 */
uint64_t sys_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)argv_ptr; (void)envp_ptr; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    EL0_STR((const char *)(uintptr_t)path_ptr);
    const char *path = (const char *)(uintptr_t)path_ptr;
    
    /* 使用 loader 模块加载 ELF */
    extern uint64_t elf_load_user(const void *buf, uint64_t len, uint64_t *entry, uint64_t *base, uint64_t *end);
    
    /* Open file via VFS */
    int vfs_fd = vfs_open(path, VFS_O_RDONLY);
    if (vfs_fd < 0) return (uint64_t)-2;  /* ENOENT */
    
    /* Get file size via stat */
    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) {
        vfs_close(vfs_fd);
        return (uint64_t)-2;
    }
    uint64_t size = st.size;
    
    /* 读取文件到缓冲区 */
    void *buf = lemis_heap_alloc(size);
    if (!buf) {
        vfs_close(vfs_fd);
        return (uint64_t)-12;  /* ENOMEM */
    }
    
    int ret = vfs_read(vfs_fd, buf, (uint32_t)size);
    vfs_close(vfs_fd);
    if (ret < 0) {
        lemis_heap_free(buf);
        return (uint64_t)-5;  /* EIO */
    }
    
    uint64_t entry = 0, base = 0, end = 0;
    ret = elf_load_user(buf, size, &entry, &base, &end);
    lemis_heap_free(buf);
    
    if (ret < 0) return (uint64_t)ret;
    
    /* 更新当前任务上下文为新程序入口 */
    sched_task_t *t = &sched_cpu.tasks[tid];
    t->context.rip = entry;
    t->context.rsp = 0x7FFFFFFFF000;  /* 用户栈顶 (简化) */
    t->context.rflags = 0x202;
    /* cs/ss 在 cinux_cpu_context_t 中不存在，由 syscall 入口处理 */
    
    /* 设置用户页表 */
    extern void task_set_user_pml4(int, uint64_t);
    task_set_user_pml4(tid, base);
    
    return 0;
}

/* ==================== musl 专用系统调用 ==================== */

/* clock_gettime - musl 用于时间获取 */
uint64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    EL0_CHECK((void*)(uintptr_t)tp, sizeof(uint64_t)*2);  /* timespec = 2*uint64 */
    
    /* 简化实现：返回固定时间 */
    uint64_t *ts = (uint64_t*)(uintptr_t)tp;
    ts[0] = 1234567890;  /* tv_sec */
    ts[1] = 0;           /* tv_nsec */
    
    return 0;
}

/* getrandom - musl 用于随机数 */
uint64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    EL0_CHECK((void*)(uintptr_t)buf, len);
    
    /* 简化实现：填充伪随机数据 */
    uint8_t *dst = (uint8_t*)(uintptr_t)buf;
    static uint64_t seed = 0xDEADBEEFCAFEBABE;
    for (uint64_t i = 0; i < len; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        dst[i] = (uint8_t)(seed >> 56);
    }
    
    return len;
}

/* arch_prctl - musl 用于设置 FS/GS base (TLS) */
uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    
    if (code == 0x1001) {  /* ARCH_SET_FS */
        sched_cpu.tasks[tid].context.fs_base = addr;
        /* 写 MSR */
        __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)addr), "d"(addr >> 32));
        return 0;
    } else if (code == 0x1002) {  /* ARCH_SET_GS */
        sched_cpu.tasks[tid].context.gs_base = addr;
        __asm__ volatile("wrmsr" :: "c"(0xC0000101), "a"((uint32_t)addr), "d"(addr >> 32));
        return 0;
    } else if (code == 0x1003) {  /* ARCH_GET_FS */
        *(uint64_t*)(uintptr_t)addr = sched_cpu.tasks[tid].context.fs_base;
        return 0;
    } else if (code == 0x1004) {  /* ARCH_GET_GS */
        *(uint64_t*)(uintptr_t)addr = sched_cpu.tasks[tid].context.gs_base;
        return 0;
    }
    return (uint64_t)-1;  /* EINVAL */
}

/* set_tid_address - musl 用于线程清理 */
uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    
    /* 存储 tid 指针，用于退出时清理 */
    sched_cpu.tasks[tid].context.kgs_base = tidptr;  /* 复用字段 */
    return tid + 1;  /* 返回 PID */
}

/* futex - musl pthread 同步原语 */
uint64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout,
                   uint64_t uaddr2, uint64_t val3)
{
    (void)uaddr; (void)op; (void)val; (void)timeout; (void)uaddr2; (void)val3;
    /* 简化实现：总是返回成功 */
    return 0;
}

/* madvise - 内存建议 */
uint64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)addr; (void)len; (void)advice; (void)a4; (void)a5; (void)a6;
    /* 简化实现：总是返回成功 */
    return 0;
}

/* fcntl - 文件控制 */
uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)fd; (void)cmd; (void)arg; (void)a4; (void)a5; (void)a6;
    /* 简化实现：总是返回 0 */
    return 0;
}

/* exit_group - 进程组退出 */
uint64_t sys_exit_group(uint64_t code, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    sched_cpu.tasks[tid].state = SCHED_TASK_ZOMBIE;
    sched_cpu.shutdown = true;
    return code;
}
