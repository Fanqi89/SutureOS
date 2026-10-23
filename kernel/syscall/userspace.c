/* SutureOS: per-process address spaces + copy-on-write. See userspace.h. */
#include "userspace.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "mmu.h"

/* The window is at 1 GiB: PML4 index 0 (0..512 GiB) and PDPT index 1 (1..2 GiB).
 * Index 0 also holds the identity map, so the private PDPT copies the kernel's
 * entry 0 and replaces only PDPT slot 1. */
#define USER_PML4_INDEX 0
#define USER_PDPT_INDEX 1

/* Everything except the physical address field (bits 51..12). Used to carry a
 * PTE's flags - including the NX bit at 63 - from the parent to the child. */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ull

extern uintptr_t moe_pmm_alloc(uint32_t n);
extern void moe_pmm_free(uintptr_t base, uint32_t n);

static uint64_t kernel_pml4_pa(void)
{
    return read_cr3() & ~0xFFFULL;
}

/* ---------------- frame reference map (keyed by physical frame) ----------------
 *
 * `fork` shares the window frames, so "who frees this page" can no longer be
 * answered from the owning userspace_t on its own. The map holds one entry per
 * *shared* window frame; a frame that is not in the table has exactly one owner.
 *
 * Sized for the worst case of PROC_MAX (kernel/syscall/process.h) processes that
 * all share nothing, plus headroom for the transient copies fork/exec make:
 * 8 * 16 = 128 live frames, and every process always has exactly 16. */
#define FRAME_REF_MAX 160
typedef struct {
    uint64_t pa;
    uint32_t refs;
} frame_ref_t;

static frame_ref_t g_frames[FRAME_REF_MAX];

static frame_ref_t *frame_find(uint64_t pa)
{
    pa &= PTE_ADDR_MASK;
    if (pa == 0) return NULL;
    for (int i = 0; i < FRAME_REF_MAX; i++)
        if (g_frames[i].refs != 0 && g_frames[i].pa == pa) return &g_frames[i];
    return NULL;
}

/* Register a frame that now has one owner (a fresh window or a fresh COW copy). */
static int frame_add(uint64_t pa)
{
    pa &= PTE_ADDR_MASK;
    if (pa == 0) return -1;
    if (frame_find(pa) != NULL) return 0;
    for (int i = 0; i < FRAME_REF_MAX; i++) {
        if (g_frames[i].refs == 0) {
            g_frames[i].pa = pa;
            g_frames[i].refs = 1;
            return 0;
        }
    }
    return -1;                                  /* table full */
}

/* One more owner of `pa`. Returns 0, or -1 when the table is full (fork then
 * fails instead of losing track of a shared frame). */
static int frame_ref(uint64_t pa)
{
    frame_ref_t *f;
    pa &= PTE_ADDR_MASK;
    if (pa == 0) return -1;
    f = frame_find(pa);
    if (f == NULL) return frame_add(pa);
    f->refs++;
    return 0;
}

/* One fewer owner. Returns the number of owners that remain; 0 means "nobody maps
 * it any more, free the frame". An untracked frame counts as a single owner. */
static uint32_t frame_unref(uint64_t pa)
{
    frame_ref_t *f;
    pa &= PTE_ADDR_MASK;
    if (pa == 0) return 0;
    f = frame_find(pa);
    if (f == NULL) return 0;
    if (f->refs > 1) return --f->refs;
    f->refs = 0;
    f->pa = 0;
    return 0;
}

uint32_t userspace_frame_refs(uint64_t pa)
{
    frame_ref_t *f = frame_find(pa);
    return (f != NULL) ? f->refs : 0;
}

/* ---------------- creation / fork ---------------- */

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
        us->page_pa[i] = pa + i * 4096u;
        if (frame_add(us->page_pa[i]) != 0) {
            for (uint64_t j = 0; j < i; j++) (void)frame_unref(us->page_pa[j]);
            moe_pmm_free((uintptr_t)pa, USERSPACE_PAGES);
            moe_pmm_free((uintptr_t)us->tables, 4);
            memset(us, 0, sizeof(*us));
            return -1;
        }
        pt[i] = us->page_pa[i] | flags;
    }

    us->pml4 = (uint64_t)(uintptr_t)pml4;
    us->pages = pa;
    us->pt = (uint64_t)(uintptr_t)pt;
    us->in_use = 1;
    return 0;
}

int userspace_fork_create(userspace_t *child, const userspace_t *parent)
{
    uint64_t *kern;
    uint64_t *cpml4, *cpdpt, *cpd, *cpt;
    uint64_t *ppt;
    uint64_t tables;

    if (child == NULL || parent == NULL || !parent->in_use || parent->pt == 0) return -1;
    memset(child, 0, sizeof(*child));

    kern = (uint64_t *)(uintptr_t)kernel_pml4_pa();
    ppt = (uint64_t *)(uintptr_t)parent->pt;

    tables = (uint64_t)moe_pmm_alloc(4);
    if (tables == 0) return -1;
    cpml4 = (uint64_t *)(uintptr_t)(tables + 0 * 4096u);
    cpdpt = (uint64_t *)(uintptr_t)(tables + 1 * 4096u);
    cpd   = (uint64_t *)(uintptr_t)(tables + 2 * 4096u);
    cpt   = (uint64_t *)(uintptr_t)(tables + 3 * 4096u);
    memset(cpml4, 0, 4096);
    memset(cpdpt, 0, 4096);
    memset(cpd, 0, 4096);
    memset(cpt, 0, 4096);
    child->tables = tables;

    /* kernel half shared, window chain private (same shape as userspace_create) */
    memcpy(cpml4, kern, 4096);
    cpml4[USER_PML4_INDEX] = (uint64_t)(uintptr_t)cpdpt | PTE_P | PTE_W | PTE_U;
    memcpy(cpdpt, (const void *)(uintptr_t)(kern[0] & ~0xFFFULL), 4096);
    cpd[0] = (uint64_t)(uintptr_t)cpt | PTE_P | PTE_W | PTE_U;
    cpdpt[USER_PDPT_INDEX] = (uint64_t)(uintptr_t)cpd | PTE_P | PTE_W | PTE_U;

    /* Every window page is shared: both sides lose PTE_W and gain PTE_COW, and the
     * frame gains a second owner. Reference counting has to succeed for *every*
     * page or the fork is undone - a half-shared window would double-free. */
    for (uint64_t i = 0; i < USERSPACE_PAGES; i++) {
        const uint64_t pa = parent->page_pa[i];
        const uint64_t flags = ppt[i] & ~PTE_ADDR_MASK;   /* P U A D G COW NX */
        const uint64_t shared = (flags & ~PTE_W) | PTE_COW | PTE_P | PTE_U;

        if (pa == 0 || frame_ref(pa) != 0) {
            for (uint64_t j = 0; j < i; j++) (void)frame_unref(parent->page_pa[j]);
            moe_pmm_free((uintptr_t)tables, 4);
            memset(child, 0, sizeof(*child));
            return -1;
        }
        child->page_pa[i] = pa;
        cpt[i] = (pa & ~0xFFFULL) | shared;
        /* the parent must fault on its next write too */
        ppt[i] = (pa & ~0xFFFULL) | shared;
    }

    mmu_flush_tlb();

    child->pml4 = (uint64_t)(uintptr_t)cpml4;
    child->pages = parent->pages;
    child->pt = (uint64_t)(uintptr_t)cpt;
    child->in_use = 1;
    return 0;
}

int userspace_load(userspace_t *us, const uint8_t *blob, uint64_t len)
{
    uint64_t done = 0;

    if (us == NULL || !us->in_use || blob == NULL || len == 0) return -1;
    if (len > (uint64_t)USERSPACE_CODE_PAGES * 4096u) return -1;

    /* A page that is still shared with another process must become private before
     * its content is replaced, otherwise execve() would rewrite a sibling's text. */
    for (uint64_t i = 0; i < USERSPACE_PAGES; i++) {
        const uint64_t pa = us->page_pa[i];
        if (pa == 0) return -1;
        if (userspace_frame_refs(pa) > 1) {
            const uint64_t fresh = (uint64_t)moe_pmm_alloc(1);
            uint64_t flags;
            if (fresh == 0) return -1;
            memcpy((void *)(uintptr_t)fresh, (const void *)(uintptr_t)pa, 4096);
            if (frame_add(fresh) != 0) { moe_pmm_free((uintptr_t)fresh, 1); return -1; }
            (void)frame_unref(pa);
            us->page_pa[i] = fresh;
            flags = ((uint64_t *)(uintptr_t)us->pt)[i] & ~PTE_ADDR_MASK;
            ((uint64_t *)(uintptr_t)us->pt)[i] =
                fresh | ((flags & ~PTE_COW) | PTE_P | PTE_U | PTE_W);
        }
        memset((void *)(uintptr_t)us->page_pa[i], 0, 4096);
    }

    /* The window pages are identity mapped in the kernel, so write them directly. */
    for (uint64_t i = 0; i < USERSPACE_PAGES && done < len; i++) {
        uint64_t n = len - done;
        if (n > 4096) n = 4096;
        memcpy((void *)(uintptr_t)us->page_pa[i], blob + done, (size_t)n);
        done += n;
    }
    mmu_flush_tlb();
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
    for (uint64_t i = 0; i < USERSPACE_PAGES; i++) {
        const uint64_t pa = us->page_pa[i];
        if (pa == 0) continue;
        if (frame_unref(pa) == 0) moe_pmm_free((uintptr_t)pa, 1);
        us->page_pa[i] = 0;
    }
    if (us->tables != 0) moe_pmm_free((uintptr_t)us->tables, 4);
    memset(us, 0, sizeof(*us));
}

int userspace_cow_fault(userspace_t *us, uint64_t fault_va)
{
    uint64_t idx;
    uint64_t *pt;
    uint64_t pte, pa, flags;
    uint32_t refs;

    if (us == NULL || !us->in_use || us->pt == 0) return -1;
    if (fault_va < USERSPACE_VA || fault_va >= USERSPACE_VA + USERSPACE_BYTES) return -1;

    idx = (fault_va - USERSPACE_VA) / 4096u;
    if (idx >= (uint64_t)USERSPACE_PAGES) return -1;
    pt = (uint64_t *)(uintptr_t)us->pt;
    pte = pt[idx];
    if ((pte & PTE_P) == 0 || (pte & PTE_COW) == 0 || (pte & PTE_U) == 0) return -1;

    pa = us->page_pa[idx];
    if (pa == 0) return -1;
    flags = pte & ~PTE_ADDR_MASK;
    refs = userspace_frame_refs(pa);

    if (refs > 1) {
        /* somebody else still maps this frame: give this process its own copy */
        const uint64_t fresh = (uint64_t)moe_pmm_alloc(1);
        if (fresh == 0) return -1;
        memcpy((void *)(uintptr_t)fresh, (const void *)(uintptr_t)pa, 4096);
        if (frame_add(fresh) != 0) { moe_pmm_free((uintptr_t)fresh, 1); return -1; }
        (void)frame_unref(pa);
        us->page_pa[idx] = fresh;
        pt[idx] = (fresh & ~0xFFFULL) | ((flags & ~PTE_COW) | PTE_P | PTE_U | PTE_W);
    } else {
        /* last reference (or an untracked frame): the content is already private,
         * so just drop the write protection - no copy at all. This is the "other
         * side was the last reference" shortcut: PTE_COW is per-mapping, the
         * reference count is the truth. */
        pt[idx] = (pa & ~0xFFFULL) | ((flags & ~PTE_COW) | PTE_P | PTE_U | PTE_W);
    }

    mmu_flush_tlb();
    if ((pt[idx] & (PTE_P | PTE_W)) != (PTE_P | PTE_W) || (pt[idx] & PTE_COW) != 0)
        return -1;
    return 0;
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
        ((uint8_t *)(uintptr_t)a.page_pa[0])[0] = 0xA5;
        ((uint8_t *)(uintptr_t)b.page_pa[0])[0] = 0x5A;
        st_report("windows are physically distinct",
                  ((uint8_t *)(uintptr_t)a.page_pa[0])[0] == 0xA5 &&
                  ((uint8_t *)(uintptr_t)b.page_pa[0])[0] == 0x5A);
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
