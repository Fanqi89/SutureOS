/* SutureOS module: kernel heap allocator (lemis_ prefix)
 * Ported from: Lemis (C:\Users\fanqi\Desktop\自研操作系统\Lemis\kernel\src\mm\heap.rs)
 * Original license: GPL-3.0
 * Changes: upstream long long* pointer arithmetic (x8, merge never worked)
 *          -> byte uintptr_t arithmetic; 4->16 byte alignment; block header
 *          with explicit pad = 32 bytes total (payload guaranteed 16-aligned);
 *          split remainder threshold HEADER+16; start==NULL falls back to
 *          built-in 64KB arena; init size underflow guard.
 *          Fixed: allocator now maintains separate free list (used=0 blocks only)
 */
#include "heap.h"
#include <stitch/string.h>
#include <stitch/selftest.h>

#define LEMIS_ALIGN 16u

typedef struct lemis_block {
    struct lemis_block *next;
    uintptr_t size;          /* payload size, excluding header+pad */
    uint8_t   pad[14];       /* header+pad = 32 bytes, payload 16-aligned */
    uint8_t   used;          /* 1 = allocated, 0 = free */
    uint8_t   _pad2;         /* ensure 16-byte alignment of payload */
} lemis_block_t;

static lemis_block_t *lemis_free_head;  /* head of FREE blocks list */
static uint8_t lemis_arena[64 * 1024] __attribute__((aligned(16)));

static inline uintptr_t lemis_align_up(uintptr_t v) {
    return (v + LEMIS_ALIGN - 1) & ~(uintptr_t)(LEMIS_ALIGN - 1);
}

void lemis_heap_init(void *start, uintptr_t size) {
    if (start == NULL || size < sizeof(lemis_block_t) + LEMIS_ALIGN) {
        start = lemis_arena;
        size  = sizeof(lemis_arena);
    }
    lemis_free_head = (lemis_block_t *)start;
    lemis_free_head->next = NULL;
    lemis_free_head->size = size - sizeof(lemis_block_t);
    lemis_free_head->used = 0;
}

void *lemis_heap_alloc(uintptr_t size) {
    if (size == 0) return NULL;
    size = lemis_align_up(size);

    lemis_block_t **pp = &lemis_free_head;
    lemis_block_t *b = lemis_free_head;

    while (b) {
        if (!b->used && b->size >= size) {
            /* Split if remainder can hold a header + at least 16 bytes. */
            if (b->size >= size + sizeof(lemis_block_t) + LEMIS_ALIGN) {
                lemis_block_t *nb = (lemis_block_t *)((uint8_t *)b + sizeof(lemis_block_t) + size);
                nb->next = b->next;
                nb->size = b->size - size - sizeof(lemis_block_t);
                nb->used = 0;
                b->next = nb;
                b->size = size;
            }
            /* Remove from free list */
            *pp = b->next;
            b->used = 1;
            return (uint8_t *)b + sizeof(lemis_block_t);
        }
        pp = &b->next;
        b = b->next;
    }
    return NULL;
}

void lemis_heap_free(void *ptr) {
    if (ptr == NULL) return;
    lemis_block_t *b = (lemis_block_t *)((uint8_t *)ptr - sizeof(lemis_block_t));
    b->used = 0;

    /* Insert into address-ordered free list. */
    lemis_block_t **pp = &lemis_free_head;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;

    /* Coalesce with next. */
    if (b->next && (uint8_t *)b + sizeof(lemis_block_t) + b->size == (uint8_t *)b->next) {
        b->size += sizeof(lemis_block_t) + b->next->size;
        b->next = b->next->next;
    }
    /* Coalesce with prev. */
    if (pp != &lemis_free_head) {
        lemis_block_t *p = lemis_free_head;
        while (p->next && p->next != b) p = p->next;
        if (p->next == b && (uint8_t *)p + sizeof(lemis_block_t) + p->size == (uint8_t *)b) {
            p->size += sizeof(lemis_block_t) + b->size;
            p->next = b->next;
        }
    }
}

uintptr_t lemis_heap_free_space(void) {
    uintptr_t total = 0;
    for (lemis_block_t *b = lemis_free_head; b; b = b->next) total += b->size;
    return total;
}

/* ---- self test: 8KB static arena, save/restore head ---- */
void st_lemis_heap_test(void) {
    static uint8_t arena[8192] __attribute__((aligned(16)));
    lemis_block_t *saved_head = lemis_free_head;
    bool pass = true;

    lemis_heap_init(arena, sizeof(arena));
    if (lemis_free_head != (lemis_block_t *)arena) pass = false;

    void *a = lemis_heap_alloc(100);
    void *b = lemis_heap_alloc(200);
    void *c = lemis_heap_alloc(50);
    if (!a || !b || !c) pass = false;
    if ((uintptr_t)a % 16 || (uintptr_t)b % 16 || (uintptr_t)c % 16) pass = false;

    /* Write patterns, free b, verify a and c intact. */
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    memset(c, 0xCC, 50);
    lemis_heap_free(b);
    bool intact = true;
    for (int i = 0; i < 100; i++) if (((uint8_t *)a)[i] != 0xAA) intact = false;
    for (int i = 0; i < 50; i++)  if (((uint8_t *)c)[i] != 0xCC) intact = false;
    if (!intact) pass = false;

    /* Realloc into the freed slot. */
    void *d = lemis_heap_alloc(150);
    if (!d) pass = false;

    lemis_heap_free(a);
    lemis_heap_free(c);
    lemis_heap_free(d);

    /* After freeing all, one big block should remain. */
    if (lemis_free_head->next != NULL) pass = false;

    lemis_free_head = saved_head;
    st_run("lemis_heap", pass);
}