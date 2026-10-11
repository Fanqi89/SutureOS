/* SutureOS module: task structure and scheduler (sched_ prefix)
 * Ported from: Cinux (C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\include\sched.h)
 * Original license: GPL-3.0
 * Changes: cooperative round-robin scheduler; no priority; no SMP;
 *          task states: RUNNABLE, RUNNING, ZOMBIE; shutdown flag set
 *          when no runnable tasks found.
 */
#ifndef STITCH_SCHED_H
#define STITCH_SCHED_H

#include <stitch/types.h>
#include <stitch/cpu_context.h>
#include <stitch/user_context.h>

#define SCHED_MAX_TASKS 16

typedef enum sched_task_state {
    SCHED_TASK_UNUSED = 0,
    SCHED_TASK_RUNNABLE,
    SCHED_TASK_RUNNING,
    SCHED_TASK_ZOMBIE,
} sched_task_state_t;

typedef struct sched_task {
    cinux_cpu_context_t context;
    sched_task_state_t  state;
    void              (*func)(void *);
    void               *arg;
    uint64_t            stack[512];  /* 4KB stack */
} sched_task_t;

typedef struct sched_cpu {
    sched_task_t tasks[SCHED_MAX_TASKS];
    int current;
    bool shutdown;
    int idle_task_id;
} sched_cpu_t;

extern volatile bool g_need_resched;
extern sched_cpu_t sched_cpu;

void sched_init(void);
int  sched_create(void (*func)(void *), void *arg);
int  sched_create_idle(void);
void sched_yield(void);
void sched_shutdown(void);
void sched_start(void);
void sched_tick(void);

void st_sched_test(void);
void st_eevdf_test(void);

/* EEVDF 调度器接口 */
void sched_eevdf_init(void);
int sched_eevdf_create(void (*func)(void *), void *arg, int nice);
void sched_eevdf_yield(void);
void sched_eevdf_tick(void);
void sched_eevdf_start(void);
void eevdf_init_cpu_idle(struct sched_task *idle_task);

/* 用户任务上下文初始化 */
void user_context_init(user_context_t *ctx,
                       uint64_t entry, uint64_t stack_top,
                       uint64_t arg);

#endif /* STITCH_SCHED_H */
