/* SutureOS: 用户内存分配 (brk/mmap) */
#include <stitch/types.h>
#include <stitch/console.h>
#include "task.h"
#include "el0_validate.h"
#include "mmu.h"
#include "loader.h"

#define USER_HEAP_START 0x40000000  /* 1GB */
#define USER_HEAP_MAX   0x80000000  /* 2GB */

static uint64_t user_brk[SCHED_MAX_TASKS] = {0};

uint64_t sys_brk(uint64_t addr, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    uint64_t old = user_brk[tid];
    if (addr == 0) return old;
    if (addr < USER_HEAP_START || addr >= USER_HEAP_MAX) return old;
    uint64_t new_brk = (addr + 4095) & ~4095;
    /* 映射新页面 */
    for (uint64_t v = old; v < new_brk; v += 4096) {
        uint64_t *pte = mmu_walk((uint64_t*)read_cr3(), 0, v, 1);
        if (!pte) return old;
        *pte = (uint64_t)loader_alloc_page() | PTE_P | PTE_W | PTE_U;
    }
    user_brk[tid] = new_brk;
    return new_brk;
}

/* 简易 mmap (仅支持 MAP_ANONYMOUS | MAP_PRIVATE) */
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void)fd; (void)offset;
    extern sched_cpu_t sched_cpu;
    int tid = sched_cpu.current;
    if (!(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE)) return (uint64_t)-1;
    if (len == 0) return (uint64_t)-1;
    len = (len + 4095) & ~4095;
    uint64_t vaddr = addr ? (addr & ~4095) : USER_HEAP_MAX - len;
    if (vaddr < USER_HEAP_START || vaddr + len > USER_HEAP_MAX) return (uint64_t)-1;
    for (uint64_t v = vaddr; v < vaddr + len; v += 4096) {
        uint64_t *pte = mmu_walk((uint64_t*)read_cr3(), 0, v, 1);
        if (!pte) return (uint64_t)-1;
        uint64_t pte_val = (uint64_t)loader_alloc_page() | PTE_P | PTE_U;
        if (prot & PROT_WRITE) pte_val |= PTE_W;
        *pte = pte_val;
    }
    return vaddr;
}

/* Reload CR3 to drop stale TLB entries after a PTE edit. */
static void mem_flush_tlb(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

#define PTE_NX_BIT (1ull << 63)

/* mprotect: change the permission bits of an already mapped anonymous range. */
uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if ((addr & 0xFFFull) != 0 || len == 0) return (uint64_t)-22;   /* EINVAL */
    len = (len + 4095) & ~4095ull;
    if (addr < USER_HEAP_START || addr + len > USER_HEAP_MAX) return (uint64_t)-12;
    for (uint64_t v = addr; v < addr + len; v += 4096) {
        uint64_t *pte = mmu_walk((uint64_t*)read_cr3(), 0, v, 0);
        if (!pte || !(*pte & PTE_P)) return (uint64_t)-12;          /* ENOMEM */
        if (prot & PROT_WRITE) *pte |= PTE_W; else *pte &= ~PTE_W;
        if (prot & PROT_EXEC)  *pte &= ~PTE_NX_BIT; else *pte |= PTE_NX_BIT;
    }
    mem_flush_tlb();
    return 0;
}

uint64_t sys_munmap(uint64_t addr, uint64_t len,
                    uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    /* Unmap the range for real (the old code only returned 0). The frames are not
     * handed back to the PMM yet - that needs a per-range allocator - but the
     * mapping is gone, so use-after-unmap faults instead of silently working. */
    if ((addr & 0xFFFull) != 0 || len == 0) return (uint64_t)-22;
    len = (len + 4095) & ~4095ull;
    if (addr < USER_HEAP_START || addr + len > USER_HEAP_MAX) return (uint64_t)-22;
    for (uint64_t v = addr; v < addr + len; v += 4096) {
        uint64_t *pte = mmu_walk((uint64_t*)read_cr3(), 0, v, 0);
        if (pte && (*pte & PTE_P)) *pte = 0;
    }
    mem_flush_tlb();
    return 0;
}

uint64_t sys_munmap_legacy_unused(uint64_t addr, uint64_t len,
                                  uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)addr; (void)len; (void)a3; (void)a4; (void)a5; (void)a6;
    /* 简化：不实际释放，仅返回 0 */
    return 0;
}