/* SutureOS: 优先级抢占式调度器 (红黑树) */
#ifndef STITCH_PRIO_SCHED_H
#define STITCH_PRIO_SCHED_H

#include <stitch/types.h>
#include "task.h"

#define SCHED_PRIO_MAX 32

void prio_sched_init(void);
void prio_sched_enqueue(int tid, int priority);  /* priority: 0=最高 */
int prio_sched_dequeue(void);                    /* 返回 tid 或 -1 */
void prio_sched_remove(uint64_t key);            /* 按 key 删除 */

#endif