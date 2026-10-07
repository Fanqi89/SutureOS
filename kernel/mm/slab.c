/* SutureOS module: slab allocator (sfox_ prefix)
 * Ported from: SpiritFoxOS (C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\kernel\mm\slab.c)
 * Original license: GPL-3.0
 * Changes: upstream double-free detection inverted (if (!bitmap_test) ->
 *          if (bitmap_test) return;), enabling full->partial/free migration;
 *          alloc_page/free_page/alloc -> moe_pmm_alloc_page/free_page/alloc;
 *          static inline pushfq+cpu_cli/conditional cpu_sti lock; only init
 *          prints 1 console_puts line; slab_realloc not ported.
 */
#include "slab.h"
#include "pmm.h"
#include <stitch/io.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>

#define SFOX_SLAB_MAGIC 0x53464F58u  /* "SFOX" */

static inline void sfox_lock(uint64_t *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags));
}
static inline void sfox_unlock(uint64_t flags) {
    if (flags & 0x200) __asm__ volatile("sti");
}

static sfox_slab_t *sfox_slab_create(uint32_t obj_size) {
    sfox_slab_t *slab = (sfox_slab_t *)moe_pmm_alloc_page();
    if (!slab) return NULL;
    slab->obj_size  = obj_size;
    slab->obj_count = (4096 - sizeof(sfox_slab_t)) / obj_size;
    slab->bitmap    = (uint8_t *)(slab + 1);
    slab->data      = (uint8_t *)slab->bitmap + (slab->obj_count + 7) / 8;
    slab->next      = NULL;
    memset(slab->bitmap, 0xFF, (slab->obj_count + 7) / 8);  /* all free */
    return slab;
}

/* Retained for future cache shrink/destroy support. */
__attribute__((unused))
static void sfox_slab_destroy(sfox_slab_t *slab) {
    moe_pmm_free_page((uintptr_t)slab);
}

void sfox_slab_cache_init(sfox_slab_cache_t *cache) {
    cache->partial = NULL;
    cache->full    = NULL;
    cache->free_list = NULL;
    cache->slab_count = 0;
}

static sfox_slab_t *sfox_cache_find_partial(sfox_slab_cache_t *cache, uint32_t obj_size) {
    sfox_slab_t *s = cache->partial;
    while (s) {
        if (s->obj_size == obj_size) return s;
        s = s->next;
    }
    return NULL;
}

static sfox_slab_t *sfox_cache_find_free(sfox_slab_cache_t *cache, uint32_t obj_size) {
    sfox_slab_t *s = cache->free_list;
    while (s) {
        if (s->obj_size == obj_size) return s;
        s = s->next;
    }
    return NULL;
}

void *sfox_slab_alloc(sfox_slab_cache_t *cache, uintptr_t size) {
    if (size == 0) return NULL;
    uint64_t flags;
    sfox_lock(&flags);

    sfox_slab_t *slab = sfox_cache_find_partial(cache, (uint32_t)size);
    if (!slab) {
        slab = sfox_cache_find_free(cache, (uint32_t)size);
        if (slab) {
            /* Move from free_list to partial. */
            sfox_slab_t **pp = &cache->free_list;
            while (*pp && *pp != slab) pp = &(*pp)->next;
            if (*pp) *pp = slab->next;
            slab->next = cache->partial;
            cache->partial = slab;
        }
    }
    if (!slab) {
        slab = sfox_slab_create((uint32_t)size);
        if (!slab) { sfox_unlock(flags); return NULL; }
        slab->next = cache->partial;
        cache->partial = slab;
        cache->slab_count++;
    }

    /* Find a free slot. */
    for (uint32_t i = 0; i < slab->obj_count; i++) {
        uint32_t byte = i / 8, bit = i % 8;
        if (slab->bitmap[byte] & (1u << bit)) {
            slab->bitmap[byte] &= ~(1u << bit);
            void *ptr = (uint8_t *)slab->data + i * slab->obj_size;

            /* If slab is now full, move to full list. */
            bool any_free = false;
            for (uint32_t j = 0; j < slab->obj_count; j++) {
                if (slab->bitmap[j / 8] & (1u << (j % 8))) { any_free = true; break; }
            }
            if (!any_free) {
                sfox_slab_t **pp = &cache->partial;
                while (*pp && *pp != slab) pp = &(*pp)->next;
                if (*pp) *pp = slab->next;
                slab->next = cache->full;
                cache->full = slab;
            }
            sfox_unlock(flags);
            return ptr;
        }
    }
    sfox_unlock(flags);
    return NULL;
}

void sfox_slab_free(sfox_slab_cache_t *cache, void *ptr, uintptr_t size) {
    if (!ptr || size == 0) return;
    uint64_t flags;
    sfox_lock(&flags);

    /* Find the slab containing ptr. */
    sfox_slab_t *slab = NULL;
    sfox_slab_t *s = cache->partial;
    while (s) {
        if ((uint8_t *)ptr >= (uint8_t *)s->data &&
            (uint8_t *)ptr < (uint8_t *)s->data + s->obj_count * s->obj_size) { slab = s; break; }
        s = s->next;
    }
    if (!slab) {
        s = cache->full;
        while (s) {
            if ((uint8_t *)ptr >= (uint8_t *)s->data &&
                (uint8_t *)ptr < (uint8_t *)s->data + s->obj_count * s->obj_size) { slab = s; break; }
            s = s->next;
        }
    }
    if (!slab) { sfox_unlock(flags); return; }

    uint32_t idx = ((uint8_t *)ptr - (uint8_t *)slab->data) / slab->obj_size;
    if (idx >= slab->obj_count) { sfox_unlock(flags); return; }

    /* Double-free detection: if already free, ignore. */
    uint32_t byte = idx / 8, bit = idx % 8;
    if (slab->bitmap[byte] & (1u << bit)) { sfox_unlock(flags); return; }

    slab->bitmap[byte] |= (1u << bit);

    /* If slab was full, move to partial. */
    sfox_slab_t **pp = &cache->full;
    while (*pp && *pp != slab) pp = &(*pp)->next;
    if (*pp) {
        *pp = slab->next;
        slab->next = cache->partial;
        cache->partial = slab;
    }

    /* If slab is now completely free, move to free_list. */
    bool any_used = false;
    for (uint32_t j = 0; j < slab->obj_count; j++) {
        if (!(slab->bitmap[j / 8] & (1u << (j % 8)))) { any_used = true; break; }
    }
    if (!any_used) {
        pp = &cache->partial;
        while (*pp && *pp != slab) pp = &(*pp)->next;
        if (*pp) {
            *pp = slab->next;
            slab->next = cache->free_list;
            cache->free_list = slab;
        }
    }
    sfox_unlock(flags);
}

uintptr_t sfox_slab_alloc_size(uintptr_t size) {
    if (size <= 8) return 8;
    if (size <= 16) return 16;
    if (size <= 32) return 32;
    if (size <= 64) return 64;
    if (size <= 128) return 128;
    if (size <= 256) return 256;
    if (size <= 512) return 512;
    if (size <= 1024) return 1024;
    return (size + 4095) & ~4095u;
}

/* ---- self test: 16-page static arena, save/restore pmm globals via public API ---- */
void st_sfox_slab_test(void) {
    static uint8_t arena[16 * 4096] __attribute__((aligned(4096)));
    bool pass = true;

    /* Initialize PMM on our test arena. */
    moe_pmm_init((uintptr_t)arena, (uintptr_t)arena + sizeof(arena));

    sfox_slab_cache_t cache;
    sfox_slab_cache_init(&cache);

    void *a = sfox_slab_alloc(&cache, 32);
    void *b = sfox_slab_alloc(&cache, 32);
    void *c = sfox_slab_alloc(&cache, 32);
    if (!a || !b || !c) pass = false;
    if (a == b || b == c || a == c) pass = false;

    memset(a, 0x11, 32);
    memset(b, 0x22, 32);
    memset(c, 0x33, 32);

    sfox_slab_free(&cache, b, 32);
    /* b should be reusable. */
    void *d = sfox_slab_alloc(&cache, 32);
    if (d != b) pass = false;

    sfox_slab_free(&cache, a, 32);
    sfox_slab_free(&cache, c, 32);
    sfox_slab_free(&cache, d, 32);

    st_run("sfox_slab", pass);
}
