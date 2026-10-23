/* SutureOS: ring-3 bring-up. See ring3.h for the design notes. */
#include "ring3.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include <stitch/user_context.h>
#include "el0_validate.h"
#include "syscall.h"      /* SYS_CLOCK_GETTIME for the tick probe */
#include "userspace.h"    /* per-process address space */
#include "process.h"      /* fork/wait4 processes of the current session */
#include "el0_switch.h"   /* el0_kernel_state_t + entry/exit primitives */
#include <stitch/user_context.h>
#include "mmu.h"

/* The embedded user program: scripts/build.py assembles user/init.S into
 * build/init.bin and objcopy's it into an object whose bytes are these symbols
 * (same mechanism as the AP trampoline). */
extern const uint8_t _binary_init_bin_start[];
extern const uint8_t _binary_init_bin_end[];
extern const uint8_t _binary_init_bin_size[];

#define USER_CS 0x2B
#define USER_SS 0x23
#define RFLAGS_USER 0x202u /* IF=1 at CPL=3: hardware interrupts are delivered to ring 3 */

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
static int g_ring3_ready;                 /* SSE/GS/kstack set up */
static el0_kernel_state_t g_kstate;       /* where el0_resume_kernel() returns to */

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

    /* IA32_EFER.NXE: without it the PTE bit 63 that mmu_map_user_pages() uses to
     * make the user stack non-executable is a *reserved* bit, so every stack access
     * took a reserved-bit #PF (and the machine looked dead). */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        lo |= (1u << 11);               /* NXE */
        __asm__ volatile("wrmsr" :: "c"(0xC0000080), "a"(lo), "d"(hi));
    }
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

/* The process layer (kernel/syscall/process.c) owns the switchable part of this
 * state: every process syscall must start on its own kernel stack, so %gs:8 is
 * re-pointed on every context switch. */
uint64_t ring3_gs_block(void) { return (uint64_t)(uintptr_t)&g_ring3_gs; }
void ring3_set_kernel_rsp(uint64_t rsp) { g_ring3_gs.kernel_rsp = rsp; }

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

/* Leave the ring-3 session: restore the launcher's GS MSRs and address space and
 * longjmp out of el0_enter_user_saved() with `code`. */
void ring3_resume_kernel_root(uint64_t code)
{
    g_ring3_exit_code = (int)code;
    g_ring3_active = 0;
    /* Restore the GS MSRs: a syscall-initiated exit skips the matching swapgs. */
    wrmsr(0xC0000101, g_kstate.gs_base);
    wrmsr(0xC0000102, g_kstate.gs_kernel_base);
    el0_validate_init();
    {
        extern void el0_resume_kernel(el0_kernel_state_t *state, uint64_t value);
        el0_resume_kernel(&g_kstate, code);            /* does not return */
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Called from sys_exit()/exit_group()/SIG_DFL while a user program is running.
 *
 * The root process of a session is the one the kernel entered, so its exit goes
 * back to the kernel launcher. A *child* has no kernel caller to return to: it
 * becomes a zombie, wakes its parent and schedules away (process.c), so the
 * syscall frame it is standing on is simply abandoned. */
void ring3_exit_user(uint64_t code)
{
    extern int  proc_current_pid(void);
    extern int  proc_current_is_root(void);
    extern void proc_exit_current(int, int) __attribute__((noreturn));

    if (proc_current_pid() >= 0 && !proc_current_is_root())
        proc_exit_current((int)code, 0);      /* does not return */

    ring3_resume_kernel_root(code);           /* does not return */
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
    g_kstate.gs_base = rdmsr(0xC0000101);
    g_kstate.gs_kernel_base = rdmsr(0xC0000102);
    {
        extern void el0_enter_user_saved(uint64_t pml4, uint64_t entry,
                                         uint64_t stack_top, el0_kernel_state_t *state);
        el0_enter_user_saved(0 /* keep the kernel map */, ctx.rip, ctx.rsp, &g_kstate);
    }
    g_ring3_active = 0;

    /* Only meaningful when the user program ever got to CPL=3 and came back. */
    console_printf("[ring3] the user program returned (exit=%d)\n",
                   (int64_t)g_ring3_exit_code);
    __asm__ volatile("sti");            /* interrupts were off while at CPL=3 */
    (void)rdmsr(0xC0000102);
    return g_ring3_exit_code;
}

/* ---------------- general blob runner (ported libc programs) --------------- */

/* Run one program as the root of a fresh process session. The program may fork()
 * children and wait4() for them; they are all destroyed when the root exits. */
int user_run_blob(const char *name, const uint8_t *blob, uint64_t len)
{
    userspace_t *us;
    int code;

    if (!g_ring3_ready) {
        sse_enable();
        g_ring3_ready = 1;
    }
    /* IA32_KERNEL_GS_BASE must be re-armed for EVERY run, not just the first one:
     * ring3_exit_user() restores the MSRs it found on entry (so the kernel's own
     * GS state is intact afterwards), which means the pointer is back to 0 when
     * the next program starts. With it at 0 the first `syscall` instruction's
     * swapgs is a no-op and `mov %gs:8,%rsp` loads the kernel stack pointer from
     * linear address 8 - the real-mode IVT - which is exactly the garbage stack
     * (0xf000ff53...) the signal self test hit. Every program that used only
     * int 0x80 never noticed. */
    wrmsr(0xC0000102, (uint64_t)(uintptr_t)&g_ring3_gs);

    if (len == 0 || len > (uint64_t)USERSPACE_CODE_PAGES * 4096u) {
        console_printf("[user] '%s': blob unusable (len=%llu, limit %u)\n",
                       (name != NULL) ? name : "?", len,
                       (int64_t)(USERSPACE_CODE_PAGES * 4096));
        return -1;
    }

    /* One session at a time: the processes of a session share the user VA window
     * and the single `syscall` gate state. */
    if (proc_session_begin() != 0) {
        console_printf("[user] '%s': a ring-3 session is already running\n",
                       (name != NULL) ? name : "?");
        return -1;
    }
    if (proc_root_create(name, blob, len) != 0) {
        console_printf("[user] '%s': cannot create an address space\n",
                       (name != NULL) ? name : "?");
        proc_session_end();
        return -1;
    }
    us = proc_root_us();
    if (us == NULL) {
        proc_session_end();
        return -1;
    }

    g_ring3_gs.kernel_rsp = proc_current_kstack_top() - 64;
    el0_validate_init();
    (void)el0_grant_range(USERSPACE_VA, USERSPACE_BYTES);
    el0_set_active(1);
    {
        extern void int80_init(void);
        int80_init();
    }

    console_printf("[user] running '%s': va=0x%llx len=%llu stack=0x%llx pml4=0x%llx\n",
                   (name != NULL) ? name : "?", USERSPACE_VA, len,
                   userspace_stack_top(us), us->pml4);

    g_ring3_active = 1;
    g_ring3_exit_code = -1;
    g_kstate.gs_base = rdmsr(0xC0000101);
    g_kstate.gs_kernel_base = rdmsr(0xC0000102);
    {
        extern void el0_enter_user_saved(uint64_t pml4, uint64_t entry,
                                         uint64_t stack_top, el0_kernel_state_t *state);
        el0_enter_user_saved(us->pml4, userspace_entry(us), userspace_stack_top(us),
                             &g_kstate);
    }
    g_ring3_active = 0;
    __asm__ volatile("sti");

    code = g_ring3_exit_code;
    console_printf("[user] '%s' exited with %d\n",
                   (name != NULL) ? name : "?", (int64_t)code);
    /* The session is over: every child the program may have left behind is
     * destroyed here (nothing is running any more, so their kernel stacks can be
     * handed back to the PMM). */
    proc_session_end();
    return code;
}

/* ---------------- built-in program registry ---------------- */

extern const uint8_t _binary_hello_bin_start[];
extern const uint8_t _binary_hello_bin_end[];
extern const uint8_t _binary_hello_bin_size[];
extern const uint8_t _binary_probe_bin_start[];
extern const uint8_t _binary_probe_bin_end[];
extern const uint8_t _binary_probe_bin_size[];
extern const uint8_t _binary_shell_bin_start[];
extern const uint8_t _binary_shell_bin_end[];
extern const uint8_t _binary_shell_bin_size[];
/* ITEM 5 acceptance: the signal-semantics program (user/apps/sigtest.c). */
extern const uint8_t _binary_sigtest_bin_start[];
extern const uint8_t _binary_sigtest_bin_end[];
extern const uint8_t _binary_sigtest_bin_size[];
/* Multi-process acceptance: fork + COW + wait4 + getppid (user/apps/forktest.c). */
extern const uint8_t _binary_forktest_bin_start[];
extern const uint8_t _binary_forktest_bin_end[];
extern const uint8_t _binary_forktest_bin_size[];

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *size_ptr;
} user_program_t;

static const user_program_t g_programs[] = {
    { "hello", _binary_hello_bin_start, _binary_hello_bin_size },
    { "probe", _binary_probe_bin_start, _binary_probe_bin_size },
    { "shell", _binary_shell_bin_start, _binary_shell_bin_size },
    /* Signal semantics acceptance (ITEM 5). Deliberately NOT auto-run at boot:
     * `run sigtest` from the shell is the acceptance path, so the frozen boot
     * log gains nothing. */
    { "sigtest", _binary_sigtest_bin_start, _binary_sigtest_bin_size },
    /* Multi-process acceptance: `run forktest`. Also not auto-run - the boot log
     * must stay byte-for-byte the same shape. */
    { "forktest", _binary_forktest_bin_start, _binary_forktest_bin_size },
};

/* ---- execve support ----
 * Called from syscall_dispatch_c() for SYS_EXECVE: rebuilds the *current* address
 * space with the requested built-in program and rewrites the syscall return frame
 * so iretq lands at the new entry point. Returns 0 or a negative errno. */
int user_exec_in_place(const char *path, void *ctx_void)
{
    user_context_t *ctx = (user_context_t *)ctx_void;
    const char *name = path;
    userspace_t *us = proc_current_us();      /* the *calling* process's window */
    const uint8_t *blob = NULL;
    uint64_t len = 0;

    if (name == NULL || us == NULL || ctx == NULL) return -14;          /* EFAULT */
    while (*name == '/') name++;

    for (uint64_t i = 0; i < sizeof(g_programs) / sizeof(g_programs[0]); i++) {
        if (strcmp(g_programs[i].name, name) == 0) {
            blob = g_programs[i].start;
            len = (uint64_t)(uintptr_t)g_programs[i].size_ptr;
            break;
        }
    }
    if (blob == NULL) return -2;                                        /* ENOENT */

    if (userspace_load(us, blob, len) != 0) return -12;                 /* ENOMEM */
    mmu_flush_tlb();

    ctx->rip = userspace_entry(us);
    ctx->rsp = userspace_stack_top(us);
    ctx->rax = 0;
    console_printf("[user] execve('/%s') -> new image, entry=0x%llx\n",
                   name, (uint64_t)ctx->rip);
    return 0;
}

/* execve through the int 0x80 gate: rebuild the image and rewrite the interrupt
 * frame so iretq returns into the new program. Frame offsets are the ones the stub
 * really produces (see kernel/syscall/int80.c). */
int user_exec_from_frame(const char *path, void *frame_void)
{
    uint8_t *f = (uint8_t *)frame_void;
    userspace_t *us = proc_current_us();      /* the *calling* process's window */
    const char *name = path;
    char name_copy[32];
    const uint8_t *blob = NULL;
    uint64_t len = 0;

    if (name == NULL || us == NULL || f == NULL) return -14;            /* EFAULT */
    while (*name == '/') name++;
    /* the path lives in the user window that userspace_load() is about to zero */
    {
        size_t n = strlen(name);
        if (n >= sizeof(name_copy)) n = sizeof(name_copy) - 1;
        memcpy(name_copy, name, n);
        name_copy[n] = '\0';
    }

    for (uint64_t i = 0; i < sizeof(g_programs) / sizeof(g_programs[0]); i++) {
        if (strcmp(g_programs[i].name, name) == 0) {
            blob = g_programs[i].start;
            len = (uint64_t)(uintptr_t)g_programs[i].size_ptr;
            break;
        }
    }
    if (blob == NULL) return -2;                                        /* ENOENT */
    if (userspace_load(us, blob, len) != 0) return -12;                 /* ENOMEM */
    mmu_flush_tlb();

    *(uint64_t *)(void *)(f + 0x88) = userspace_entry(us);              /* rip */
    *(uint64_t *)(void *)(f + 0xA0) = userspace_stack_top(us);          /* rsp */
    *(uint64_t *)(void *)(f + 0x70) = 0;                                /* rax = 0 */
    console_printf("[user] execve('/%s') ok, entry=0x%llx\n",
                   name_copy, userspace_entry(us));
    return 0;
}

/* Console input for ring 3 (`int 0x80` read on fd 0). Returns the number of bytes
 * placed in buf, 0 on an empty poll, or a negative errno. */
extern int console_getc(void);

extern void console_putc(char c);

int user_console_read(char *buf, unsigned len)
{
    unsigned got = 0;
    if (buf == NULL || len == 0) return -22;
    while (got < len) {
        int c = console_getc();
        if (c < 0) {
            if (got == 0) return -11;               /* EAGAIN */
            break;
        }
        if (c == '\r') c = '\n';
        /* echo: the user program cannot paint the console itself, the kernel does
         * it here so the ring-3 shell behaves like a real terminal */
        if (c == 8 || c == 127) {                   /* backspace */
            if (got > 0) {
                got--;
                console_puts("\b \b");
            }
            continue;
        }
        buf[got++] = (char)c;
        console_putc((char)c);
        if (c == '\n') break;
    }
    return (int)got;
}

int user_run_builtin(const char *name)
{
    if (name == NULL) return -1;

    for (uint64_t i = 0; i < sizeof(g_programs) / sizeof(g_programs[0]); i++) {
        if (strcmp(g_programs[i].name, name) == 0) {
            const uint64_t len = (uint64_t)(uintptr_t)g_programs[i].size_ptr;
            return user_run_blob(g_programs[i].name, g_programs[i].start, len);
        }
    }
    return -1;
}

const char *user_builtin_list(void) { return "hello probe shell sigtest forktest"; }

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
