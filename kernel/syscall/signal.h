/* SutureOS: 信号处理头文件 */
#ifndef STITCH_SIGNAL_H
#define STITCH_SIGNAL_H

#include <stitch/types.h>
#include "task.h"

#define NSIG 32
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_RESTORER 0x04000000
#define SIGSEGV 11

struct sigaction {
    void (*sa_handler)(int);
    uint64_t sa_flags;
    void (*sa_restorer)(void);
    uint64_t sa_mask;
};

struct k_sigaction {
    struct sigaction sa;
};

extern struct k_sigaction sig_table[SCHED_MAX_TASKS][32];

/* 信号掩码 */
extern uint64_t sig_masks[SCHED_MAX_TASKS];
extern uint64_t sig_pending[SCHED_MAX_TASKS];

void sig_init_task(int tid);
void sig_init_all(void);
void sig_send(int tid, int sig);
void sig_check_pending(void *uctx);

/* fork(): the child inherits the parent's handlers and blocked mask (the slot is
 * the process's own row, see process.h PROC_SLOT_BASE); pending signals are not
 * inherited. */
void sig_slot_clone(int from, int to);
uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_ptr, uint64_t oldact_ptr,
                          uint64_t sigsetsize, uint64_t a5, uint64_t a6);
void sig_send(int tid, int sig);
void sig_check_pending(void *uctx);
uint64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

#define SIGSEGV 11
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_RESTORER 0x04000000

#endif