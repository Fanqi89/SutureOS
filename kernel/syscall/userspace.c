/* SutureOS: per-process address spaces. See userspace.h for the layout rationale. */
#include "userspace.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "mmu.h"

/* The window is at 1 GiB: PML4 index 0 (0..512 GiB) and PDPT index 1 (1..2 GiB).
 * Index 0 also holds the identity map, so the private PDPT copies the kernel's
 * entry 0 and replaces only PDPT slot 1. */
#define USER_PML4_INDEX 0
#define USER_PDPT_INDEX 1

extern uintptr_t moe_pmm_alloc(uint32_t n);
extern void moe_pmm_free(uintptr_t base, uint32_t n);

static uint64_t kernel_pml4_pa(void)
{
    return read_cr3() & ~0xFFFULL;
}

int userspace_create(userspace_t *us)
{
    uint64_t *kern;
    uint64_t *pml4;
    uint64_t *kpdpt;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t pa;

    if (us == NULL) return -1;
    memset(us, 0, sizeof(*us));

    kern = (uint64_t *)(uintptr_t)kernel_pml4_pa();

    /* The window's physical pages and the three page-table pages must come from
     * the PMM in one piece each (identity mapped, so we can write them directly). */
    pa = (uint64_t)moe_pmm_alloc(USERSPACE_PAGES);
    if (pa == 0) return -1;

    /* The four page-table pages come from the PMM too (identity mapped, so they can
     * be written directly) and are released in userspace_destroy(); the static MMU
     * pool only has a handful of pages and would run out after a few programs. */
    {
        uint64_t tables = (uint64_t)moe_pmm_alloc(4);
        if (tables == 0) { moe_pmm_free((uintptr_t)pa, USERSPACE_PAGES); return -1; }
        pml4 = (uint64_t *)(uintptr_t)(tables + 0 * 4096u);
        pdpt = (uint64_t *)(uintptr_t)(tables + 1 * 4096u);
        pd   = (uint64_t *)(uintptr_t)(tables + 2 * 4096u);
        pt   = (uint64_t *)(uintptr_t)(tables + 3 * 4096u);
        memset(pml4, 0, 4096);
        memset(pdpt, 0, 4096);
        memset(pd, 0, 4096);
        memset(pt, 0, 4096);
        us->tables = tables;
    }

    /* 1. share the whole kernel map (identity map + MMIO windows) */
    memcpy(pml4, kern, 4096);
    pml4[USER_PML4_INDEX] = (uint64_t)(uintptr_t)pdpt | PTE_P | PTE_W | PTE_U;

    /* 2. copy the kernel's low PDPT (identity map 0..1 GiB) into the private one */
    kpdpt = (uint64_t *)(uintptr_t)(kern[0] & ~0xFFFULL);
    memcpy(pdpt, kpdpt, 4096);
    pdpt[USER_PDPT_INDEX] = (uint64_t)(uintptr_t)pd | PTE_P | PTE_W | PTE_U;

    /* 3. the PD/PT chain for the window: 0x40000000 is PD index 0, PT index 0..15 */
    memset(pd, 0, 4096);
    memset(pt, 0, 4096);
    pd[0] = (uint64_t)(uintptr_t)pt | PTE_P | PTE_W | PTE_U;
    for (uint64_t i = 0; i < USERSPACE_PAGES; i++) {
        const int is_stack = (i >= (uint64_t)USERSPACE_CODE_PAGES);
        uint64_t flags = PTE_P | PTE_U | PTE_W;
        if (is_stack) flags |= PTE_NX;             /* stack is not executable */
        pt[i] = (pa + i * 4096u) | flags;
    }

    us->pml4 = (uint64_t)(uintptr_t)pml4;
    us->pages = pa;
    us->pt = (uint64_t)(uintptr_t)pt;
    us->in_use = 1;
    return 0;
}

int userspace_load(userspace_t *us, const uint8_t *blob, uint64_t len)
{
    if (us == NULL || !us->in_use || blob == NULL || len == 0) return -1;
    if (len > (uint64_t)USERSPACE_CODE_PAGES * 4096u) return -1;

    /* The window pages are identity mapped in the kernel, so write them directly. */
    memset((void *)(uintptr_t)us->pages, 0, (size_t)USERSPACE_BYTES);
    memcpy((void *)(uintptr_t)us->pages, blob, (size_t)len);
    return 0;
}

uint64_t userspace_entry(const userspace_t *us)
{
    (void)us;
    return USERSPACE_VA;
}

uint64_t userspace_stack_top(const userspace_t *us)
{
    (void)us;
    return USERSPACE_VA + USERSPACE_BYTES - 16u;
}

void userspace_destroy(userspace_t *us)
{
    if (us == NULL || !us->in_use) return;
    if (us->pages != 0) moe_pmm_free((uintptr_t)us->pages, USERSPACE_PAGES);
    if (us->tables != 0) moe_pmm_free((uintptr_t)us->tables, 4);
    memset(us, 0, sizeof(*us));
}

/* ---------------- acceptance ---------------- */

static uint32_t g_checks;
static uint32_t g_failed;

static void st_report(const char *what, int ok)
{
    g_checks++;
    if (!ok) g_failed++;
    console_printf("userspace selftest: %s ... %s\n", what, ok ? "PASS" : "FAIL");
}

int userspace_selftest(void)
{
    userspace_t a, b;
    g_checks = 0;
    g_failed = 0;

    st_report("address space A created", userspace_create(&a) == 0);
    st_report("address space B created", userspace_create(&b) == 0);
    st_report("both use the same user VA but different pages",
              a.pml4 != b.pml4 && a.pages != b.pages && a.pt != b.pt);

    /* write different bytes into the two windows */
    if (a.in_use && b.in_use) {
        ((uint8_t *)(uintptr_t)a.pages)[0] = 0xA5;
        ((uint8_t *)(uintptr_t)b.pages)[0] = 0x5A;
        st_report("windows are physically distinct",
                  ((uint8_t *)(uintptr_t)a.pages)[0] == 0xA5 &&
                  ((uint8_t *)(uintptr_t)b.pages)[0] == 0x5A);
    }

    /* the kernel identity map must be shared so the kernel can keep running */
    if (a.in_use) {
        uint64_t *kern = (uint64_t *)(uintptr_t)kernel_pml4_pa();
        uint64_t *pml4 = (uint64_t *)(uintptr_t)a.pml4;
        /* slot 0 points at a private PDPT (so the window can be private), but that
         * PDPT's own entries are copies of the kernel's identity map */
        {
            uint64_t *updpt = (uint64_t *)(uintptr_t)(pml4[0] & ~0xFFFULL);
            uint64_t *kpdpt = (uint64_t *)(uintptr_t)(kern[0] & ~0xFFFULL);
            st_report("kernel identity map is visible from the user address space",
                      (updpt[0] & ~0xFFFULL) == (kpdpt[0] & ~0xFFFULL));
        }
        st_report("user window PDPT slot differs from the kernel's",
                  ((uint64_t *)(uintptr_t)(pml4[0] & ~0xFFFULL))[USER_PDPT_INDEX] !=
                  ((uint64_t *)(uintptr_t)(kern[0] & ~0xFFFULL))[USER_PDPT_INDEX]);
        st_report("window PTE is user accessible",
                  (((uint64_t *)(uintptr_t)a.pt)[0] & (PTE_P | PTE_U)) == (PTE_P | PTE_U));
        st_report("window stack PTE is non-executable",
                  (((uint64_t *)(uintptr_t)a.pt)[USERSPACE_CODE_PAGES] & PTE_NX) != 0);
    }

    userspace_destroy(&a);
    userspace_destroy(&b);
    st_report("address spaces released", !a.in_use && !b.in_use);

    console_printf("userspace selftest: RESULT %s (%u/%u)\n",
                   (g_failed == 0) ? "PASS" : "FAIL",
                   g_checks - g_failed, g_checks);
    return (g_failed == 0) ? 0 : 1;
}
