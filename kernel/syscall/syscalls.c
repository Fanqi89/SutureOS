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

/* 文件描述符表 (每任务静态) - 存储 VFS fd */
#define MAX_FDS 32
struct fd_entry {
    int vfs_fd;  /* VFS layer file descriptor */
    uint32_t flags;  /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
};

static struct fd_entry fd_tables[SCHED_MAX_TASKS][MAX_FDS];

/* Initialize fd tables - call from task creation */
void fd_tables_init(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        for (int j = 0; j < MAX_FDS; j++) {
            fd_tables[i][j].vfs_fd = -1;
            fd_tables[i][j].flags = 0;
        }
    }
}

/* 分配空闲 fd */
static int fd_alloc(int tid) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_tables[tid][i].vfs_fd < 0) return i;
    }
    return -1;
}

uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    /* pipes live in their own fd space (kernel/ipc/pipe.c) */
    if (pipe_is_fd((int)fd)) {
        EL0_CHECK(buf, len);
        return (uint64_t)(int64_t)pipe_read_fd((int)fd, (void *)(uintptr_t)buf,
                                              (uint32_t)len);
    }
    if (fd >= MAX_FDS) return (uint64_t)-1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return (uint64_t)(int64_t)(-9);
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
    int tid = sched_cpu.current;
    if (pipe_is_fd((int)fd)) {
        EL0_CHECK(buf, len);
        return (uint64_t)(int64_t)pipe_write_fd((int)fd, (const void *)(uintptr_t)buf,
                                               (uint32_t)len);
    }
    if (fd >= MAX_FDS) return (uint64_t)-1;
    /* fd 1/2 are the console whenever they are not a real opened file. This has
     * to be decided before indexing the per-task table: the kernel itself makes
     * syscalls with sched_cpu.current == -1 (e.g. the boot-time ring-3 run), and
     * fd_tables[-1] used to read out of bounds and swallow the output. */
    {
        const int valid_tid = (tid >= 0 && tid < SCHED_MAX_TASKS);
        if ((fd == 1 || fd == 2) &&
            (!valid_tid || fd_tables[tid][fd].vfs_fd < 0)) {
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
    int tid = sched_cpu.current;
    EL0_STR((const char *)(uintptr_t)path_ptr);
    const char *path = (const char *)(uintptr_t)path_ptr;
    
    int vfs_flags = VFS_O_RDONLY;
    if ((flags & 3) == 1) vfs_flags = VFS_O_WRONLY;
    else if ((flags & 3) == 2) vfs_flags = VFS_O_RDWR;
    if (flags & 0x40) vfs_flags |= VFS_O_CREAT;  /* O_CREAT */
    if (flags & 0x200) vfs_flags |= VFS_O_TRUNC; /* O_TRUNC */
    
    int vfs_fd = vfs_open(path, vfs_flags);
    if (vfs_fd < 0) return (uint64_t)-2;  /* ENOENT */
    
    int fd = fd_alloc(sched_cpu.current);
    if (fd < 0) {
        vfs_close(vfs_fd);
        return (uint64_t)-1;  /* EMFILE */
    }
    fd_tables[tid][fd].vfs_fd = vfs_fd;
    fd_tables[tid][fd].flags = flags & 3;
    return fd;
}

uint64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    if (pipe_is_fd((int)fd)) return (uint64_t)(int64_t)pipe_close_fd((int)fd);
    if (fd >= MAX_FDS) return (uint64_t)-1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return (uint64_t)(int64_t)(-9);
    if (fd_tables[tid][fd].vfs_fd < 0) return (uint64_t)-1;
    int ret = vfs_close(fd_tables[tid][fd].vfs_fd);
    fd_tables[tid][fd].vfs_fd = -1;
    fd_tables[tid][fd].flags = 0;
    return ret;
}

/* 简易 fork：复制页表、上下文、fd 表，实现 COW */
uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int parent = sched_cpu.current;
    int child = sched_create(NULL, NULL);  /* 稍后填充上下文 */
    if (child < 0) return (uint64_t)-1;

    /* 复制用户上下文 (父进程返回子 PID，子进程返回 0) */
    sched_task_t *pt = &sched_cpu.tasks[parent];
    sched_task_t *ct = &sched_cpu.tasks[child];
    cinux_cpu_context_t *pctx = &pt->context;
    cinux_cpu_context_t *cctx = &ct->context;
    cctx->r15 = pctx->r15;
    cctx->r14 = pctx->r14;
    cctx->r13 = pctx->r13;
    cctx->r12 = pctx->r12;
    cctx->rbp = pctx->rbp;
    cctx->rbx = pctx->rbx;
    cctx->rsp = pctx->rsp;
    cctx->rip = pctx->rip;
    cctx->rflags = pctx->rflags;
    cctx->kgs_base = pctx->kgs_base;
    cctx->fs_base = pctx->fs_base;
    cctx->_pad = pctx->_pad;
    /* 子进程返回 0 - 通过 user_context_t 的 rax 在 syscall 返回时设置 */

    /* 复制 fd 表 */
    memcpy(&fd_tables[child], &fd_tables[parent], sizeof(fd_tables[0]));

    /* 复制页表并建立 COW 映射 */
    extern void task_set_user_pml4(int, uint64_t);
    uint64_t child_pml4;
    if (mmu_fork_copy(read_cr3(), &child_pml4) == 0) {
        task_set_user_pml4(child, child_pml4);
    } else {
        task_set_user_pml4(child, pt->context.kgs_base);  /* 回退：共享内核页表 */
    }

    /* 标记父进程用户页面为 COW */
    mmu_mark_user_pages_cow(read_cr3());

    return child;  /* 父进程返回子 PID */
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