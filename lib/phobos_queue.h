/* SutureOS module: ring buffer queue (phobos_ prefix)
 * Ported from: Phobos (C:\Users\fanqi\Desktop\自研操作系统\Phobos\include\container\queue.h)
 * Original license: GPL-3.0
 * Changes: fixed-size array-backed ring buffer; put returns 0 on full,
 *          get returns 0 on empty; peek added; no dynamic allocation.
 */
#ifndef STITCH_PHOBOS_QUEUE_H
#define STITCH_PHOBOS_QUEUE_H

#include <stitch/types.h>

typedef struct phobos_queue {
    void   **buffer;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t capacity;
} phobos_queue_t;

void  phobos_queue_init(phobos_queue_t *q, void **buffer, uint32_t capacity);
bool  phobos_queue_put(phobos_queue_t *q, void *item);   /* false if full */
bool  phobos_queue_get(phobos_queue_t *q, void **item);  /* false if empty */
bool  phobos_queue_peek(phobos_queue_t *q, void **item);
bool  phobos_queue_full(phobos_queue_t *q);
bool  phobos_queue_empty(phobos_queue_t *q);
uint32_t phobos_queue_count(phobos_queue_t *q);

void st_phobos_queue_test(void);

#endif /* STITCH_PHOBOS_QUEUE_H */
