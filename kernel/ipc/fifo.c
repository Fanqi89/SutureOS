/* SutureOS module: FIFO queue (helos_ prefix)
 * Ported from: Helo OS (C:\Users\fanqi\Desktop\自研操作系统\HeloOS\kernel\include\fifo.h)
 * Original license: GPL-3.0
 * Changes: design adapted + clean rewrite (Haribote lineage copyright risk);
 *          32-bit index type; put returns 0 on full, get returns 0 on empty;
 *          flags field removed; buffer is void**.
 */
#include "fifo.h"
#include <stitch/selftest.h>

void helos_fifo32_init(helos_fifo32_t *fifo, void **buffer, uint32_t capacity) {
    fifo->buffer   = buffer;
    fifo->head     = 0;
    fifo->tail     = 0;
    fifo->count    = 0;
    fifo->capacity = capacity;
}

bool helos_fifo32_put(helos_fifo32_t *fifo, void *data) {
    if (fifo->count >= fifo->capacity) return false;
    fifo->buffer[fifo->tail] = data;
    fifo->tail = (fifo->tail + 1) % fifo->capacity;
    fifo->count++;
    return true;
}

bool helos_fifo32_get(helos_fifo32_t *fifo, void **data) {
    if (fifo->count == 0) return false;
    *data = fifo->buffer[fifo->head];
    fifo->head = (fifo->head + 1) % fifo->capacity;
    fifo->count--;
    return true;
}

bool helos_fifo32_empty(helos_fifo32_t *fifo) { return fifo->count == 0; }
bool helos_fifo32_full(helos_fifo32_t *fifo)  { return fifo->count >= fifo->capacity; }
uint32_t helos_fifo32_count(helos_fifo32_t *fifo) { return fifo->count; }

/* ---- self test ---- */
void st_helos_fifo32_test(void) {
    static void *buf[8];
    helos_fifo32_t fifo;
    helos_fifo32_init(&fifo, buf, 8);
    bool pass = true;

    if (!helos_fifo32_empty(&fifo)) pass = false;

    int a = 1, b = 2, c = 3;
    if (!helos_fifo32_put(&fifo, &a)) pass = false;
    if (!helos_fifo32_put(&fifo, &b)) pass = false;
    if (!helos_fifo32_put(&fifo, &c)) pass = false;
    if (helos_fifo32_count(&fifo) != 3) pass = false;

    void *out;
    if (!helos_fifo32_get(&fifo, &out) || out != &a) pass = false;
    if (!helos_fifo32_get(&fifo, &out) || out != &b) pass = false;
    if (helos_fifo32_count(&fifo) != 1) pass = false;

    /* Fill to capacity. */
    for (int i = 0; i < 7; i++) {
        if (!helos_fifo32_put(&fifo, &i)) pass = false;
    }
    if (!helos_fifo32_full(&fifo)) pass = false;
    if (helos_fifo32_put(&fifo, &a)) pass = false;  /* full */

    /* Drain. */
    while (helos_fifo32_get(&fifo, &out)) {}
    if (!helos_fifo32_empty(&fifo)) pass = false;

    st_run("helos_fifo32", pass);
}
