/* SutureOS module: slab allocator (sfox_ prefix)
 * Ported from: SpiritFoxOS (C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\kernel\mm\slab.c)
 * Original license: GPL-3.0
 * Changes: upstream double-free detection inverted (if (!bitmap_test) ->
 *          if (bitmap_test) return;), enabling full->partial/free migration;
 *          alloc_page/free_page/alloc -> moe_pmm_alloc_page/free_page/alloc;
 *          static inline pushfq+cpu_cli/conditional cpu_sti lock; only init
 *          prints 1 console_puts line; slab_realloc not ported.
 */
#ifndef STITCH_SLAB_H
#define STITCH_SLAB_H

#include <stitch/types.h>

typedef struct sfox_slab {
    uint32_t obj_size;
    uint32_t obj_count;
    uint8_t *bitmap;        /* 1 = free */
    void    *data;
    struct sfox_slab *next;
} sfox_slab_t;

typedef struct sfox_slab_cache {
    sfox_slab_t *partial;
    sfox_slab_t *full;
    sfox_slab_t *free_list;
    uint32_t     slab_count;
} sfox_slab_cache_t;

void *sfox_slab_alloc(sfox_slab_cache_t *cache, uintptr_t size);
void  sfox_slab_free(sfox_slab_cache_t *cache, void *ptr, uintptr_t size);
void  sfox_slab_cache_init(sfox_slab_cache_t *cache);
uintptr_t sfox_slab_alloc_size(uintptr_t size);  /* size class lookup */

void st_sfox_slab_test(void);

#endif /* STITCH_SLAB_H */
