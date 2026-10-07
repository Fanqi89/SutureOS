/* SutureOS module: FIFO queue (helos_ prefix)
 * Ported from: Helo OS (C:\Users\fanqi\Desktop\自研操作系统\HeloOS\kernel\include\fifo.h)
 * Original license: GPL-3.0
 * Changes: design adapted + clean rewrite (Haribote lineage copyright risk);
 *          32-bit index type; put returns 0 on full, get returns 0 on empty;
 *          flags field removed; buffer is void**.
 */
#ifndef STITCH_FIFO_H
#define STITCH_FIFO_H

#include <stitch/types.h>

typedef struct helos_fifo32 {
    void    **buffer;
    uint32_t  head;
    uint32_t  tail;
    uint32_t  count;
    uint32_t  capacity;
} helos_fifo32_t;

void  helos_fifo32_init(helos_fifo32_t *fifo, void **buffer, uint32_t capacity);
bool  helos_fifo32_put(helos_fifo32_t *fifo, void *data);   /* false if full */
bool  helos_fifo32_get(helos_fifo32_t *fifo, void **data);  /* false if empty */
bool  helos_fifo32_empty(helos_fifo32_t *fifo);
bool  helos_fifo32_full(helos_fifo32_t *fifo);
uint32_t helos_fifo32_count(helos_fifo32_t *fifo);

void st_helos_fifo32_test(void);

#endif /* STITCH_FIFO_H */
