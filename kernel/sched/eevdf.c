/* SutureOS: EEVDF 调度器实现 (移植自 CoolPotOS)
 * Source: C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\src\task\eevdf.c
 * Original license: MIT
 * Changes: Adapted to SutureOS task structure and types; integrated with existing selftest framework
 *          Using SutureOS cp_rbtree instead of Linux rb_tree
 */
#include "eevdf.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>
#include "task.h"
#include "heap.h"

unsigned int sysctl_sched_base_slice = 700000ULL; // 默认时间片长度 (ns)

/* Linux 兼容的 nice -> weight 映射表 */
const int sched_prio_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */ 9548,  7620,  6100,  4904,  3906,
    /*  -5 */ 3121,  2501,  1991,  1586,  1277,
    /*   0 */ 1024,  820,   655,   526,   423,
    /*   5 */ 335,   272,   215,   172,   137,
    /*  10 */ 110,   87,    70,    56,    45,
    /*  15 */ 36,    29,    23,    18,    15,
};

const uint32_t sched_prio_to_wmult[40] = {
    /* -20 */ 48388,     59856,     76040,     92818,     118348,
    /* -15 */ 147320,    184698,    229616,    287308,    360437,
    /* -10 */ 449829,    563644,    704093,    875809,    1099582,
    /*  -5 */ 1376151,   1717300,   2157191,   2708050,   3363326,
    /*   0 */ 4194304,   5237765,   6557202,   8165337,   10153587,
    /*   5 */ 12820798,  15790321,  19976592,  24970740,  31350126,
    /*  10 */ 39045157,  49367440,  61356676,  76695844,  95443717,
    /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

static struct eevdf_t g_eevdf;
static cp_rb_tree_t g_eevdf_root;

/* 全局访问器 */
struct eevdf_t *eevdf_get_global(void) {
    return &g_eevdf;
}

/* 128位乘法辅助 */
static uint64_t mul_u64_u32_shr(uint64_t a, uint32_t mul, unsigned int shift) {
    return (uint64_t)(((unsigned __int128)a * mul) >> shift);
}

static inline void avg_vruntime_update(uint64_t delta) {
    g_eevdf.avg_vruntime -= g_eevdf.avg_load * delta;
}

static inline int64_t entity_key(struct sched_entity *entity) {
    return (int64_t)(entity->vruntime - g_eevdf.min_vruntime);
}

static inline bool entity_before(const struct sched_entity *a, const struct sched_entity *b) {
    return (int64_t)(a->deadline - b->deadline) < 0;
}

static int vruntime_eligible(uint64_t vruntime) {
    struct sched_entity *curr = g_eevdf.current;
    int64_t avg = g_eevdf.avg_vruntime;
    long load = g_eevdf.avg_load;
    
    if (unlikely(load == 0))
        return 1;
    
    if (curr && curr->on_rq) {
        unsigned long weight = scale_load_down(curr->load.weight);
        avg += entity_key(curr) * weight;
        load += weight;
    }
    return avg >= (int64_t)(vruntime - g_eevdf.min_vruntime) * load;
}

/* 红黑树插入 - 按 deadline 排序 (使用 SutureOS cp_rbtree) */
void insert_sched_entity(cp_rb_tree_t *root, struct sched_entity *se) {
    /* 使用 deadline 作为 key */
    se->rb_node.key = se->deadline;
    se->rb_node.data = se;
    cp_rb_insert(root, se->deadline, se);
}

struct sched_entity *pick_earliest_entity(cp_rb_tree_t *root) {
    cp_rb_node_t *node = cp_rb_first(root);
    if (!node)
        return NULL;
    return (struct sched_entity *)node->data;
}

void set_load_weight(struct sched_entity *entity) {
    int prio = entity->prio - MAX_RT_PRIO;
    struct load_weight lw;
    
    if (entity->prio == NICE_TO_PRIO(20)) {
        lw.weight = scale_load(WEIGHT_IDLEPRIO);
        lw.inv_weight = WMULT_IDLEPRIO;
    } else if (prio >= 0 && prio < 40) {
        lw.weight = scale_load(sched_prio_to_weight[prio]);
        lw.inv_weight = sched_prio_to_wmult[prio];
    } else {
        lw.weight = NICE_0_LOAD;
        lw.inv_weight = WMULT_CONST;
    }
    entity->load = lw;
}

static void __update_inv_weight(struct load_weight *lw) {
    unsigned long w;
    if (lw->inv_weight) return;
    w = scale_load_down(lw->weight);
    if (w >= WMULT_CONST)
        lw->inv_weight = 1;
    else if (!w)
        lw->inv_weight = WMULT_CONST;
    else
        lw->inv_weight = WMULT_CONST / w;
}

static inline void __min_vruntime_update(struct sched_entity *se, cp_rb_node_t *node) {
    if (node) {
        struct sched_entity *rse = (struct sched_entity *)node->data;
        if ((int64_t)(rse->min_vruntime - se->min_vruntime) > 0)
            se->min_vruntime = rse->min_vruntime;
    }
}

static uint64_t __calc_delta(uint64_t delta_exec, unsigned long weight, struct load_weight *lw) {
    uint64_t fact = scale_load_down(weight);
    uint32_t fact_hi = (uint32_t)(fact >> 32);
    int shift = WMULT_SHIFT;
    int fs;

    __update_inv_weight(lw);

    if (fact_hi) {
        fs = 64 - __builtin_clzll(fact_hi);
        shift -= fs;
        fact >>= fs;
    }

    fact = (uint64_t)(fact * lw->inv_weight);
    fact_hi = (uint32_t)(fact >> 32);
    if (fact_hi) {
        fs = 64 - __builtin_clzll(fact_hi);
        shift -= fs;
        fact >>= fs;
    }

    return mul_u64_u32_shr(delta_exec, fact, shift);
}

static inline bool min_vruntime_update(struct sched_entity *se, bool exit) {
    uint64_t old_min_vruntime = se->min_vruntime;
    cp_rb_node_t *node = &se->rb_node;

    se->min_vruntime = se->vruntime;
    __min_vruntime_update(se, node->right);
    __min_vruntime_update(se, node->left);

    return se->min_vruntime == old_min_vruntime;
}

static inline uint64_t calc_delta_fair(uint64_t delta, struct sched_entity *se) {
    if (se->load.weight != NICE_0_LOAD)
        delta = __calc_delta(delta, NICE_0_LOAD, &se->load);
    return delta;
}

static inline uint64_t min_vruntime(uint64_t min_vruntime, uint64_t vruntime) {
    int64_t delta = (int64_t)(vruntime - min_vruntime);
    if (delta < 0)
        min_vruntime = vruntime;
    return min_vruntime;
}

static bool update_deadline(struct sched_entity *se) {
    if ((int64_t)(se->vruntime - se->deadline) < 0)
        return false;
    if (!se->custom_slice)
        se->slice = sysctl_sched_base_slice;
    se->deadline = se->vruntime + calc_delta_fair(se->slice, se);
    se->rb_node.key = se->deadline; // 更新 key
    return true;
}

struct sched_entity *eevdf_new_entity(struct sched_task *task, uint64_t prio) {
    struct sched_entity *entity = (struct sched_entity *)malloc(sizeof(struct sched_entity));
    if (!entity) return NULL;
    
    memset(entity, 0, sizeof(*entity));
    entity->is_idle = (prio == NICE_TO_PRIO(20));
    entity->prio = prio;
    entity->slice = sysctl_sched_base_slice;
    entity->custom_slice = 0;
    entity->on_rq = true;
    entity->deadline = 0;
    entity->vruntime = g_eevdf.min_vruntime;
    entity->min_vruntime = entity->vruntime;
    entity->exec_start = 0; // 将在调度时设置
    entity->is_yield = false;
    set_load_weight(entity);
    update_deadline(entity);
    entity->task = task;
    return entity;
}

struct sched_entity *pick_eevdf(void) {
    struct sched_entity *se = pick_earliest_entity(&g_eevdf_root);
    struct sched_entity *curr = g_eevdf.current;
    struct sched_entity *best = NULL;
    cp_rb_node_t *node = g_eevdf_root.root;

    if (se && vruntime_eligible(se->vruntime)) {
        best = se;
        goto found;
    }

    while (node) {
        cp_rb_node_t *left = node->left;
        if (left && vruntime_eligible(((struct sched_entity *)left->data)->min_vruntime)) {
            node = left;
            continue;
        }
        se = (struct sched_entity *)node->data;
        if (vruntime_eligible(se->vruntime)) {
            best = se;
            break;
        }
        node = node->right;
    }

found:
    if (!best || (curr && entity_before(curr, best)))
        best = curr;

    return best;
}

static uint64_t __update_min_vruntime(uint64_t vruntime) {
    uint64_t min_vruntime = g_eevdf.min_vruntime;
    int64_t delta = (int64_t)(vruntime - min_vruntime);
    if (delta > 0) {
        avg_vruntime_update(delta);
        min_vruntime = vruntime;
    }
    return min_vruntime;
}

static void update_min_vruntime(void) {
    cp_rb_node_t *node = g_eevdf_root.root;
    struct sched_entity *se = node ? (struct sched_entity *)node->data : NULL;
    struct sched_entity *curr = g_eevdf.current;
    uint64_t vruntime = g_eevdf.min_vruntime;

    if (curr) {
        if (curr->on_rq)
            vruntime = curr->vruntime;
        else
            curr = NULL;
    }

    if (se) {
        if (!curr)
            vruntime = se->min_vruntime;
        else
            vruntime = min_vruntime(vruntime, se->vruntime);
    }
    g_eevdf.min_vruntime = MAX(__update_min_vruntime(vruntime), vruntime);
}

/* vruntime 溢出保护 */
static void wrap_vruntime(void) {
    if (likely(g_eevdf.min_vruntime < VRUNTIME_OFFSET_THRESHOLD))
        return;
    uint64_t offset = VRUNTIME_OFFSET_THRESHOLD;
    g_eevdf.min_vruntime -= offset;
    
    struct sched_entity *curr = g_eevdf.current;
    if (curr) {
        curr->vruntime -= offset;
        curr->deadline -= offset;
        curr->rb_node.key = curr->deadline;
    }
    
    cp_rb_node_t *node;
    for (node = cp_rb_first(&g_eevdf_root); node; node = cp_rb_next(node)) {
        struct sched_entity *se = (struct sched_entity *)node->data;
        if (se->vruntime > offset) se->vruntime -= offset;
        if (se->deadline > offset) {
            se->deadline -= offset;
            se->rb_node.key = se->deadline;
        }
    }
    
    if (g_eevdf.avg_load) {
        g_eevdf.avg_vruntime -= (uint64_t)g_eevdf.avg_load * offset;
    } else {
        g_eevdf.avg_vruntime = 0;
    }
}

static int64_t update_curr_se(struct sched_entity *curr) {
    uint64_t now = 0; // 简化：使用固定时间片，实际可读取 TSC/HPET
    int64_t delta_exec = sysctl_sched_base_slice; // 简化：固定时间片
    
    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;
    return delta_exec;
}

static void update_vlag(struct sched_entity *se) {
    int64_t vlag_raw = g_eevdf.min_vruntime - se->vruntime;
    int64_t limit = calc_delta_fair(MAX(2 * se->slice, 1000000), se);
    
    if (vlag_raw > limit) vlag_raw = limit;
    if (vlag_raw < -limit) vlag_raw = -limit;
    se->vlag = vlag_raw;
}

void eevdf_entity_tick(struct sched_entity *curr) {
    if (unlikely(!curr)) return;
    
    bool resche = false;
    int64_t delta_exec = update_curr_se(curr);
    
    if (unlikely(delta_exec <= 0)) {
        if (curr->is_yield) {
            curr->deadline = curr->vruntime + calc_delta_fair(curr->slice, curr);
            curr->rb_node.key = curr->deadline;
            curr->is_yield = false;
            min_vruntime_update(curr, false);
            cp_rb_delete(&g_eevdf_root, curr->rb_node.key);
            insert_sched_entity(&g_eevdf_root, curr);
        }
        return;
    }
    
    curr->vruntime += calc_delta_fair(delta_exec, curr);
    update_vlag(curr);
    resche = update_deadline(curr);
    update_min_vruntime();
    curr->min_vruntime = g_eevdf.min_vruntime;
    
    if (curr->is_yield) {
        curr->deadline = curr->vruntime + calc_delta_fair(curr->slice, curr);
        curr->rb_node.key = curr->deadline;
        curr->is_yield = false;
        resche = true;
    }
    
    if (resche || curr->is_idle) {
        min_vruntime_update(curr, false);
        cp_rb_delete(&g_eevdf_root, curr->rb_node.key);
        insert_sched_entity(&g_eevdf_root, curr);
    }
    wrap_vruntime();
}

void eevdf_set_entity_yield(struct sched_entity *entity) {
    entity->is_yield = true;
}

void eevdf_change_entity_weight(struct sched_entity *entity, uint64_t prio) {
    if (entity->prio == prio) return;
    entity->is_idle = (prio == NICE_TO_PRIO(20));
    entity->prio = prio;
    set_load_weight(entity);
    cp_rb_delete(&g_eevdf_root, entity->rb_node.key);
    insert_sched_entity(&g_eevdf_root, entity);
}

struct sched_entity *eevdf_pick_next_task(void) {
    eevdf_entity_tick(g_eevdf.current);
    struct sched_entity *current = pick_eevdf();
    current->exec_start = 0; // 简化
    g_eevdf.current = current;
    return current;
}

void eevdf_enqueue_entity(struct sched_entity *se) {
    insert_sched_entity(&g_eevdf_root, se);
    g_eevdf.avg_load += scale_load_down(se->load.weight);
}

void eevdf_dequeue_entity(struct sched_entity *se) {
    cp_rb_delete(&g_eevdf_root, se->rb_node.key);
    se->on_rq = false;
    
    if (g_eevdf.current == se)
        g_eevdf.current = NULL;
    
    g_eevdf.avg_load -= scale_load_down(se->load.weight);
    min_vruntime_update(se, true);
    update_min_vruntime();
}

void eevdf_init_cpu_idle(struct sched_task *idle_task) {
    cp_rb_init(&g_eevdf_root);
    g_eevdf.root = &g_eevdf_root;
    g_eevdf.min_vruntime = 0;
    g_eevdf.avg_load = 0;
    g_eevdf.avg_vruntime = 0;
    
    struct sched_entity *idle_entity = eevdf_new_entity(idle_task, NICE_TO_PRIO(20));
    idle_entity->handle = &g_eevdf;
    idle_task->context.kgs_base = (uint64_t)(uintptr_t)idle_entity; // 复用字段存 entity 指针
    
    g_eevdf.idle_entity = idle_entity;
    g_eevdf.current = idle_entity;
    insert_sched_entity(&g_eevdf_root, idle_entity);
}

void eevdf_init(void) {
    /* 红黑树在 init_cpu_idle 中初始化 */
}

/* ==================== 调度器对外接口 ==================== */

static struct sched_task *g_current_task = NULL;

void sched_eevdf_init(void) {
    eevdf_init();
    console_puts("[eevdf] scheduler initialized\n");
}

int sched_eevdf_create(void (*func)(void *), void *arg, int nice) {
    int tid = sched_create(func, arg);
    if (tid < 0) return -1;
    
    uint64_t prio = NICE_TO_PRIO(nice);
    struct sched_task *task = &sched_cpu.tasks[tid];
    struct sched_entity *entity = eevdf_new_entity(task, prio);
    if (!entity) return -1;
    
    entity->handle = &g_eevdf;
    task->context.kgs_base = (uint64_t)(uintptr_t)entity;
    
    eevdf_enqueue_entity(entity);
    console_printf("[eevdf] task %d created with nice=%d prio=%llu\n", tid, nice, prio);
    return tid;
}

void sched_eevdf_yield(void) {
    if (g_eevdf.current && g_eevdf.current->task) {
        eevdf_set_entity_yield(g_eevdf.current);
        sched_yield(); // 触发上下文切换
    }
}

void sched_eevdf_tick(void) {
    if (g_eevdf.current) {
        eevdf_entity_tick(g_eevdf.current);
    }
}

void sched_eevdf_start(void) {
    console_puts("[eevdf] starting scheduler...\n");
    
    // 确保 idle 任务存在
    if (!g_eevdf.idle_entity) {
        if (sched_cpu.idle_task_id >= 0) {
            struct sched_task *idle = &sched_cpu.tasks[sched_cpu.idle_task_id];
            eevdf_init_cpu_idle(idle);
        }
    }
    
    // 启用中断开始调度
    __asm__ volatile("sti");
    
    // 调度循环
    while (!sched_cpu.shutdown) {
        struct sched_entity *next = eevdf_pick_next_task();
        if (!next) {
            __asm__ volatile("hlt");
            continue;
        }
        
        struct sched_task *next_task = next->task;
        if (!next_task) {
            __asm__ volatile("hlt");
            continue;
        }
        
        int prev_tid = sched_cpu.current;
        if (prev_tid >= 0 && prev_tid != (next_task - sched_cpu.tasks)) {
            sched_cpu.tasks[prev_tid].state = SCHED_TASK_RUNNABLE;
        }
        
        sched_cpu.current = next_task - sched_cpu.tasks;
        next_task->state = SCHED_TASK_RUNNING;
        g_current_task = next_task;
        
        // 上下文切换
        if (prev_tid >= 0) {
            cinux_context_switch(&sched_cpu.tasks[prev_tid].context, &next_task->context);
        } else {
            cinux_context_switch(NULL, &next_task->context);
        }
    }
}

/* 自检 */
void st_eevdf_test(void) {
    bool pass = true;
    
    eevdf_init();
    
    // 测试实体创建
    struct sched_task dummy_task;
    memset(&dummy_task, 0, sizeof(dummy_task));
    struct sched_entity *e1 = eevdf_new_entity(&dummy_task, NICE_TO_PRIO(0));
    struct sched_entity *e2 = eevdf_new_entity(&dummy_task, NICE_TO_PRIO(-5));
    
    if (!e1 || !e2) pass = false;
    
    // 测试入队/出队
    eevdf_enqueue_entity(e1);
    eevdf_enqueue_entity(e2);
    
    if (g_eevdf_root.root == NULL) pass = false;
    
    // 测试拣选
    struct sched_entity *picked = eevdf_pick_next_task();
    if (!picked) pass = false;
    
    // 测试 vruntime 推进
    eevdf_entity_tick(picked);
    if (picked->vruntime == 0) pass = false;
    
    // 清理
    eevdf_dequeue_entity(e1);
    eevdf_dequeue_entity(e2);
    free(e1);
    free(e2);
    
    st_run("eevdf", pass);
}