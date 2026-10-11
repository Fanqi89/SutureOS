/* SutureOS: x86_64 4-level page table (TTBR0 clone for EL0 isolation)
 * Ported from hudos mmu.c (ARM64) -> x86_64 48-bit VA, 4KB pages
 * 克隆内核页表，仅提升用户镜像/栈/参数页到 U/S=1，其余保持 S=0
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "mmu.h"

/* 页表常量 */
#define PAGE_SIZE       4096
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

/* COW 支持 */
#define PTE_COW         (1ULL << 9)   /* Copy-on-Write 标记 */
#define PTE_COW_MASK    (PTE_COW | PTE_W)  /* COW 页面的写保护掩码 */

#define PML4_ENTRIES    512
#define PDP_ENTRIES     512
#define PD_ENTRIES      512
#define PT_ENTRIES      512

/* 物理内存分配器（简化：从静态池分配页表页） */
static uint64_t mmu_page_pool[8192] __attribute__((aligned(4096)));
static size_t mmu_pool_used = 0;

static void *mmu_alloc_page(void)
{
    if (mmu_pool_used >= sizeof(mmu_page_pool)) return NULL;
    void *p = (uint8_t *)mmu_page_pool + mmu_pool_used;
    mmu_pool_used += PAGE_SIZE;
    memset(p, 0, PAGE_SIZE);
    return p;
}

/* 查找或创建页表项 */
uint64_t *mmu_walk(uint64_t *table, int level, uint64_t vaddr, int create)
{
    int shift = 39 - level * 9;           /* 39,30,21,12 */
    int idx = (vaddr >> shift) & 0x1FF;
    uint64_t *entry = &table[idx];

    if (level == 3) return entry;         /* PT level, 返回 PTE */

    uint64_t next_pa = *entry & ~0xFFF;
    uint64_t *next_table;
    if (next_pa) {
        next_table = (uint64_t *)(uintptr_t)next_pa;  /* 1:1 映射 */
    } else if (create) {
        next_table = mmu_alloc_page();
        if (!next_table) return NULL;
        *entry = (uint64_t)(uintptr_t)next_table | PTE_P | PTE_W | PTE_U;
    } else {
        return NULL;
    }
    return mmu_walk(next_table, level + 1, vaddr, create);
}

/* 克隆内核页表到用户 TTBR0：仅提升 [base, base+len) 到 U/S=1 */
int mmu_clone_for_el0(uint64_t kernel_pml4, uint64_t *out_user_pml4,
                      uint64_t base, uint64_t len)
{
    uint64_t *user_pml4 = mmu_alloc_page();
    if (!user_pml4) return -1;

    /* 1. 复制内核 PML4（高半部分保持共享，低半部分置 0） */
    memcpy(user_pml4 + 256, (void *)(uintptr_t)(kernel_pml4 + 256 * 8), 256 * 8);
    memset(user_pml4, 0, 256 * 8);

    /* 2. 为用户区域 [base, base+len) 逐页设置 U/S=1 */
    for (uint64_t v = base & ~(PAGE_SIZE - 1);
         v < base + len; v += PAGE_SIZE) {
        uint64_t *pte = mmu_walk(user_pml4, 0, v, 1);
        if (!pte) { console_puts("[mmu] walk failed\n"); return -1; }
        *pte |= PTE_U;   /* 提升到用户可访问 */
    }

    *out_user_pml4 = (uint64_t)(uintptr_t)user_pml4;
    console_printf("[mmu] EL0 clone: kernel=%p user=%p base=%p len=%llu\n",
                   (void*)kernel_pml4, (void*)*out_user_pml4, (void*)base, len);
    return 0;
}

/* 释放用户页表（简化：不释放，实际应递归释放） */
void mmu_free_user_pml4(uint64_t user_pml4) { (void)user_pml4; }

/* COW 支持函数 */
void mmu_mark_cow(uint64_t *pte) {
    if (!pte) return;
    *pte |= PTE_COW;
    *pte &= ~PTE_W;  /* 移除写权限，触发 #PF */
}

bool mmu_is_cow(uint64_t pte) {
    return (pte & PTE_COW) != 0;
}

/* 处理 COW 缺页 */
int mmu_handle_cow(uint64_t *pml4, uint64_t fault_addr) {
    if (!pml4) return -1;
    
    uint64_t *pte = mmu_walk((uint64_t *)pml4, 0, fault_addr, 0);
    if (!pte || !mmu_is_cow(*pte)) return -1;
    
    uint64_t old_pfn = (*pte & ~0xFFF) >> 12;
    
    // 简化：不做引用计数，直接复制页面
    void *new_page = mmu_alloc_page();
    if (!new_page) return -1;
    
    memcpy(new_page, (void *)(old_pfn << 12), PAGE_SIZE);
    
    uint64_t new_pfn = ((uint64_t)(uintptr_t)new_page) >> 12;
    *pte = (new_pfn << 12) | (((*pte & 0xFFF) & ~PTE_COW) | PTE_W);
    
    // 刷新 TLB
    __asm__ volatile("invlpg %0" :: "m"(*(volatile char *)fault_addr) : "memory");
    
    return 0;
}

/* fork 时标记所有用户页面为 COW */
int mmu_mark_user_pages_cow(uint64_t pml4) {
    for (uint64_t v = 0x40000000; v < 0x80000000; v += PAGE_SIZE) {  /* 用户空间 1GB-2GB */
        uint64_t *pte = mmu_walk((uint64_t *)pml4, 0, v, 0);
        if (pte && (*pte & PTE_P) && (*pte & PTE_U)) {
            if (!mmu_is_cow(*pte) && (*pte & PTE_W)) {
                uint64_t *pte_w = mmu_walk((uint64_t *)pml4, 0, v, 1);
                if (pte_w) {
                    *pte_w |= PTE_COW;
                    *pte_w &= ~PTE_W;
                }
            }
        }
    }
    return 0;
}

/* 复制页表用于 fork（建立 COW 映射） */
int mmu_fork_copy(uint64_t parent_pml4_val, uint64_t *out_child_pml4) {
    uint64_t *child_pml4 = mmu_alloc_page();
    if (!child_pml4) return -1;
    
    /* 复制高半部分（内核空间共享） */
    uint64_t *parent_pml4 = (uint64_t *)read_cr3();
    memcpy(child_pml4 + 256, (void *)(uintptr_t)((uint64_t)parent_pml4 + 256 * 8), 256 * 8);
    memset(child_pml4, 0, 256 * 8);
    
    /* 复制用户空间页表，建立 COW 映射 */
    for (uint64_t v = 0x40000000; v < 0x80000000; v += PAGE_SIZE) {  /* 用户空间 1GB-2GB */
        uint64_t *parent_pte = mmu_walk((uint64_t *)read_cr3(), 0, v, 0);
        if (parent_pte && (*parent_pte & PTE_P) && (*parent_pte & PTE_U)) {
            uint64_t *child_pte = mmu_walk(child_pml4, 0, v, 1);
            if (!child_pte) continue;
            
            if (*parent_pte & PTE_P) {
                /* 标记为 COW */
                *child_pte = *parent_pte | PTE_COW;
                *child_pte &= ~PTE_W;  /* 移除写权限 */
            }
        }
    }
    
    *out_child_pml4 = (uint64_t)(uintptr_t)child_pml4;
    return 0;
}