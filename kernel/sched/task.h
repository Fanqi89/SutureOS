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
} sched_cpu_t;

void sched_init(void);
int  sched_create(void (*func)(void *), void *arg);
void sched_yield(void);
void sched_shutdown(void);
void sched_start(void);
void sched_tick(void);

void st_sched_test(void);

#endif /* STITCH_SCHED_H */
