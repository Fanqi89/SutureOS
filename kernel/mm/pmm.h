/* SutureOS module: physical page frame allocator (moe_ prefix)
 * Ported from: MoeOS (C:\Users\fanqi\Desktop\自研操作系统\MoeOS\kernel\src\mm\page.rs)
 * Original license: GPL-3.0
 * Changes: Rust Vec<u8> flags -> per-page 1-byte Taken/Last bitmap; first-fit
 *          scan; convergent init loop (fixes upstream OOB); alloc window
 *          i+n<=num_pages (fixes upstream off-by-one); panic -> silent
 *          NULL/ignore; O(1) free_count.
 */
#ifndef STITCH_PMM_H
#define STITCH_PMM_H

#include <stitch/types.h>

/* One byte per page: bit0 = Taken, bit1 = Last (end of a run). */
#define PMM_FLAG_TAKEN  0x01u
#define PMM_FLAG_LAST   0x02u

void     moe_pmm_init(uintptr_t start, uintptr_t end);
uintptr_t moe_pmm_alloc(uint32_t n);        /* n contiguous pages, NULL on fail */
void     moe_pmm_free(uintptr_t base, uint32_t n);
uint64_t moe_pmm_free_count(void);
uint64_t moe_pmm_total(void);
uintptr_t moe_pmm_alloc_page(void);          /* single page convenience wrapper */
void     moe_pmm_free_page(uintptr_t page);

void st_moe_pmm_test(void);

#endif /* STITCH_PMM_H */
