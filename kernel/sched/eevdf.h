/* SutureOS: EEVDF 调度器 (移植自 CoolPotOS)
 * Source: C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\src\include\task\eevdf.h
 * Original license: MIT
 * Changes: Adapted to SutureOS types and task structure; prefix: eevdf_;
 *          Using SutureOS cp_rbtree instead of Linux rb_tree
 */
#ifndef STITCH_EEVDF_H
#define STITCH_EEVDF_H

#include <stitch/types.h>
#include <stitch/rbtree.h>
#include "task.h"

#define SCHED_FIXEDPOINT_SHIFT 10
#define SCHED_FIXEDPOINT_SCALE (1L << SCHED_FIXEDPOINT_SHIFT)

#define SCHED_CAPACITY_SHIFT SCHED_FIXEDPOINT_SHIFT
#define SCHED_CAPACITY_SCALE (1L << SCHED_CAPACITY_SHIFT)

#define NICE_0_LOAD_SHIFT (SCHED_FIXEDPOINT_SHIFT + SCHED_FIXEDPOINT_SHIFT)
#define scale_load(w)     ((w) << SCHED_FIXEDPOINT_SHIFT)
#define scale_load_down(w)                                                                         \
    ({                                                                                             \
        unsigned long __w = (w);                                                                   \
        if (__w)                                                                                   \
            __w = MAX(2UL, __w >> SCHED_FIXEDPOINT_SHIFT);                                         \
        __w;                                                                                       \
    })

#define WEIGHT_IDLEPRIO 3
#define WMULT_IDLEPRIO  1431655765

#define NICE_0_LOAD (1L << NICE_0_LOAD_SHIFT)

#define WMULT_CONST (~0U)
#define WMULT_SHIFT 32

#define VRUNTIME_OFFSET_THRESHOLD 0xa000000000000000

#define NICE_TO_PRIO(nice) (MAX_RT_PRIO + (nice) + 20)
#define PRIO_TO_NICE(prio) ((prio) - MAX_RT_PRIO - 20)

#define MAX_RT_PRIO 100
#define MAX_DL_PRIO 0

/* 常用宏 */
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct eevdf_t eevdf_t;

struct load_weight {
    uint64_t weight;
    uint32_t inv_weight;
};

struct sched_entity {
    uint64_t prio;     // 调度优先级 (nice 值映射)
    uint64_t vruntime; // 虚拟时间
    uint64_t slice;    // 时间片 (纳秒)
    uint64_t custom_slice;
    uint64_t deadline;     // 虚拟截止时间
    uint64_t exec_start;   // 此次调度开始执行的时间
    uint64_t min_vruntime; // 子树下的 min_vruntime
    uint64_t sum_exec_runtime;
    int64_t vlag; // min_vruntime 与 vruntime 的差值
    bool is_idle; // 是否是 IDLE 进程
    struct load_weight load;
    cp_rb_node_t rb_node; // 红黑树节点 (使用 SutureOS cp_rbtree)
    bool on_rq;              // 是否就绪
    struct sched_task *task; // 对应的任务结构
    bool is_yield;           // 是否是 yield 任务
    eevdf_t *handle;         // EEVDF 调度环境
};

struct eevdf_t {
    cp_rb_tree_t *root;
    struct sched_entity *current;     // 当前调度单元
    struct sched_entity *idle_entity; // IDLE 调度单元
    uint64_t avg_load;
    uint64_t avg_vruntime;
    uint64_t min_vruntime;
};

extern unsigned int sysctl_sched_base_slice;

/* EEVDF 核心接口 */
void eevdf_init(void);
void eevdf_enqueue_entity(struct sched_entity *se);
void eevdf_dequeue_entity(struct sched_entity *se);
struct sched_entity *eevdf_pick_next_task(void);
void eevdf_entity_tick(struct sched_entity *curr);
void eevdf_set_entity_yield(struct sched_entity *se);
void eevdf_change_entity_weight(struct sched_entity *se, uint64_t prio);
struct sched_entity *eevdf_new_entity(struct sched_task *task, uint64_t prio);
void eevdf_free_entity(struct sched_entity *se);
void eevdf_init_cpu_idle(struct sched_task *idle_task);

/* 全局访问器 */
struct eevdf_t *eevdf_get_global(void);

/* 调度器对外接口 (替换原有 sched_*) */
void sched_eevdf_init(void);
int sched_eevdf_create(void (*func)(void *), void *arg, int nice);
void sched_eevdf_yield(void);
void sched_eevdf_tick(void);
void sched_eevdf_start(void);

#endif /* STITCH_EEVDF_H */