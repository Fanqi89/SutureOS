/* SutureOS: 信号处理 (sigaction / sigprocmask / 信号帧 / sigreturn)
 *
 * Signal delivery on x86-64 SutureOS:
 *
 *   1. A syscall or a fault marks a pending bit (sig_send()). Delivery happens on
 *      the return-to-user path: syscall_dispatch_c() calls sig_check_pending()
 *      once the syscall's return value is in ctx->rax, which is the only point
 *      this kernel has where the interrupted user context is fully reconstructible
 *      (user_context_t, 176 bytes, built by arch/x86_64/syscall.S).
 *
 *   2. sig_check_pending() builds a `struct sigframe` ON THE USER STACK, below the
 *      interrupted rsp:
 *
 *         [frame_rsp+0]    return address = act.sa_restorer   <- handler `ret`
 *         [frame_rsp+8]    magic  (SUTURESG)
 *         [frame_rsp+16]   signal number
 *         [frame_rsp+24]   the task's signal mask before delivery
 *         [frame_rsp+32]   the interrupted user_context_t (176 bytes)
 *
 *      and rewrites the context to rip = handler, rdi = sig, rsp = frame_rsp.
 *      frame_rsp is 16-byte aligned then lowered by 8 so the handler sees the
 *      SysV "just after the call" alignment (rsp % 16 == 8).
 *
 *   3. The trampoline is the *program's own* restorer, not kernel-injected code:
 *      a handler compiled in user space returns with `ret`, which pops
 *      sa_restorer, and user/lib/syscall.S provides __v64_sigreturn_stub
 *      (`mov $15,%rax; syscall`). Writing a stub into the user stack instead
 *      would be wrong here because the user stack PTE is deliberately NX
 *      (kernel/syscall/userspace.c); no executable byte is injected.
 *
 *   4. rt_sigreturn(15) copies the saved user_context_t back into the live
 *      syscall frame, so rip/rsp/rflags and every GPR resume exactly where the
 *      signal interrupted the program, and restores the saved signal mask.
 *
 * Honest limits: no siginfo (the handler sees only int), no sigaltstack, no
 * SA_RESTART/SA_RESETHAND, no real-time signal queueing (one pending bit per
 * signal), no SIGSTOP/job control, and a handler registered without a restorer
 * is refused (the signal is dropped and the refusal is logged) instead of
 * returning into garbage.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "task.h"
#include <stitch/user_context.h>
#include "el0_validate.h"
#include "signal.h"

#define NSIG_STOP   19      /* SIGSTOP: never blockable */
#define NSIG_KILL   9       /* SIGKILL: never blockable */

#define SIGFRAME_MAGIC 0x5355545552455347ull    /* "SUTURESG" (LE) */

/* SA_* flags the kernel honours (the rest are recorded, not acted on).
 * SA_RESTORER comes from signal.h (0x04000000). */
#define SA_NODEFER     0x40000000u

typedef struct {
    uint64_t       ret_addr;    /* +0   act.sa_restorer: the handler's `ret` target */
    uint64_t       magic;       /* +8                                          */
    uint64_t       signr;       /* +16                                         */
    uint64_t       saved_mask;  /* +24  signal mask before this delivery       */
    user_context_t ctx;         /* +32  the interrupted register state          */
} sigframe_t;

_Static_assert(sizeof(user_context_t) == 176, "user_context_t layout");
_Static_assert(sizeof(sigframe_t) == 208, "sigframe must stay 4*8 + 176 bytes");

/* Both tables are declared extern in signal.h (the fault path in
 * kernel/interrupts/intr.c only uses sig_send, but the header stays the single
 * description of the signal state). */
struct k_sigaction sig_table[SCHED_MAX_TASKS][NSIG];

/* 信号掩码 */
uint64_t sig_masks[SCHED_MAX_TASKS] = {0};
uint64_t sig_pending[SCHED_MAX_TASKS] = {0};

/* Bits that can never be blocked (Linux: SIGKILL / SIGSTOP). */
#define UNBLOCKABLE ((1ull << (NSIG_KILL - 1)) | (1ull << (NSIG_STOP - 1)))

void sig_init_task(int tid) {
    for (int i = 0; i < NSIG; i++) {
        sig_table[tid][i].sa.sa_handler = SIG_DFL;
        sig_table[tid][i].sa.sa_flags = 0;
        sig_table[tid][i].sa.sa_mask = 0;
        sig_table[tid][i].sa.sa_restorer = NULL;
    }
    sig_masks[tid] = 0;
    sig_pending[tid] = 0;
}

void sig_init_all(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) sig_init_task(i);
}

uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_ptr, uint64_t oldact_ptr,
                          uint64_t sigsetsize, uint64_t a5, uint64_t a6)
{
    (void)sigsetsize; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    const int tid = sched_cpu.current;

    if (signum == 0 || signum >= NSIG) return (uint64_t)(int64_t)(-22); /* -EINVAL */
    if (signum == NSIG_KILL || signum == NSIG_STOP)
        return (uint64_t)(int64_t)(-22);        /* cannot be caught or ignored */
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return (uint64_t)(int64_t)(-3); /* -ESRCH */
    {
        struct sigaction *act = (struct sigaction *)(uintptr_t)act_ptr;
        struct sigaction *oldact = (struct sigaction *)(uintptr_t)oldact_ptr;
        if (oldact) {
            EL0_CHECK(oldact, sizeof(struct sigaction));
            memcpy(oldact, &sig_table[tid][signum].sa, sizeof(struct sigaction));
        }
        if (act) {
            EL0_CHECK(act, sizeof(struct sigaction));
            memcpy(&sig_table[tid][signum].sa, act, sizeof(struct sigaction));
        }
    }
    return 0;
}

/* rt_sigprocmask(14): the block/unblock set really masks delivery, because
 * sig_check_pending() ANDs the pending word with this mask. SIGKILL/SIGSTOP bits
 * are always cleared, exactly like Linux. */
uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_va, uint64_t oldset_va,
                            uint64_t sigsetsize, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    const int tid = sched_cpu.current;

    if (tid < 0 || tid >= SCHED_MAX_TASKS) return (uint64_t)(int64_t)(-3); /* -ESRCH */
    /* Linux requires exactly sizeof(sigset_t) == 8 and rejects anything else. */
    if (sigsetsize != sizeof(uint64_t)) return (uint64_t)(int64_t)(-22);
    if (oldset_va != 0) {
        if (!el0_ptr_ok(oldset_va, sizeof(uint64_t)))
            return (uint64_t)(int64_t)(-14);                /* -EFAULT */
        memcpy((void *)(uintptr_t)oldset_va, &sig_masks[tid], sizeof(uint64_t));
    }
    if (set_va != 0) {
        uint64_t set;
        if (!el0_ptr_ok(set_va, sizeof(uint64_t)))
            return (uint64_t)(int64_t)(-14);
        memcpy(&set, (const void *)(uintptr_t)set_va, sizeof(set));
        switch ((int)how) {
        case 0:  sig_masks[tid] |= set;  break;         /* SIG_BLOCK   */
        case 1:  sig_masks[tid] &= ~set; break;         /* SIG_UNBLOCK */
        case 2:  sig_masks[tid]  = set;  break;         /* SIG_SETMASK */
        default: return (uint64_t)(int64_t)(-22);       /* -EINVAL     */
        }
        sig_masks[tid] &= ~UNBLOCKABLE;
        sig_masks[tid] &= ~(1ull << (NSIG - 1));        /* bits 1..31 only */
    }
    return 0;
}

/* 发送信号 */
void sig_send(int tid, int sig) {
    if (sig >= NSIG || sig < 1) return;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return;
    sig_pending[tid] |= (1ULL << (sig - 1));
    /* 如果目标在休眠，唤醒 */
    extern sched_cpu_t sched_cpu;
    if (sched_cpu.tasks[tid].state == SCHED_TASK_ZOMBIE) return;
    sched_cpu.tasks[tid].state = SCHED_TASK_RUNNABLE;
}

/* 检查并分发待处理信号 (在 syscall 返回时调用) */
void sig_check_pending(void *uctx_void) {
    extern sched_cpu_t sched_cpu;
    extern int ring3_active(void);
    extern void ring3_exit_user(uint64_t code);
    user_context_t *uctx = (user_context_t *)uctx_void;
    sigframe_t frame;
    uint64_t pending, handler, frame_rsp;
    int sig, tid;
    struct sigaction *sa;

    if (uctx == NULL) return;
    tid = sched_cpu.current;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return;

    pending = sig_pending[tid] & ~sig_masks[tid];
    if (!pending) return;
    sig = __builtin_ctzll(pending) + 1;
    sig_pending[tid] &= ~(1ULL << (sig - 1));
    sa = &sig_table[tid][sig].sa;
    handler = (uint64_t)(uintptr_t)sa->sa_handler;

    if (handler == (uint64_t)SIG_IGN) return;      /* SIG_IGN: drop it */

    if (handler == (uint64_t)SIG_DFL) {
        /* Default action: terminate the *process*, never the machine. The old
         * code called sched_shutdown() here, so any SIG_DFL delivery halted the
         * whole system; kernel-context deliveries are simply dropped. */
        if (ring3_active()) {
            ring3_exit_user((uint64_t)(128u + (uint64_t)sig));
        }
        return;
    }

    /* A handler is only reachable if the program gave us a return trampoline.
     * Refusing is honest: jumping there without one would `ret` into garbage. */
    if (sa->sa_restorer == NULL) {
        console_printf("[sig] deliver FAILED reason=no-restorer sig=%d\n",
                       (int64_t)sig);
        return;
    }

    /* Lay the frame out below the interrupted user stack pointer. */
    frame_rsp = (uctx->rsp - (uint64_t)sizeof(sigframe_t)) & ~0xFULL;
    frame_rsp -= 8;                     /* handler sees rsp % 16 == 8 (SysV) */
    if (!el0_ptr_ok(frame_rsp, (uint64_t)sizeof(sigframe_t))) {
        console_printf("[sig] deliver FAILED reason=bad-user-stack sig=%d rsp=0x%llx\n",
                       (int64_t)sig, (unsigned long long)uctx->rsp);
        return;
    }

    frame.ret_addr   = (uint64_t)(uintptr_t)sa->sa_restorer;
    frame.magic      = SIGFRAME_MAGIC;
    frame.signr      = (uint64_t)sig;
    frame.saved_mask = sig_masks[tid];
    frame.ctx        = *uctx;
    memcpy((void *)(uintptr_t)frame_rsp, &frame, sizeof(frame));

    /* Block this signal while its handler runs unless SA_NODEFER was asked for,
     * plus the handler's own sa_mask. sigreturn restores the saved mask. */
    if ((sa->sa_flags & SA_NODEFER) == 0u)
        sig_masks[tid] |= (1ull << (sig - 1));
    sig_masks[tid] |= (sa->sa_mask & ~UNBLOCKABLE);

    uctx->rip = handler;
    uctx->rdi = (uint64_t)sig;          /* handler(int sig) */
    uctx->rsp = frame_rsp;
}

/* sigreturn: 从信号处理返回，恢复被信号打断的完整现场 */
uint64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    extern sched_cpu_t sched_cpu;
    extern user_context_t *syscall_current_ctx(void);
    user_context_t *uctx = syscall_current_ctx();
    sigframe_t frame;
    const int tid = sched_cpu.current;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Only a syscall-instruction return has a live user_context_t to restore. */
    if (uctx == NULL) return (uint64_t)(int64_t)(-22);          /* -EINVAL */
    /* The handler reached its restorer with `ret`, which already popped the
     * return-address slot at frame+0, so the live rsp is frame+8. Linux's
     * rt_sigreturn subtracts one word for exactly this reason. */
    {
        const uint64_t frame_base = uctx->rsp - sizeof(uint64_t);
        if (!el0_ptr_ok(frame_base, (uint64_t)sizeof(sigframe_t)))
            return (uint64_t)(int64_t)(-14);                    /* -EFAULT */
        memcpy(&frame, (const void *)(uintptr_t)frame_base, sizeof(frame));
        if (frame.magic != SIGFRAME_MAGIC) {
            console_printf("[sig] sigreturn: no signal frame at rsp=0x%llx magic=0x%llx\n",
                           (unsigned long long)uctx->rsp,
                           (unsigned long long)frame.magic);
            return (uint64_t)(int64_t)(-14);
        }
    }

    if (tid >= 0 && tid < SCHED_MAX_TASKS)
        sig_masks[tid] = frame.saved_mask & ~UNBLOCKABLE;

    /* Full restore: r15..rax, rip, cs, rflags, rsp, ss, fs/gs bases. */
    *uctx = frame.ctx;

    /* syscall_dispatch_c() writes the handler's return value into ctx->rax and
     * arch/x86_64/syscall.S then stores it into the iret frame, so returning the
     * *restored* rax is what keeps the register intact. */
    return uctx->rax;
}
