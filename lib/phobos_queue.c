/* SutureOS module: ring buffer queue (phobos_ prefix)
 * Ported from: Phobos (C:\Users\fanqi\Desktop\自研操作系统\Phobos\include\container\queue.h)
 * Original license: GPL-3.0
 * Changes: fixed-size array-backed ring buffer; put returns 0 on full,
 *          get returns 0 on empty; peek added; no dynamic allocation.
 */
#include "phobos_queue.h"
#include <stitch/selftest.h>

void phobos_queue_init(phobos_queue_t *q, void **buffer, uint32_t capacity) {
    q->buffer   = buffer;
    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;
    q->capacity = capacity;
}

bool phobos_queue_put(phobos_queue_t *q, void *item) {
    if (q->count >= q->capacity) return false;
    q->buffer[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return true;
}

bool phobos_queue_get(phobos_queue_t *q, void **item) {
    if (q->count == 0) return false;
    *item = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

bool phobos_queue_peek(phobos_queue_t *q, void **item) {
    if (q->count == 0) return false;
    *item = q->buffer[q->head];
    return true;
}

bool phobos_queue_full(phobos_queue_t *q)  { return q->count >= q->capacity; }
bool phobos_queue_empty(phobos_queue_t *q) { return q->count == 0; }
uint32_t phobos_queue_count(phobos_queue_t *q) { return q->count; }

/* ---- self test ---- */
void st_phobos_queue_test(void) {
    static void *buf[4];
    phobos_queue_t q;
    phobos_queue_init(&q, buf, 4);
    bool pass = true;

    if (!phobos_queue_empty(&q)) pass = false;
    if (phobos_queue_full(&q)) pass = false;

    int a = 1, b = 2, c = 3, d = 4, e = 5;
    if (!phobos_queue_put(&q, &a)) pass = false;
    if (!phobos_queue_put(&q, &b)) pass = false;
    if (!phobos_queue_put(&q, &c)) pass = false;
    if (!phobos_queue_put(&q, &d)) pass = false;
    if (phobos_queue_put(&q, &e)) pass = false;  /* full */
    if (!phobos_queue_full(&q)) pass = false;

    void *out;
    if (!phobos_queue_peek(&q, &out) || out != &a) pass = false;
    if (!phobos_queue_get(&q, &out) || out != &a) pass = false;
    if (!phobos_queue_get(&q, &out) || out != &b) pass = false;
    if (phobos_queue_count(&q) != 2) pass = false;

    /* Wrap around. */
    if (!phobos_queue_put(&q, &e)) pass = false;
    if (!phobos_queue_get(&q, &out) || out != &c) pass = false;
    if (!phobos_queue_get(&q, &out) || out != &d) pass = false;
    if (!phobos_queue_get(&q, &out) || out != &e) pass = false;
    if (!phobos_queue_empty(&q)) pass = false;

    st_run("phobos_queue", pass);
}
