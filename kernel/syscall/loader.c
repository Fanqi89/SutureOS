/* SutureOS: 简易 ELF 加载器 (仅加载 PT_LOAD 段，跳入 entry) */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "loader.h"
#include "el0_validate.h"
#include "syscall.h"
#include "mmu.h"
#include <stitch/user_context.h>
#include "task.h"
#include "vfs.h"
#include "el0_switch.h"

#define ELF_MAGIC 0x464C457F
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr;
    uint64_t p_filesz, p_memsz, p_align;
} __attribute__((packed));

int elf_load_user(const void *buf, size_t len,
                  uint64_t *out_entry, uint64_t *out_base, uint64_t *out_end)
{
    if (len < sizeof(struct elf64_ehdr)) return -1;
    const struct elf64_ehdr *eh = buf;
    if (*(uint32_t *)eh->e_ident != ELF_MAGIC) return -1;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) return -1;

    uint64_t min_v = ~0ULL, max_v = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (void *)((uint8_t *)buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < min_v) min_v = ph->p_vaddr & ~4095;
        if (ph->p_vaddr + ph->p_memsz > max_v) max_v = (ph->p_vaddr + ph->p_memsz + 4095) & ~4095;
    }
    if (min_v == ~0ULL) return -1;

    *out_base = min_v;
    *out_end  = max_v;
    *out_entry = eh->e_entry;

    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (void *)((uint8_t *)buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_vaddr;
        const uint8_t *src = (const uint8_t *)buf + ph->p_offset;
        memset(dst, 0, ph->p_memsz);
        memcpy(dst, src, ph->p_filesz);
        console_printf("[loader] PT_LOAD vaddr=%p filesz=%llu memsz=%llu flags=%x\n",
                       (void*)ph->p_vaddr, ph->p_filesz, ph->p_memsz, ph->p_flags);
    }
    return 0;
}

static uint64_t user_page_pool[256][512] __attribute__((aligned(4096)));
static int user_page_pool_used = 0;

void *loader_alloc_page(void)
{
    if (user_page_pool_used >= 256) return NULL;
    void *p = &user_page_pool[user_page_pool_used][0];
    user_page_pool_used++;
    memset(p, 0, 4096);
    return p;
}

extern void *vfs_read_file(const char *path, size_t *len);

int syscall_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    const char *path = (const char *)(uintptr_t)path_ptr;
    (void)argv_ptr; (void)envp_ptr;
    EL0_STR(path);

    size_t len;
    void *buf = vfs_read_file(path, &len);
    if (!buf) return (uint64_t)-2;

    uint64_t entry, base, end;
    if (elf_load_user(buf, len, &entry, &base, &end) < 0) return (uint64_t)-1;

    uint64_t user_pml4;
    if (mmu_clone_for_el0(read_cr3(), &user_pml4, base, end - base) < 0)
        return (uint64_t)-1;

    el0_validate_init();
    el0_grant_range(base, end - base);

    static uint64_t user_stack_pool[16][1024] __attribute__((aligned(4096)));
    static int stack_pool_used = 0;
    if (stack_pool_used >= 16) return (uint64_t)-1;
    uint64_t ustack = (uint64_t)&user_stack_pool[stack_pool_used][1024];
    stack_pool_used++;

    user_context_t uctx;
    user_context_init(&uctx, entry, ustack, 0);

    extern void task_set_user_pml4(int tid, uint64_t pml4);
    extern sched_cpu_t sched_cpu;
    task_set_user_pml4(sched_cpu.current, user_pml4);

    el0_set_active(1);
    el0_enter_user(user_pml4, &uctx);
    return 0;
}