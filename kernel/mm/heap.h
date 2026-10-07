/* SutureOS module: kernel heap allocator (lemis_ prefix)
 * Ported from: Lemis (C:\Users\fanqi\Desktop\自研操作系统\Lemis\kernel\src\mm\heap.rs)
 * Original license: GPL-3.0
 * Changes: upstream long long* pointer arithmetic (x8, merge never worked)
 *          -> byte uintptr_t arithmetic; 4->16 byte alignment; block header
 *          with explicit pad = 32 bytes total (payload guaranteed 16-aligned);
 *          split remainder threshold HEADER+16; start==NULL falls back to
 *          built-in 64KB arena; init size underflow guard.
 */
#ifndef STITCH_HEAP_H
#define STITCH_HEAP_H

#include <stitch/types.h>

#define LEMIS_HEAP_HEADER_SIZE 32u   /* header + pad, payload 16-aligned */

void  lemis_heap_init(void *start, uintptr_t size);
void *lemis_heap_alloc(uintptr_t size);
void  lemis_heap_free(void *ptr);
uintptr_t lemis_heap_free_space(void);

void st_lemis_heap_test(void);

#endif /* STITCH_HEAP_H */
