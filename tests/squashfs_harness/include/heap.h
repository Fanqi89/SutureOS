/* Host-harness shadow of kernel/mm/heap.h (see include/stitch/types.h). */
#ifndef STITCH_HEAP_H
#define STITCH_HEAP_H

#include <stitch/types.h>

#define LEMIS_HEAP_HEADER_SIZE 32u

void  lemis_heap_init(void *start, uintptr_t size);
void *lemis_heap_alloc(uintptr_t size);
void  lemis_heap_free(void *ptr);
uintptr_t lemis_heap_free_space(void);

#define malloc(size) lemis_heap_alloc(size)
#define free(ptr) lemis_heap_free(ptr)

#endif /* STITCH_HEAP_H */
