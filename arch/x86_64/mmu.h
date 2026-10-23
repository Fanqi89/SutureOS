/* SutureOS: x86_64 MMU for EL0 isolation */
#ifndef STITCH_MMU_H
#define STITCH_MMU_H

#include <stitch/types.h>

/* 克隆内核页表，为 EL0 创建用户 TTBR0
 * kernel_pml4: 当前内核 PML4 物理地址 (已 1:1 映射)
 * out_user_pml4: 输出用户 PML4 物理地址
 * base/len: 用户镜像/栈/参数区域，将被设置为 U/S=1
 * 返回 0 成功，-1 失败 */
int mmu_clone_for_el0(uint64_t kernel_pml4, uint64_t *out_user_pml4,
                      uint64_t base, uint64_t len);

/* 释放用户页表 */
void mmu_free_user_pml4(uint64_t user_pml4);

/* 读取当前 CR3 */
static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* 页表常量 */
#define PTE_P           (1ULL << 0)   /* Present */
#define PTE_W           (1ULL << 1)   /* Write */
#define PTE_U           (1ULL << 2)   /* User */
#define PTE_PWT         (1ULL << 3)
#define PTE_PCD         (1ULL << 4)
#define PTE_A           (1ULL << 5)
#define PTE_D           (1ULL << 6)
#define PTE_PS          (1ULL << 7)   /* Page Size (1GB/2MB) */
#define PTE_G           (1ULL << 8)
#define PTE_NX          (1ULL << 63)  /* No-execute */

/* Copy-on-write marker. x86_64 leaves bits 9..11 available to software, so the
 * flag lives in PTE bit 9 and the hardware ignores it. A COW page is always
 * present, read-only and mapped in two (or more) address spaces; the #PF handler
 * in kernel/interrupts/intr.c resolves it through kernel/syscall/userspace.c.
 * Both the PTE and the frame reference map are owned by userspace.c - the old
 * half-finished helpers in this file copied pages out of the static MMU pool and
 * had no reference counting, so they could double-free a 16-page window. */
#define PTE_COW         (1ULL << 9)

/* 页表遍历/分配 */
uint64_t *mmu_walk(uint64_t *table, int level, uint64_t vaddr, int create);
void *loader_alloc_page(void);

/* Map [phys, phys+bytes) at the SAME virtual addresses using 2 MiB pages.
 * Needed for MMIO/framebuffer windows outside the boot identity map (e.g. a
 * linear framebuffer at 0xFD000000). Returns 0 on success, -1 on failure. */
int mmu_map_mmio_identity(uint64_t phys, uint64_t bytes);

/* Expose the identity-mapped range [va, va+len) to ring 3 by setting U/S=1 on
 * the 4 KiB pages (and on their parent tables). 2 MiB pages are split into a
 * page table first, so only the requested pages become user accessible - the
 * caller must own the physical pages (i.e. they came from moe_pmm_alloc()),
 * otherwise this hands kernel memory to user mode.
 *
 * writable != 0 sets PTE_W; executable != 0 clears PTE_NX.
 * Returns 0 on success, -1 when a parent entry is missing or out of pool. */
int mmu_map_user_identity(uint64_t va, uint64_t len, int writable, int executable);

/* Map [va, va+pages*4KiB) to the physical pages starting at pa, with U/S=1.
 * Used to run a program that was linked at a fixed VA. */
int mmu_map_user_pages(uint64_t va, uint64_t pa, uint64_t pages,
                       int writable, int executable);

/* Zeroed 4 KiB page-table page from the MMU pool. */
void *mmu_alloc_table(void);

/* Reload CR3 (flush the TLB) after rewriting PTEs. */
void mmu_flush_tlb(void);

/* Create the intermediate page tables for a user VA range. */
int mmu_prepare_user_range(uint64_t va, uint64_t pages);

/* Copy-on-write: fork and the #PF resolution live in kernel/syscall/userspace.c
 * (userspace_fork_create / userspace_cow_fault) because they need the window's
 * per-page frame map and the frame reference counts, which only that module
 * keeps. PTE_COW above is the software bit they share with this header. */

#endif