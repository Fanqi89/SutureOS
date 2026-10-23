/* SutureOS: 系统调用分发器 + MSR 初始化 */
#include <stitch/user_context.h>
#include <stitch/console.h>
#include <stitch/types.h>
#include "syscall.h"        /* SYS_* numbers (execve special case) */

#define SYSCALL_MAX 512
static syscall_fn_t syscall_table[SYSCALL_MAX];

/* 从汇编调用：rdi 指向 user_context_t（内核栈上） */
uint64_t syscall_dispatch_c(user_context_t *ctx)
{
    uint64_t nr = ctx->rax;
    uint64_t ret;

    if (nr >= SYSCALL_MAX || syscall_table[nr] == NULL) {
        console_printf("[syscall] bad nr=%lu\n", nr);
        return (uint64_t)-1;
    }
    /* execve is special: it replaces the caller's image and rewrites the return
     * frame, so iretq lands at the new program's entry point. */
    if (nr == SYS_EXECVE) {
        extern int user_exec_in_place(const char *path, void *ctx);
        extern int ring3_active(void);
        if (ring3_active()) {
            return (uint64_t)(int64_t)user_exec_in_place(
                (const char *)(uintptr_t)ctx->rdi, ctx);
        }
    }

    /* ring-3 trace: the first handful of calls, so a hang can be localised */
    {
        extern int ring3_active(void);
        extern void ring3_trace_syscall(uint64_t nr, uint64_t a1, uint64_t a2);
        if (ring3_active()) ring3_trace_syscall(nr, ctx->rdi, ctx->rsi);
    }
    ret = syscall_table[nr](ctx->rdi, ctx->rsi, ctx->rdx,
                            ctx->r10, ctx->r8, ctx->r9);

    /* Signal delivery point: Linux delivers pending signals when returning to
     * user mode, and a syscall exit is the only place this kernel has that. The
     * handler rewrites ctx->rip/rsp, so it must run after the return value is
     * fixed up (rax) and only for user tasks (cs != kernel code segment). */
    {
        extern void sig_check_pending(void *uctx);
        if (ctx->cs == 0x23) {              /* user code segment: ring 3 */
            ctx->rax = ret;
            sig_check_pending(ctx);
            return ctx->rax;
        }
    }
    return ret;
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

    /* IA32_EFER.SCE (bit 0) must be set or the `syscall` instruction raises #UD -
     * that is exactly what the first ring-3 run hit (vector 6 at user ip+0x1c). */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        lo |= 1u;                                   /* SCE: syscall/sysret enable */
        __asm__ volatile("wrmsr" :: "c"(0xC0000080), "a"(lo), "d"(hi));
    }

    /* STAR: bits 63:48 = SYSRET base (CS = base+16 = 0x2B, SS = base+8 = 0x23)
     *       bits 47:32 = SYSCALL CS/SS base (CS = 0x08, SS = 0x10)
     * The previous value put 0x10 in the SYSCALL field (kernel data) and 0x23 in
     * the SYSRET field (which would have produced CS=0x33). */
    uint64_t star = ((uint64_t)0x1Bull << 48) | ((uint64_t)0x08ull << 32);

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
    {
        extern uint64_t sys_mprotect(uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t);
        syscall_register(10, sys_mprotect);   /* mprotect */
    }
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