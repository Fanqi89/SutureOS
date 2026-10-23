/* SutureOS: per-process user address space.
 *
 * The ring-3 programs live at a fixed virtual address (0x40000000, see
 * user/lib/user.ld) inside their own PML4:
 *
 *   - the kernel half is shared (SutureOS is a flat identity-mapped kernel, so the
 *     user PML4 keeps entries 0..511 pointing at the same tables);
 *   - the 1 GiB..2 GiB PML4 slot (which is free in the kernel map - RAM ends far
 *     below it) gets a PRIVATE PDPT/PD/PT chain for the user window at
 *     0x40000000, so two processes can both use that VA and see different pages.
 *
 * Fork shares the window's 16 physical frames instead of copying them: both PTEs
 * are made read-only and marked PTE_COW (bit 9), the frame's reference count is
 * incremented, and the first write from either side is resolved by
 * userspace_cow_fault() out of the #PF handler. The frames are counted in a small
 * table inside this module, so userspace_destroy() hands a shared frame back to
 * the PMM exactly once - without the count, a parent and a child that still share
 * their window would each free the same 16 pages (double free).
 */
#ifndef SUTUREOS_USERSPACE_H
#define SUTUREOS_USERSPACE_H

#include <stitch/types.h>

#define USERSPACE_VA        0x40000000ull   /* window base (user/lib/user.ld) */
#define USERSPACE_CODE_PAGES 8
#define USERSPACE_STACK_PAGES 8
#define USERSPACE_PAGES     (USERSPACE_CODE_PAGES + USERSPACE_STACK_PAGES)
#define USERSPACE_BYTES     (USERSPACE_PAGES * 4096ull)

/* A user address space: the PML4 to load, the physical pages behind the window,
 * and the window's PT so execve/fork can remap it.
 *
 * page_pa[i] is the frame *currently* backing window page i. It starts as
 * pages + i*4096 and changes when a COW fault (or execve) gives the process a
 * private copy, so `pages` alone is not enough to describe the window. */
typedef struct {
    uint64_t pml4;                  /* physical address of the PML4 */
    uint64_t pages;                 /* physical base of the original window run */
    uint64_t pt;                    /* physical address of the window's PT */
    uint64_t tables;                /* physical base of the 4 page-table pages */
    uint64_t page_pa[USERSPACE_PAGES]; /* frame behind each window page (now) */
    int      in_use;
} userspace_t;

/* Build a fresh address space with `pages` pages allocated for the window.
 * Returns 0 on success, -1 when the page pools are exhausted. */
int userspace_create(userspace_t *us);

/* fork(): build `child` as a copy-on-write clone of `parent`. Both sides' window
 * PTEs end up read-only + PTE_COW, every shared frame's reference count is
 * incremented, and the child gets its own PML4/PDPT/PD/PT. Returns 0 or -1. */
int userspace_fork_create(userspace_t *child, const userspace_t *parent);

/* Copy a flat program blob into the code pages, zero the rest, and reset the
 * stack pointer area. Breaks any COW sharing first, so an execve() in one process
 * can never rewrite a sibling's shared window. Returns 0 on success. */
int userspace_load(userspace_t *us, const uint8_t *blob, uint64_t len);

/* User entry/stack top inside the window. */
uint64_t userspace_entry(const userspace_t *us);
uint64_t userspace_stack_top(const userspace_t *us);

/* Release the address space: drop one reference from every window frame (freeing
 * the frames whose count reaches zero) and return the 4 page-table pages. */
void userspace_destroy(userspace_t *us);

/* Resolve a copy-on-write fault on `fault_va` for `us`: give the faulting process
 * a private frame, clear PTE_COW and set PTE_W. When the frame's reference count
 * is already 1 (the other side copied earlier) the page is simply made writable
 * again - the "last reference" shortcut, no copy at all.
 * Returns 0 when the mapping is now writable (the faulting instruction can be
 * retried), -1 when this address was not a COW fault. */
int userspace_cow_fault(userspace_t *us, uint64_t fault_va);

/* Reference count of a window frame (0 when the frame is not tracked, i.e. it has
 * exactly one owner). Exposed for the acceptante self-tests. */
uint32_t userspace_frame_refs(uint64_t pa);

/* Self test: two address spaces both at 0x40000000 with different contents, the
 * kernel map present in both, and only the window marked user-accessible. */
int userspace_selftest(void);

#endif /* SUTUREOS_USERSPACE_H */
