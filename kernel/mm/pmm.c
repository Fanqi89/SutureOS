/* SutureOS module: physical page frame allocator (moe_ prefix)
 * Ported from: MoeOS (C:\Users\fanqi\Desktop\自研操作系统\MoeOS\kernel\src\mm\page.rs)
 * Original license: GPL-3.0
 * Changes: Rust Vec<u8> flags -> per-page 1-byte Taken/Last bitmap; first-fit
 *          scan; convergent init loop (fixes upstream OOB); alloc window
 *          i+n<=num_pages (fixes upstream off-by-one); panic -> silent
 *          NULL/ignore; O(1) free_count.
 */
#include "pmm.h"
#include <stitch/string.h>
#include <stitch/selftest.h>

#define PAGE_SIZE 4096u

static uint8_t *pmm_flags;      /* 1 byte per page */
static uintptr_t pmm_start;     /* first page-aligned byte */
static uint64_t  pmm_num_pages;
static uint64_t  pmm_free_count;

void moe_pmm_init(uintptr_t start, uintptr_t end) {
    /* Align start up, end down. */
    start = (start + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    end   = end & ~(uintptr_t)(PAGE_SIZE - 1);
    if (end <= start) { pmm_num_pages = 0; pmm_free_count = 0; return; }

    /* Convergent loop: shrink page count until flag area + data area fit in
     * [start, end). Fixes upstream OOB when end-start is not page-aligned. */
    uint64_t np = (end - start) / PAGE_SIZE;
    for (;;) {
        uint64_t flag_bytes = np;               /* 1 byte per page */
        uintptr_t data_start = start + flag_bytes;
        if (data_start >= end) { np--; if (np == 0) break; continue; }
        uint64_t data_pages = (end - data_start) / PAGE_SIZE;
        if (data_pages >= np) break;
        np = data_pages;
        if (np == 0) break;
    }
    pmm_num_pages  = np;
    pmm_start      = start;
    pmm_flags      = (uint8_t *)start;
    memset(pmm_flags, 0, np);

    /* CRITICAL: the flag array itself occupies the first ceil(np/4096) pages of
     * the region, and moe_pmm_alloc() hands out pages starting at index 0.
     * Without this reservation the very first allocation (the kernel heap in
     * kmain: lemis_heap_init(moe_pmm_alloc(2048), 8 MiB)) returns the flag
     * array itself, so every later alloc/free writes 0x01/0x00 flag bytes
     * straight into the live heap - corrupting the heap header, flanterm's
     * context (invisible text: text_fg became 0x00000001) and any AP/task
     * stack allocated afterwards. Mark the flag pages as taken and do not
     * count them as free. */
    uint64_t flag_pages = (np + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < flag_pages && i < np; i++)
        pmm_flags[i] = PMM_FLAG_TAKEN;
    pmm_free_count = (np > flag_pages) ? (np - flag_pages) : 0;
}

uintptr_t moe_pmm_alloc(uint32_t n) {
    if (n == 0 || pmm_num_pages == 0) return 0;
    if ((uint64_t)n > pmm_num_pages) return 0;

    for (uint64_t i = 0; i + n <= pmm_num_pages; i++) {
        /* Skip taken pages quickly. */
        if (pmm_flags[i] & PMM_FLAG_TAKEN) continue;

        /* Check n consecutive free pages. */
        uint32_t j;
        for (j = 0; j < n; j++) {
            if (pmm_flags[i + j] & PMM_FLAG_TAKEN) break;
        }
        if (j < n) { i += j; continue; }   /* skip past the taken page */

        /* Found a run. */
        for (j = 0; j < n; j++) {
            pmm_flags[i + j] = PMM_FLAG_TAKEN;
            if (j == n - 1) pmm_flags[i + j] |= PMM_FLAG_LAST;
        }
        pmm_free_count -= n;
        return pmm_start + (uintptr_t)(i * PAGE_SIZE);
    }
    return 0;
}

void moe_pmm_free(uintptr_t base, uint32_t n) {
    if (n == 0 || base < pmm_start) return;
    uint64_t idx = (base - pmm_start) / PAGE_SIZE;
    if (idx >= pmm_num_pages) return;

    for (uint32_t j = 0; j < n && idx + j < pmm_num_pages; j++) {
        if (pmm_flags[idx + j] & PMM_FLAG_TAKEN) {
            pmm_flags[idx + j] = 0;
            pmm_free_count++;
        }
    }
}

uint64_t moe_pmm_free_count(void) { return pmm_free_count; }
uint64_t moe_pmm_total(void)      { return pmm_num_pages; }

uintptr_t moe_pmm_alloc_page(void) { return moe_pmm_alloc(1); }
void moe_pmm_free_page(uintptr_t page) { moe_pmm_free(page, 1); }

/* ---- self test: 24-page 4096-aligned static arena, save/restore globals ---- */
void st_moe_pmm_test(void) {
    static uint8_t arena[24 * 4096] __attribute__((aligned(4096)));
    static uint8_t saved_flags[24];
    uintptr_t saved_start; uint64_t saved_np, saved_fc;
    bool pass = true;

    saved_start = pmm_start; saved_np = pmm_num_pages; saved_fc = pmm_free_count;
    memcpy(saved_flags, pmm_flags, 24);

    moe_pmm_init((uintptr_t)arena, (uintptr_t)arena + sizeof(arena));
    if (pmm_num_pages != 23) pass = false;

    /* The flag array lives at the start of the region, i.e. inside its first
     * page, so that page must be reserved and never handed out. This is a
     * regression test for the bug where moe_pmm_alloc(2048) returned the flag
     * array itself and the kernel heap ended up overlapping it. */
    uint64_t flag_pages = (23 + 4095) / 4096;
    if (pmm_free_count != 23 - flag_pages) pass = false;
    if (!(pmm_flags[0] & PMM_FLAG_TAKEN)) pass = false;

    /* alloc 3 pages -> contiguous, starts after the reserved flag page(s). */
    uintptr_t a = moe_pmm_alloc(3);
    if (a == 0 || a != (uintptr_t)arena + flag_pages * 4096) pass = false;
    if (pmm_free_count != 23 - flag_pages - 3) pass = false;
    if (pmm_flags[flag_pages] != PMM_FLAG_TAKEN ||
        pmm_flags[flag_pages + 1] != PMM_FLAG_TAKEN ||
        pmm_flags[flag_pages + 2] != (PMM_FLAG_TAKEN | PMM_FLAG_LAST)) pass = false;

    /* alloc 1 page -> next free. */
    uintptr_t b = moe_pmm_alloc(1);
    if (b != a + 3 * 4096) pass = false;

    /* free the 3-page run (the single page allocated as `b` is still held). */
    moe_pmm_free(a, 3);
    if (pmm_free_count != 23 - flag_pages - 1) pass = false;
    if (pmm_flags[flag_pages] || pmm_flags[flag_pages + 1] ||
        pmm_flags[flag_pages + 2]) pass = false;
    if (!(pmm_flags[0] & PMM_FLAG_TAKEN)) pass = false;   /* still reserved */

    /* alloc 2 pages -> reuses the freed run. */
    uintptr_t c = moe_pmm_alloc(2);
    if (c != a) pass = false;

    /* restore. */
    pmm_start = saved_start; pmm_num_pages = saved_np; pmm_free_count = saved_fc;
    memcpy(pmm_flags, saved_flags, 24);

    st_run("moe_pmm", pass);
}