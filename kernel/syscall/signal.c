/* SutureOS: 信号处理 (sigaction/sigreturn) */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "task.h"
#include <stitch/user_context.h>
#include "el0_validate.h"

#define NSIG 32
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_RESTORER 0x04000000

struct sigaction {
    void (*sa_handler)(int);
    uint64_t sa_flags;
    void (*sa_restorer)(void);
    uint64_t sa_mask;
};

struct k_sigaction {
    struct sigaction sa;
};

static struct k_sigaction sig_table[SCHED_MAX_TASKS][NSIG];

/* 信号掩码 */
static uint64_t sig_masks[SCHED_MAX_TASKS] = {0};
static uint64_t sig_pending[SCHED_MAX_TASKS] = {0};

void sig_init_task(int tid) {
    for (int i = 0; i < NSIG; i++) {
        sig_table[tid][i].sa.sa_handler = SIG_DFL;
        sig_table[tid][i].sa.sa_flags = 0;
        sig_table[tid][i].sa.sa_mask = 0;
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
    if (signum >= NSIG) return (uint64_t)-1;
    struct sigaction *act = (struct sigaction *)(uintptr_t)act_ptr;
    struct sigaction *oldact = (struct sigaction *)(uintptr_t)oldact_ptr;
    if (oldact) {
        EL0_CHECK(oldact, sizeof(struct sigaction));
        memcpy(oldact, &sig_table[sched_cpu.current][signum].sa, sizeof(struct sigaction));
    }
    if (act) {
        EL0_CHECK(act, sizeof(struct sigaction));
        memcpy(&sig_table[sched_cpu.current][signum].sa, act, sizeof(struct sigaction));
    }
    return 0;
}

/* 发送信号 */
void sig_send(int tid, int sig) {
    if (sig >= NSIG || sig < 1) return;
    sig_pending[tid] |= (1ULL << (sig - 1));
    /* 如果目标在休眠，唤醒 */
    extern sched_cpu_t sched_cpu;
    sched_cpu.tasks[tid].state = SCHED_TASK_RUNNABLE;
}

/* 检查并分发待处理信号 (在 syscall 返回、中断返回时调用) */
void sig_check_pending(user_context_t *uctx) {
    extern sched_cpu_t sched_cpu;
    uint64_t pending = sig_pending[sched_cpu.current] & ~sig_masks[sched_cpu.current];
    if (!pending) return;
    int sig = __builtin_ctzll(pending) + 1;
    sig_pending[sched_cpu.current] &= ~(1ULL << (sig - 1));
    struct sigaction *sa = &sig_table[sched_cpu.current][sig].sa;
    if ((uintptr_t)sa->sa_handler == (uintptr_t)SIG_IGN) return;
    if ((uintptr_t)sa->sa_handler == (uintptr_t)SIG_DFL) {
        /* 默认动作：终止 */
        sched_shutdown();
        return;
    }
    /* 保存当前上下文到 signal frame */
    /* 简化：直接修改 uctx->rip 指向 handler */
    uctx->rip = (uint64_t)sa->sa_handler;
    uctx->rdi = sig;
    if (sa->sa_flags & SA_RESTORER) {
        uctx->rsp -= 8;
        *(uint64_t *)(uintptr_t)uctx->rsp = (uint64_t)sa->sa_restorer;
    }
}

/* sigreturn: 从信号处理返回 */
uint64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* 恢复上下文 (由用户栈上的 sigframe 完成) */
    /* 简化：仅返回 */
    return 0;
}