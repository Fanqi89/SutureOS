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

/* 页表遍历/分配 */
uint64_t *mmu_walk(uint64_t *table, int level, uint64_t vaddr, int create);
void *loader_alloc_page(void);

/* COW 支持 */
void mmu_mark_cow(uint64_t *pte);
bool mmu_is_cow(uint64_t pte);
int mmu_handle_cow(uint64_t *pte, uint64_t fault_addr);
int mmu_mark_user_pages_cow(uint64_t pml4);
int mmu_fork_copy(uint64_t parent_pml4, uint64_t *out_child_pml4);

#endif