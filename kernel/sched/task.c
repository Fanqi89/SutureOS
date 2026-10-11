/* SutureOS module: task structure and scheduler (sched_ prefix)
 * Ported from: Cinux (C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\include\sched.h)
 * Original license: GPL-3.0
 * Changes: cooperative round-robin scheduler; no priority; no SMP;
 *          task states: RUNNABLE, RUNNING, ZOMBIE; shutdown flag set
 *          when no runnable tasks found.
 *          Fixed: idle task handling, interrupt-safe tick, VMware compatibility
 *          Enhanced: full context initialization including segment bases and flags
 */
#include "task.h"
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>

sched_cpu_t sched_cpu;
volatile bool g_need_resched = false;

/* Idle task: check for reschedule flag and yield when needed */
static void idle_task(void *arg) {
    (void)arg;
    for (;;) {
        if (g_need_resched) {
            g_need_resched = false;
            sched_yield();
        }
        __asm__ volatile("hlt");
    }
}

/* Trampoline: called as initial rip for new tasks.
 * Sets rdi = task->arg, calls task->func, then sched_shutdown if it returns. */
static void sched_trampoline(void) {
    sched_task_t *t = &sched_cpu.tasks[sched_cpu.current];
    t->func(t->arg);
    sched_shutdown();
}

void sched_init(void) {
    memset(&sched_cpu, 0, sizeof(sched_cpu));
    sched_cpu.current = -1;
    sched_cpu.shutdown = false;
    sched_cpu.idle_task_id = -1;
}

int sched_create(void (*func)(void *), void *arg) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (sched_cpu.tasks[i].state == SCHED_TASK_UNUSED) {
            sched_task_t *t = &sched_cpu.tasks[i];
            memset(t, 0, sizeof(*t));
            t->state = SCHED_TASK_RUNNABLE;
            t->func = func;
            t->arg = arg;
            /* Set up initial context: rip = trampoline, rsp = stack top */
            t->context.rip = (uint64_t)sched_trampoline;
            t->context.rsp = (uint64_t)&t->stack[511];
            t->context.rflags = 0x202;  /* IF=1, bit 1 reserved=1 */
            t->context.fs_base = 0;
            t->context.kgs_base = 0;
            t->context._pad = 0;
            return i;
        }
    }
    return -1;
}

/* Create and start the idle task (must be called before enabling interrupts) */
int sched_create_idle(void) {
    int id = sched_create(idle_task, NULL);
    if (id >= 0) {
        sched_cpu.idle_task_id = id;
        sched_cpu.tasks[id].state = SCHED_TASK_RUNNING;
        sched_cpu.current = id;
        /* Initialize idle task context fully */
        sched_cpu.tasks[id].context.fs_base = 0;
        sched_cpu.tasks[id].context.kgs_base = 0;
        sched_cpu.tasks[id].context._pad = 0;
        sched_cpu.tasks[id].context.rflags = 0x202;
    }
    return id;
}

void sched_yield(void) {
    /* Save current context, pick next runnable task, switch to it. */
    if (sched_cpu.current >= 0) {
        sched_cpu.tasks[sched_cpu.current].state = SCHED_TASK_RUNNABLE;
    }
    int next = -1;
    for (int i = 1; i <= SCHED_MAX_TASKS; i++) {
        int idx = (sched_cpu.current + i) % SCHED_MAX_TASKS;
        if (sched_cpu.tasks[idx].state == SCHED_TASK_RUNNABLE) {
            next = idx;
            break;
        }
    }
    if (next < 0) {
        sched_cpu.shutdown = true;
        return;
    }
    int prev = sched_cpu.current;
    sched_cpu.current = next;
    sched_cpu.tasks[next].state = SCHED_TASK_RUNNING;
    if (prev >= 0) {
        cinux_context_switch(&sched_cpu.tasks[prev].context, &sched_cpu.tasks[next].context);
    } else {
        /* First switch: just load the new context. */
        cinux_context_switch(NULL, &sched_cpu.tasks[next].context);
    }
}

/* Called from timer ISR (vector 0x20) to trigger a scheduler tick.
 * Must be safe to call from interrupt context. */
void sched_tick(void)
{
    /* Just set the reschedule flag, don't do context switch in interrupt context */
    g_need_resched = true;
}

void sched_shutdown(void) {
    if (sched_cpu.current >= 0) {
        sched_cpu.tasks[sched_cpu.current].state = SCHED_TASK_ZOMBIE;
    }
    sched_cpu.shutdown = true;
}

void sched_start(void) {
    /* Poll until shutdown flag is set. */
    while (!sched_cpu.shutdown) {
        if (g_need_resched) {
            g_need_resched = false;
            sched_yield();
        }
        __asm__ volatile("hlt");
    }
}

/* ---- self test ---- */
static void sched_test_task_a(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) sched_yield();
}

static void sched_test_task_b(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) sched_yield();
}

void st_sched_test(void) {
    sched_init();
    bool pass = true;

    int tid_a = sched_create(sched_test_task_a, NULL);
    int tid_b = sched_create(sched_test_task_b, NULL);
    if (tid_a < 0 || tid_b < 0) pass = false;

    /* Note: we can't actually run the scheduler in a self-test without
     * disrupting the kernel. Just verify task creation. */
    if (sched_cpu.tasks[tid_a].state != SCHED_TASK_RUNNABLE) pass = false;
    if (sched_cpu.tasks[tid_b].state != SCHED_TASK_RUNNABLE) pass = false;

    /* Clean up. */
    sched_cpu.tasks[tid_a].state = SCHED_TASK_UNUSED;
    sched_cpu.tasks[tid_b].state = SCHED_TASK_UNUSED;
    sched_cpu.current = -1;
    sched_cpu.shutdown = false;
    sched_cpu.idle_task_id = -1;

    st_run("sched", pass);
}

/* 为用户任务初始化 user_context_t：入口、栈、参数 */
void user_context_init(user_context_t *ctx,
                       uint64_t entry, uint64_t stack_top,
                       uint64_t arg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->rip   = entry;
    ctx->rsp   = stack_top;
    ctx->rflags = 0x202;            /* IF=1, reserved bit */
    ctx->cs    = 0x23;              /* 用户代码段 | DPL=3 */
    ctx->ss    = 0x2B;              /* 用户数据段 | DPL=3 */
    ctx->rdi   = arg;               /* 第一个参数 */
    ctx->fs_base = 0;
    ctx->gs_base = 0;
}

/* 保存用户 PML4 到 task 结构（用于 EL0 切换） */
void task_set_user_pml4(int tid, uint64_t pml4)
{
    if (tid >= 0 && tid < SCHED_MAX_TASKS) {
        extern sched_cpu_t sched_cpu;
        /* 复用 kgs_base 字段存用户 PML4 */
        sched_cpu.tasks[tid].context.kgs_base = pml4;
    }
}