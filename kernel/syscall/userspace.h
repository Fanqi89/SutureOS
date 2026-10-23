/* SutureOS: per-process user address space (task 2 of "complete userspace").
 *
 * Until now the ring-3 programs lived in the kernel's own page tables: the user
 * pages at 0x40000000 were simply marked U/S=1 there. That cannot work for more
 * than one program (fork/exec need a private address space) and it is not what an
 * OS does. This module builds a real per-process PML4:
 *
 *   - the kernel half is shared (SutureOS is a flat identity-mapped kernel, so the
 *     user PML4 keeps entries 0..511 pointing at the same tables);
 *   - the 1 GiB..2 GiB PML4 slot (which is free in the kernel map - RAM ends far
 *     below it) gets a PRIVATE PDPT/PD/PT chain for the user window at
 *     0x40000000, so two processes can both use that VA and see different pages.
 *
 * Switching CR3 is safe in both directions because the kernel map is shared: the
 * kernel keeps running normally while a user PML4 is active (that is what the
 * syscall entry does), and ring-3 code can only touch pages whose PTE has U=1.
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
 * and the window's PT so execve can remap it. */
typedef struct {
    uint64_t pml4;                  /* physical address of the PML4 */
    uint64_t pages;                 /* physical base of the window pages */
    uint64_t pt;                    /* physical address of the window's PT */
    uint64_t tables;                /* physical base of the 4 page-table pages */
    int      in_use;
} userspace_t;

/* Build a fresh address space with `pages` pages allocated for the window.
 * Returns 0 on success, -1 when the page pools are exhausted. */
int userspace_create(userspace_t *us);

/* Copy a flat program blob into the code pages, zero the rest, and reset the
 * stack pointer area. Returns 0 on success. */
int userspace_load(userspace_t *us, const uint8_t *blob, uint64_t len);

/* User entry/stack top inside the window. */
uint64_t userspace_entry(const userspace_t *us);
uint64_t userspace_stack_top(const userspace_t *us);

/* Release the address space back to the PMM and drop the mapping. */
void userspace_destroy(userspace_t *us);

/* Self test: two address spaces both at 0x40000000 with different contents, the
 * kernel map present in both, and only the window marked user-accessible. */
int userspace_selftest(void);

#endif /* SUTUREOS_USERSPACE_H */
