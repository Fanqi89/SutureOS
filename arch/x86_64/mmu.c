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

/* COW 支持：PTE 位定义见 mmu.h（PTE_COW = bit 9，x86_64 的软件可用位）。
 * fork/缺页的真正实现在 kernel/syscall/userspace.c —— 那里才有窗口的每页物理帧
 * 映射和引用计数，见该文件的说明。这里原先的 mmu_handle_cow/mmu_fork_copy 用
 * 静态 MMU 池当页拷贝来源、且完全没有引用计数（父子两个地址空间各自销毁时会
 * 双重释放同一段 16 页窗口），已删除。 */

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

/* 把物理区间按 2 MiB 页恒等映射进当前页表（用于 1 GiB 之外的开机恒等映射
 * 覆盖不到的 MMIO/线性帧缓冲，例如 0xFD000000）。
 * 注意：不能复用 mmu_walk()——那个函数总是递归到 PTE 级（level 3），
 * 在 PD 级遇到空项时还会分配一张空 PT 顶掉 2 MiB 页映射。这里显式逐级下降。
 * 返回 0 = 成功，-1 = 失败。 */
static uint64_t *mmu_next_table(uint64_t *entry, int create)
{
    uint64_t pa = *entry & ~0xFFFULL;
    if (pa) return (uint64_t *)(uintptr_t)pa;      /* 已存在（1:1 映射） */
    if (!create) return NULL;
    uint64_t *nt = mmu_alloc_page();
    if (!nt) return NULL;
    for (int i = 0; i < 512; i++) nt[i] = 0;
    *entry = (uint64_t)(uintptr_t)nt | PTE_P | PTE_W;
    return nt;
}

int mmu_map_mmio_identity(uint64_t phys, uint64_t bytes)
{
    if (bytes == 0) return -1;
    uint64_t start = phys & ~0x1FFFFFULL;
    uint64_t end   = (phys + bytes + 0x1FFFFFULL) & ~0x1FFFFFULL;

    for (uint64_t a = start; a < end; a += 0x200000ULL) {
        uint64_t *pml4 = (uint64_t *)(uintptr_t)read_cr3();
        uint64_t *pdpt = mmu_next_table(&pml4[(a >> 39) & 0x1FF], 1);
        if (!pdpt) return -1;
        if (pdpt[(a >> 30) & 0x1FF] & PTE_PS) continue;  /* 1 GiB 页已覆盖 */
        uint64_t *pd = mmu_next_table(&pdpt[(a >> 30) & 0x1FF], 1);
        if (!pd) return -1;
        uint64_t *pde = &pd[(a >> 21) & 0x1FF];
        if (*pde & PTE_PS) continue;                     /* 已映射为 2 MiB 页 */
        *pde = a | PTE_P | PTE_W | PTE_PS;
    }

    /* 刷新 TLB：重载 CR3 */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    return 0;
}

/* 把一段 identity map 的 4 KiB 页提升到 U/S=1（ring 3 可访问）。
 * 2 MiB 大页会先被拆成页表，因此只有被点名的那几页对用户开放；
 * 调用者必须自己拥有这些物理页（来自 moe_pmm_alloc），否则等于把内核内存交给用户态。 */
int mmu_map_user_identity(uint64_t va, uint64_t len, int writable, int executable)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(read_cr3() & ~0xFFFULL);
    uint64_t first = va & ~(uint64_t)0xFFF;
    uint64_t last  = (va + len + 0xFFF) & ~(uint64_t)0xFFF;

    for (uint64_t v = first; v < last; v += PAGE_SIZE) {
        uint64_t *pml4e = &pml4[(v >> 39) & 0x1FF];
        uint64_t *pdpt;
        uint64_t *pdpe;
        uint64_t *pd;
        uint64_t *pde;
        uint64_t *pt;
        uint64_t *pte;

        if ((*pml4e & PTE_P) == 0) return -1;
        pdpt = (uint64_t *)(uintptr_t)(*pml4e & ~0xFFFULL);
        pdpe = &pdpt[(v >> 30) & 0x1FF];
        if ((*pdpe & PTE_P) == 0 || (*pdpe & PTE_PS) != 0) return -1;  /* no 1 GiB split */
        pd = (uint64_t *)(uintptr_t)(*pdpe & ~0xFFFULL);
        pde = &pd[(v >> 21) & 0x1FF];
        if ((*pde & PTE_P) == 0) return -1;

        if ((*pde & PTE_PS) != 0) {
            /* split this 2 MiB page into 512 identity 4 KiB pages */
            const uint64_t base = *pde & ~(uint64_t)0x1FFFFF;
            const uint64_t flags = (*pde & (PTE_P | PTE_W | PTE_PWT | PTE_PCD |
                                            PTE_A | PTE_D | PTE_G | PTE_NX)) & ~PTE_PS;
            uint64_t *new_pt = (uint64_t *)mmu_alloc_page();
            if (new_pt == NULL) return -1;
            for (int i = 0; i < 512; i++) {
                new_pt[i] = (base + (uint64_t)i * PAGE_SIZE) | (flags & ~PTE_U);
            }
            *pde = (uint64_t)(uintptr_t)new_pt | (flags & ~PTE_PS) | PTE_P | PTE_W;
            /* the parent tables must allow user access too */
            *pml4e |= PTE_U;
            *pdpe  |= PTE_U;
            *pde   |= PTE_U;
        } else {
            *pml4e |= PTE_U;
            *pdpe  |= PTE_U;
            *pde   |= PTE_U;
        }

        pt = (uint64_t *)(uintptr_t)(*pde & ~0xFFFULL);
        pte = &pt[(v >> 12) & 0x1FF];
        if ((*pte & PTE_P) == 0) return -1;
        *pte |= PTE_P | PTE_U;
        if (writable)   *pte |= PTE_W;
        if (executable) *pte &= ~PTE_NX;
        else            *pte |= PTE_NX;
    }

    /* flush the TLB: reload CR3 */
    {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    return 0;
}

/* Public page-table allocator: the per-process address spaces (userspace.c) need
 * a zeroed 4 KiB table just like mmu_walk() does internally. */
void *mmu_alloc_table(void)
{
    return mmu_alloc_page();
}

/* Reload CR3 to flush the TLB after the caller rewrote PTEs. */
void mmu_flush_tlb(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* 把用户虚拟区 [va, va+pages*4096) 映射到物理页 pa.. （每页 4 KiB，U/S=1）。
 * 与 mmu_map_user_identity 的区别：VA 不再等于 PA，这是跑"链接在固定地址"的
 * 用户程序所必需的（VimtuOS 的扁平 blob 模型链接在固定 VA）。 */
int mmu_map_user_pages(uint64_t va, uint64_t pa, uint64_t pages,
                       int writable, int executable)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(read_cr3() & ~0xFFFULL);

    for (uint64_t i = 0; i < pages; i++) {
        const uint64_t v = va + i * PAGE_SIZE;
        uint64_t *pml4e = &pml4[(v >> 39) & 0x1FF];
        uint64_t *pdpt;
        uint64_t *pdpe;
        uint64_t *pd;
        uint64_t *pde;
        uint64_t *pt;
        uint64_t *pte;

        if ((*pml4e & PTE_P) == 0) return -1;
        pdpt = (uint64_t *)(uintptr_t)(*pml4e & ~0xFFFULL);
        pdpe = &pdpt[(v >> 30) & 0x1FF];
        if ((*pdpe & PTE_P) == 0) return -1;          /* caller must pre-map the PDPT */
        if (*pdpe & PTE_PS) return -1;                 /* 1 GiB page: not supported */
        pd = (uint64_t *)(uintptr_t)(*pdpe & ~0xFFFULL);
        pde = &pd[(v >> 21) & 0x1FF];
        if ((*pde & PTE_P) == 0 || (*pde & PTE_PS) != 0) return -1;

        *pml4e |= PTE_U;
        *pdpe  |= PTE_U;
        *pde   |= PTE_U;
        pt = (uint64_t *)(uintptr_t)(*pde & ~0xFFFULL);
        pte = &pt[(v >> 12) & 0x1FF];
        *pte = (pa + i * PAGE_SIZE) | PTE_P | PTE_U;
        if (writable)   *pte |= PTE_W;
        if (executable) *pte &= ~PTE_NX;
        else            *pte |= PTE_NX;
    }

    {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    return 0;
}

/* 为 mmu_map_user_pages 准备 VA 区间的页表（PML4/PDPT/PD/PT 全部就位）。 */
int mmu_prepare_user_range(uint64_t va, uint64_t pages)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(read_cr3() & ~0xFFFULL);
    for (uint64_t i = 0; i < pages; i++) {
        if (mmu_walk(pml4, 0, va + i * PAGE_SIZE, 1) == NULL) return -1;
    }
    return 0;
}

/* 克隆内核页表到用户 TTBR0：仅提升 [base, base+len) 到 U/S=1 */int mmu_clone_for_el0(uint64_t kernel_pml4, uint64_t *out_user_pml4,
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
