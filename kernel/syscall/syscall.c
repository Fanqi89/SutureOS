/* SutureOS: 系统调用分发器 + MSR 初始化 */
#include <stitch/user_context.h>
#include <stitch/console.h>
#include <stitch/types.h>

#define SYSCALL_MAX 512
static syscall_fn_t syscall_table[SYSCALL_MAX];

/* 从汇编调用：rdi 指向 user_context_t（内核栈上） */
uint64_t syscall_dispatch_c(user_context_t *ctx)
{
    uint64_t nr = ctx->rax;
    if (nr >= SYSCALL_MAX || syscall_table[nr] == NULL) {
        console_printf("[syscall] bad nr=%lu\n", nr);
        return (uint64_t)-1;
    }
    return syscall_table[nr](ctx->rdi, ctx->rsi, ctx->rdx,
                             ctx->r10, ctx->r8, ctx->r9);
}

void syscall_register(uint64_t nr, syscall_fn_t fn)
{
    if (nr < SYSCALL_MAX) syscall_table[nr] = fn;
}

/* Direct dispatch by syscall number with explicit arguments (also used by
 * the Linux ABI compatibility layer, which cannot fabricate a
 * user_context_t on the ring-3 stack). */
uint64_t syscall_invoke(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (nr >= SYSCALL_MAX || syscall_table[nr] == NULL) {
        return (uint64_t)-1;
    }
    return syscall_table[nr](a1, a2, a3, a4, a5, a6);
}

int syscall_is_registered(uint64_t nr)
{
    return nr < SYSCALL_MAX && syscall_table[nr] != NULL;
}

/* 初始化 MSR：IA32_LSTAR, IA32_STAR, IA32_FMASK */
void syscall_init(void)
{
    extern void syscall_entry(void);
    uint64_t entry = (uint64_t)(uintptr_t)syscall_entry;

    /* STAR: bits 63:48 = sysret CS|3 (用户代码段 0x23)
     *       bits 47:32 = syscall SS (内核数据段 0x10) */
    uint64_t star = ((uint64_t)0x23ull << 48) | ((uint64_t)0x10ull << 32);

    __asm__ volatile("wrmsr" :: "c"(0xC0000082), "a"((uint32_t)entry),
                     "d"(entry >> 32));           /* IA32_LSTAR */
    __asm__ volatile("wrmsr" :: "c"(0xC0000081), "a"((uint32_t)star),
                     "d"(star >> 32));            /* IA32_STAR */
    __asm__ volatile("wrmsr" :: "c"(0xC0000084), "a"(0x200), "d"(0)); /* IA32_FMASK: 清 IF */

    /* 注册基础系统调用 */
    extern uint64_t sys_read(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_write(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_open(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_close(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_fork(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_exit(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_yield(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_execve(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_rt_sigaction(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_brk(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_mmap(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_munmap(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t);

    syscall_register(0, sys_read);    /* read */
    syscall_register(1, sys_write);   /* write */
    syscall_register(2, sys_open);    /* open */
    syscall_register(3, sys_close);   /* close */
    syscall_register(12, sys_brk);    /* brk */
    syscall_register(9, sys_mmap);    /* mmap */
    syscall_register(11, sys_munmap); /* munmap */
    syscall_register(13, sys_rt_sigaction);  /* rt_sigaction */
    syscall_register(15, sys_rt_sigreturn);  /* rt_sigreturn */
    syscall_register(57, sys_fork);   /* fork */
    syscall_register(59, sys_execve); /* execve */
    syscall_register(60, sys_exit);   /* exit */
    syscall_register(24, sys_yield);  /* sched_yield */

    /* 注册 musl 专用系统调用 */
    extern uint64_t sys_clock_gettime(uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_getrandom(uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_arch_prctl(uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_set_tid_address(uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_futex(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t);
    extern uint64_t sys_madvise(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_fcntl(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
    extern uint64_t sys_exit_group(uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);

    syscall_register(228, sys_clock_gettime);  /* clock_gettime */
    syscall_register(318, sys_getrandom);      /* getrandom */
    syscall_register(158, sys_arch_prctl);     /* arch_prctl */
    syscall_register(218, sys_set_tid_address); /* set_tid_address */
    syscall_register(202, sys_futex);          /* futex */
    syscall_register(28, sys_madvise);         /* madvise */
    syscall_register(72, sys_fcntl);           /* fcntl */
    syscall_register(231, sys_exit_group);     /* exit_group */

    console_puts("[syscall] initialized\n");
}