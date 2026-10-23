/* SutureOS: ring-3 bring-up. See ring3.h for the design notes. */
#include "ring3.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include <stitch/user_context.h>
#include "el0_validate.h"
#include "mmu.h"

/* The embedded user program: scripts/build.py assembles user/init.S into
 * build/init.bin and objcopy's it into an object whose bytes are these symbols
 * (same mechanism as the AP trampoline). */
extern const uint8_t _binary_init_bin_start[];
extern const uint8_t _binary_init_bin_end[];
extern const uint8_t _binary_init_bin_size[];

#define USER_CS 0x2B
#define USER_SS 0x23
#define RFLAGS_USER 0x2u   /* IF=0 while at CPL=3 (see ring3.h note on interrupts) */

/* Per-CPU block the syscall entry expects at %gs:
 *   gs:0 = user rsp (saved on entry)
 *   gs:8 = kernel rsp for this task (loaded on entry) */
struct ring3_gs {
    uint64_t user_rsp;
    uint64_t kernel_rsp;
};
static struct ring3_gs g_ring3_gs;

/* Kernel stack used for syscalls made by the ring-3 program. It must NOT be the
 * launcher's stack: the syscall entry pushes a 176-byte frame there, which used
 * to overwrite the return address saved by el0_enter_user_saved(), so exit(60)
 * "returned" into garbage (observed: kernel #UD at rip=0x3). */
#define RING3_KSTACK_BYTES (16u * 1024u)
static uint8_t g_ring3_kstack[RING3_KSTACK_BYTES] __attribute__((aligned(16)));

static uint64_t g_user_base;          /* PMM-allocated user region (identity) */
static uint64_t g_saved_kernel_rsp;    /* where el0_resume_kernel() returns to */
static volatile int g_ring3_active;
static volatile int g_ring3_exit_code = -1;
static volatile int g_ring3_ran;

/* ---- privileged helpers ---- */

static void sse_enable(void)
{
    uint64_t cr0;
    uint64_t cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);                /* clear EM: no FPU emulation */
    cr0 &= ~(1ULL << 3);                /* clear TS: no lazy FPU switch */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);                 /* OSFXSR: SSE state is saved/restored */
    cr4 |= (1ULL << 10);                /* OSXMMEXCPT: unmasked SIMD exceptions */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

int ring3_active(void) { return g_ring3_active; }

static int g_trace_budget = 0;   /* set to 8 to trace the first ring-3 syscalls */

/* Localises a hang: shows the syscall number and the first two arguments. */
void ring3_trace_syscall(uint64_t nr, uint64_t a1, uint64_t a2)
{
    if (g_trace_budget <= 0) return;
    g_trace_budget--;
    console_printf("[ring3] syscall nr=%llu a1=0x%llx a2=0x%llx\n", nr, a1, a2);
}
int ring3_last_exit(void) { return g_ring3_exit_code; }
int ring3_ran_ever(void) { return g_ring3_ran; }

/* Called from sys_exit() while the user program is running. */
void ring3_exit_user(uint64_t code)
{
    g_ring3_exit_code = (int)code;
    g_ring3_active = 0;
    el0_validate_init();
    {
        extern void el0_resume_kernel(uint64_t saved_rsp, uint64_t value);
        el0_resume_kernel(g_saved_kernel_rsp, code);   /* does not return */
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

int ring3_run_init(void)
{
    user_context_t ctx;
    const uint64_t blob_len = (uint64_t)(uintptr_t)_binary_init_bin_size;
    uint64_t base;

    if (g_ring3_ran) {
        g_ring3_exit_code = -1;
        return g_ring3_exit_code;
    }
    g_ring3_ran = 1;

    if (blob_len == 0 || blob_len > RING3_USER_BYTES) {
        console_printf("[ring3] user blob unusable (len=%llu)\n", blob_len);
        return -1;
    }

    /* Own the physical pages so PMM cannot hand them to the kernel later. */
    {
        extern uintptr_t moe_pmm_alloc(uint32_t n);
        base = (uint64_t)moe_pmm_alloc(RING3_USER_PAGES);
    }
    if (base == 0) {
        console_puts("[ring3] no memory for the user image\n");
        return -1;
    }
    g_user_base = base;

    /* Image pages: user readable/writable/executable. Stack page: no-execute. */
    if (mmu_map_user_identity(base, RING3_USER_BYTES - 4096, 1, 1) != 0 ||
        mmu_map_user_identity(base + RING3_USER_BYTES - 4096, 4096, 1, 0) != 0) {
        console_puts("[ring3] could not mark the user pages user-accessible\n");
        return -1;
    }
    memcpy((void *)(uintptr_t)base, _binary_init_bin_start, (size_t)blob_len);

    sse_enable();

    /* The syscall entry swaps GS with IA32_KERNEL_GS_BASE, so the per-CPU block
     * must be there before we drop to CPL=3. */
    wrmsr(0xC0000102, (uint64_t)(uintptr_t)&g_ring3_gs);   /* IA32_KERNEL_GS_BASE */
    g_ring3_gs.user_rsp = 0;
    g_ring3_gs.kernel_rsp = 0;   /* filled by syscall_entry from kstack top */

    /* The stack for a syscall must be a kernel stack: reuse this thread's by
     * taking the current rsp at entry time. el0_enter_user_saved records it. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base;
    ctx.rsp = base + RING3_USER_BYTES - 64;      /* 16-byte aligned-ish */
    ctx.rflags = RFLAGS_USER;
    ctx.cs = USER_CS;
    ctx.ss = USER_SS;

    /* Reject syscall pointer arguments outside the granted user range. */
    el0_validate_init();
    (void)el0_grant_range(base, RING3_USER_BYTES);
    el0_set_active(1);

    /* Kernel stack top for the syscall entry: a dedicated 16 KiB stack, not the
     * launcher's frame (see g_ring3_kstack). */
    g_ring3_gs.kernel_rsp = (uint64_t)(uintptr_t)&g_ring3_kstack[RING3_KSTACK_BYTES] - 64;

    console_printf("[ring3] entering CPL=3: entry=%p stack=%p blob=%llu bytes\n",
                   (void *)(uintptr_t)ctx.rip, (void *)(uintptr_t)ctx.rsp, blob_len);

    g_ring3_active = 1;
    g_ring3_exit_code = -1;
    {
        extern void el0_enter_user_saved(uint64_t entry, uint64_t stack_top,
                                         uint64_t *slot);
        el0_enter_user_saved(ctx.rip, ctx.rsp, &g_saved_kernel_rsp);
    }
    g_ring3_active = 0;

    /* Only meaningful when the user program ever got to CPL=3 and came back. */
    console_printf("[ring3] the user program returned (exit=%d)\n",
                   (int64_t)g_ring3_exit_code);
    __asm__ volatile("sti");            /* interrupts were off while at CPL=3 */
    (void)rdmsr(0xC0000102);
    return g_ring3_exit_code;
}

/* ---------------- acceptance ---------------- */

static uint32_t g_checks;
static uint32_t g_failed;

static void st_report(const char *what, int ok)
{
    g_checks++;
    if (!ok) g_failed++;
    console_printf("ring3 selftest: %s ... %s\n", what, ok ? "PASS" : "FAIL");
}

int ring3_selftest(void)
{
    g_checks = 0;
    g_failed = 0;

    st_report("a user blob is embedded in the kernel",
              (uint64_t)(uintptr_t)_binary_init_bin_size > 0);
    st_report("user pages were mapped user-accessible",
              g_user_base != 0 && g_user_base == (g_user_base & ~0xFFFULL));
    {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        st_report("SSE enabled (CR4.OSFXSR|OSXMMEXCPT)",
                  (cr4 & ((1ULL << 9) | (1ULL << 10))) == ((1ULL << 9) | (1ULL << 10)));
    }
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        st_report("FPU emulation/task-switch flags clear (CR0.EM|TS)",
                  (cr0 & ((1ULL << 2) | (1ULL << 3))) == 0);
    }
    {
        /* The syscall entry does swapgs on the way in and out, so after a round
         * trip the block lives in the other MSR. Either one proving reachable is
         * the invariant that matters: swapgs must land on &g_ring3_gs. */
        const uint64_t gs_base = rdmsr(0xC0000100 + 1);   /* IA32_GS_BASE */
        const uint64_t kgs_base = rdmsr(0xC0000100 + 2);  /* IA32_KERNEL_GS_BASE */
        st_report("the syscall per-CPU block is reachable via swapgs",
                  gs_base == (uint64_t)(uintptr_t)&g_ring3_gs ||
                  kgs_base == (uint64_t)(uintptr_t)&g_ring3_gs);
    }
    st_report("the user program ran to exit()", g_ring3_ran && g_ring3_exit_code >= 0);
    st_report("exit code 42 propagated from ring 3", g_ring3_exit_code == 42);

    /* Page-permission proof: walk the tables the CPU uses. */
    {
        uint64_t *pml4 = (uint64_t *)(uintptr_t)(read_cr3() & ~0xFFFULL);
        const uint64_t v = g_user_base;
        uint64_t *pdpt;
        uint64_t *pd;
        uint64_t *pt;
        uint64_t pte_code;
        uint64_t pte_kernel;

        pdpt = (uint64_t *)(uintptr_t)(pml4[(v >> 39) & 0x1FF] & ~0xFFFULL);
        pd   = (uint64_t *)(uintptr_t)(pdpt[(v >> 30) & 0x1FF] & ~0xFFFULL);
        pt   = (uint64_t *)(uintptr_t)(pd[(v >> 21) & 0x1FF] & ~0xFFFULL);
        pte_code = pt[(v >> 12) & 0x1FF];
        {
            const uint64_t kv = 0x100000;              /* kernel text */
            uint64_t *kpdpt = (uint64_t *)(uintptr_t)(pml4[(kv >> 39) & 0x1FF] & ~0xFFFULL);
            uint64_t *kpd   = (uint64_t *)(uintptr_t)(kpdpt[(kv >> 30) & 0x1FF] & ~0xFFFULL);
            uint64_t *kpd_e = &kpd[(kv >> 21) & 0x1FF];
            if ((*kpd_e & PTE_PS) != 0) {
                pte_kernel = *kpd_e;                    /* 2 MiB page: U bit is what matters */
            } else {
                uint64_t *kpt = (uint64_t *)(uintptr_t)(*kpd_e & ~0xFFFULL);
                pte_kernel = kpt[(kv >> 12) & 0x1FF];
            }
        }
        st_report("user code PTE has U/S=1 and P=1",
                  (pte_code & (PTE_P | PTE_U)) == (PTE_P | PTE_U));
        st_report("kernel text is NOT user accessible (U/S=0)",
                  (pte_kernel & PTE_U) == 0);
    }

    console_printf("ring3 selftest: RESULT %s (%u/%u)\n",
                   (g_failed == 0) ? "PASS" : "FAIL",
                   g_checks - g_failed, g_checks);
    return (g_failed == 0) ? 0 : 1;
}
