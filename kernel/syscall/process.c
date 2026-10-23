/* SutureOS: ring-3 processes - fork with copy-on-write, wait4, getppid.
 *
 * Why this module exists instead of using kernel/sched/task.c
 * ----------------------------------------------------------
 * The ring-3 world does not run *on* the kernel scheduler. A ring-3 program is
 * entered synchronously from kernel C by el0_enter_user_saved(); the "process" is
 * whatever kernel call frame is currently parked below the iretq, and it comes
 * back by longjmp'ing out of the syscall (el0_resume_kernel) when it exits. That
 * is why fork() cannot simply create a sched task: a task has a trampoline which
 * starts from nothing, while a forked child has to resume *inside* a syscall
 * frame with the parent's registers and its own copy of the address space.
 *
 * This module therefore implements a tiny cooperative scheduler whose unit is the
 * user process, and whose context switch is a kernel *stack* switch:
 *
 *   proc_ctx_switch(&from->ksp, &to->ksp)   push rbx/rbp/r12-r15, swap rsp, pop, ret
 *
 * Every process owns a 16 KiB kernel stack (PMM), so a process that is switched
 * out in the middle of a syscall leaves its C frames on its own stack and picks
 * them up exactly where it left off:
 *
 *   fork()   parent returns the child's pid immediately; the child is left
 *            RUNNABLE with a hand-built stack frame whose `ret` lands in
 *            proc_child_start -> proc_enter_user_first -> iretq with rax = 0.
 *   wait4()  the parent blocks (PROC_BLOCKED) and schedules; the child runs, and
 *            its exit wakes the parent and schedules back into it.
 *   exit()   the child becomes a zombie, wakes the parent, and schedules away -
 *            it never runs again, so it never returns from the switch.
 *   yield()  voluntary round-robin between the processes of the session.
 *
 * Consequences that are documented rather than hidden:
 *   - Scheduling is COOPERATIVE (a process gives up the CPU at fork/wait4/yield/
 *     exit and nowhere else); a spinning child starves its siblings, exactly like
 *     a cooperative scheduler and unlike a preemptive one.
 *   - All processes of a session share one kernel task slot, one CR3 switch point
 *     and the same user VA window (0x40000000), which is what makes the address
 *     space private instead of the VA.
 *   - The int 0x80 gate keeps running on the single static IRQ stack
 *     (arch/x86_64/gdt_idt.c rsp0): safe only because no int 0x80 call can block.
 *     fork/wait4/yield are reached through the `syscall` gate, whose entry loads
 *     the per-process kernel stack from %gs:8.
 */
#include "process.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "task.h"          /* sched_cpu: getpid() fallback outside a process */
#include "el0_validate.h"
#include "el0_switch.h"
#include "mmu.h"
#include "ring3.h"
#include "syscall.h"       /* fd table clone/release */
#include "signal.h"

#define PROC_KSTACK_PAGES 4
#define PROC_KSTACK_BYTES (PROC_KSTACK_PAGES * 4096)

#define IA32_FS_BASE        0xC0000100u
#define IA32_GS_BASE        0xC0000101u
#define IA32_KERNEL_GS_BASE 0xC0000102u

#define USER_CS 0x2B
#define USER_SS 0x23

/* WNOHANG for wait4(2). */
#define PROC_WNOHANG 1

extern uintptr_t moe_pmm_alloc(uint32_t n);
extern void moe_pmm_free(uintptr_t base, uint32_t n);

/* arch/x86_64/el0_switch.S: first instruction executed by a forked child. */
extern void proc_child_start(void);

typedef struct {
    int      state;             /* PROC_* */
    int      pid;
    int      ppid;              /* 0 = the kernel started it */
    int      is_root;           /* the session's first process */
    int      exit_code;         /* 0..255 */
    int      exit_sig;          /* 0 = a normal exit, else the killing signal */
    uint64_t kstack;            /* PMM base of this process's kernel stack */
    uint64_t kstack_top;
    uint64_t ksp;               /* kernel rsp while switched out (offset matters:
                                 * proc_ctx_switch takes &ksp) */
    int      fd_slot;           /* index into the fd/signal arrays */
    int      sig_slot;
    uint64_t fs_base;
    uint64_t gs_base;
    userspace_t us;
    user_context_t ctx;         /* user registers for the first iretq */
} proc_t;

static proc_t g_procs[PROC_MAX];
static int g_current = -1;
static int g_next_pid = 1;
static int g_session;

static void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

static proc_t *proc_current(void)
{
    if (g_current < 0 || g_current >= PROC_MAX) return NULL;
    return &g_procs[g_current];
}

int proc_current_pid(void)
{
    proc_t *p = proc_current();
    return (p != NULL) ? p->pid : -1;
}

int proc_current_is_root(void)
{
    proc_t *p = proc_current();
    return (p != NULL) ? p->is_root : 0;
}

int proc_current_slot(void)
{
    proc_t *p = proc_current();
    return (p != NULL) ? p->sig_slot : -1;
}

userspace_t *proc_current_us(void)
{
    proc_t *p = proc_current();
    return (p != NULL) ? &p->us : NULL;
}

userspace_t *proc_root_us(void)
{
    if (g_procs[0].state == PROC_FREE || !g_procs[0].is_root) return NULL;
    return &g_procs[0].us;
}

uint64_t proc_current_kstack_top(void)
{
    proc_t *p = proc_current();
    return (p != NULL) ? p->kstack_top : 0;
}

int proc_slot_for_pid(int pid)
{
    if (pid <= 0) return -1;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_procs[i].state != PROC_FREE && g_procs[i].pid == pid &&
            g_procs[i].state != PROC_ZOMBIE)
            return g_procs[i].sig_slot;
    return -1;
}

static int proc_index_of_pid(int pid)
{
    if (pid <= 0) return -1;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_procs[i].state != PROC_FREE && g_procs[i].pid == pid) return i;
    return -1;
}

/* ---------------- session lifecycle ---------------- */

int proc_session_begin(void)
{
    if (g_session) return -1;
    memset(g_procs, 0, sizeof(g_procs));
    g_current = -1;
    g_next_pid = 1;
    g_session = 1;
    return 0;
}

void proc_session_end(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &g_procs[i];
        if (p->us.in_use) userspace_destroy(&p->us);
        if (p->state != PROC_FREE && p->fd_slot >= PROC_SLOT_BASE)
            fd_table_release(p->fd_slot);
        if (p->kstack != 0) {
            moe_pmm_free((uintptr_t)p->kstack, PROC_KSTACK_PAGES);
            p->kstack = 0;
        }
        p->state = PROC_FREE;
    }
    memset(g_procs, 0, sizeof(g_procs));
    g_current = -1;
    g_session = 0;
}

int proc_root_create(const char *name, const uint8_t *blob, uint64_t len)
{
    proc_t *p;

    (void)name;
    if (!g_session) return -1;
    p = &g_procs[0];
    if (p->state != PROC_FREE) return -1;
    memset(p, 0, sizeof(*p));

    if (userspace_create(&p->us) != 0) return -1;
    if (userspace_load(&p->us, blob, len) != 0) {
        userspace_destroy(&p->us);
        return -1;
    }
    p->kstack = (uint64_t)moe_pmm_alloc(PROC_KSTACK_PAGES);
    if (p->kstack == 0) {
        userspace_destroy(&p->us);
        return -1;
    }
    p->kstack_top = (p->kstack + PROC_KSTACK_BYTES) & ~0xFULL;
    p->fd_slot = PROC_SLOT_BASE + 0;
    p->sig_slot = p->fd_slot;
    p->is_root = 1;
    p->pid = g_next_pid++;
    p->ppid = 0;
    p->state = PROC_RUNNING;
    g_current = 0;
    return 0;
}

/* ---------------- scheduling ---------------- */

static void proc_switch_to(int next)
{
    proc_t *prev = proc_current();
    proc_t *nx = &g_procs[next];

    if (prev == nx) return;
    nx->state = PROC_RUNNING;
    g_current = next;
    /* The syscall entry reads %gs:8 on the way in, so it must always describe the
     * process that is about to run at CPL=3. */
    ring3_set_kernel_rsp(nx->kstack_top - 64);
    /* GS/swapgs bookkeeping.
     *
     * There are two stable states of the GS MSR pair:
     *   user side  (GS_BASE = 0, KERNEL_GS_BASE = &g_ring3_gs): CPL=3, and also
     *              the first instruction of an int 0x80 handler (that gate does
     *              not swapgs);
     *   kernel side(GS_BASE = &g_ring3_gs, KERNEL_GS_BASE = 0): inside a
     *              `syscall`-entered frame, whose epilogue does one swapgs.
     * A process can only be switched out from inside a `syscall` frame (fork,
     * wait4, yield and exit are all reached through that gate), so whoever we
     * switch to must find the *kernel side* pair - otherwise its final swapgs
     * delivers the wrong base to CPL=3 and the next `syscall` loads rsp from
     * linear address 8 (observed: RSP = 0xf000ff53f000e2c3, the real-mode IVT).
     * That is exactly what a child exiting through int 0x80 leaves behind, so
     * normalise the pair on every switch instead of trusting the leaver. */
    wrmsr(IA32_GS_BASE, ring3_gs_block());
    wrmsr(IA32_KERNEL_GS_BASE, 0);
    /* Every kernel stack is identity mapped in every address space (the user PML4
     * shares the whole kernel half), so the CR3 switch can happen before the stack
     * switch. */
    write_cr3(nx->us.pml4);
    proc_ctx_switch(&prev->ksp, &nx->ksp);
    /* Resumed: another switch picked us and made us RUNNING again. */
}

/* Pick the next runnable process, round-robin. Returns 1 when it switched (the
 * caller is resumed later), 0 when nothing could run. */
static int proc_schedule(void)
{
    if (g_current < 0) return 0;
    for (int i = 1; i <= PROC_MAX; i++) {
        const int idx = (g_current + i) % PROC_MAX;
        if (g_procs[idx].state == PROC_RUNNABLE) {
            proc_switch_to(idx);
            return 1;
        }
    }
    return 0;
}

void proc_yield(void)
{
    proc_t *p = proc_current();
    if (p == NULL) return;
    p->state = PROC_RUNNABLE;
    if (!proc_schedule()) p->state = PROC_RUNNING;
}

/* ---------------- process creation / destruction ---------------- */

static int proc_alloc(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_procs[i].state == PROC_FREE && !g_procs[i].us.in_use) {
            const uint64_t kstack = g_procs[i].kstack;   /* reused, never freed here */
            memset(&g_procs[i], 0, sizeof(g_procs[i]));
            g_procs[i].kstack = kstack;
            return i;
        }
    }
    return -1;
}

/* Hand back everything a dead process owns except its kernel stack: an exiting
 * process is executing on that stack, so it may only be released when nothing can
 * be running any more (proc_session_end) or reused by the next process. */
static void proc_drop_resources(proc_t *p)
{
    if (p->us.in_use) userspace_destroy(&p->us);
    if (p->fd_slot >= PROC_SLOT_BASE) fd_table_release(p->fd_slot);
    p->fd_slot = 0;
    p->sig_slot = 0;
}

static void proc_reap(proc_t *p)
{
    const uint64_t kstack = p->kstack;
    proc_drop_resources(p);
    memset(p, 0, sizeof(*p));
    p->kstack = kstack;
    p->state = PROC_FREE;
}

/* Hand-built kernel stack so that proc_ctx_switch's "pop 6, ret" arrives at
 * proc_child_start with a 16-byte aligned rsp. */
static void proc_init_kstack(proc_t *p)
{
    uint64_t *sp = (uint64_t *)(uintptr_t)p->kstack_top;

    sp[-1] = (uint64_t)(uintptr_t)proc_child_start;   /* ret target */
    sp[-2] = 0;                                       /* rbx */
    sp[-3] = 0;                                       /* rbp */
    sp[-4] = 0;                                       /* r12 */
    sp[-5] = 0;                                       /* r13 */
    sp[-6] = 0;                                       /* r14 */
    sp[-7] = 0;                                       /* r15 */
    p->ksp = (uint64_t)(uintptr_t)&sp[-7];
}

int proc_fork(user_context_t *parent_ctx)
{
    proc_t *parent = proc_current();
    proc_t *c;
    int slot;

    if (parent == NULL || parent_ctx == NULL) return -38;    /* -ENOSYS */
    slot = proc_alloc();
    if (slot < 0) return -11;                                /* -EAGAIN */
    c = &g_procs[slot];

    if (userspace_fork_create(&c->us, &parent->us) != 0) {
        c->state = PROC_FREE;
        return -12;                                          /* -ENOMEM */
    }
    if (c->kstack == 0) {
        c->kstack = (uint64_t)moe_pmm_alloc(PROC_KSTACK_PAGES);
        if (c->kstack == 0) {
            userspace_destroy(&c->us);
            c->state = PROC_FREE;
            return -12;
        }
    }
    c->kstack_top = (c->kstack + PROC_KSTACK_BYTES) & ~0xFULL;

    /* The child resumes exactly where the parent is in user space, with rax = 0:
     * that is what makes fork() return twice. */
    c->ctx = *parent_ctx;
    c->ctx.rax = 0;
    c->ctx.cs = USER_CS;
    c->ctx.ss = USER_SS;
    c->fs_base = parent->fs_base;
    c->gs_base = parent->gs_base;

    c->fd_slot = PROC_SLOT_BASE + slot;
    c->sig_slot = c->fd_slot;
    if (fd_table_clone(parent->fd_slot, c->fd_slot) != 0) {
        userspace_destroy(&c->us);
        c->state = PROC_FREE;
        return -24;                                          /* -EMFILE */
    }
    sig_slot_clone(parent->sig_slot, c->sig_slot);

    c->is_root = 0;
    c->ppid = parent->pid;
    c->pid = g_next_pid++;
    c->exit_code = 0;
    c->exit_sig = 0;
    proc_init_kstack(c);
    c->state = PROC_RUNNABLE;
    return c->pid;
}

void proc_enter_user_first(void)
{
    proc_t *p = proc_current();

    if (p == NULL) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    /* We get here in the middle of *another* process's syscall, i.e. after its
     * swapgs. The first thing the child will do at CPL=3 is `syscall`, which
     * swaps GS again and then loads %rsp from %gs:8 - so the pair has to be
     * written explicitly instead of inherited. */
    wrmsr(IA32_GS_BASE, p->gs_base);
    wrmsr(IA32_KERNEL_GS_BASE, ring3_gs_block());
    wrmsr(IA32_FS_BASE, p->fs_base);
    ring3_set_kernel_rsp(p->kstack_top - 64);

    el0_validate_init();
    (void)el0_grant_range(USERSPACE_VA, USERSPACE_BYTES);
    el0_set_active(1);

    write_cr3(p->us.pml4);
    el0_enter_user(&p->ctx);            /* iretq into ring 3, never returns */
    for (;;) __asm__ volatile("cli; hlt");
}

void proc_exit_current(int code, int sig)
{
    proc_t *p = proc_current();
    int parent_idx;
    uint32_t status;

    if (p == NULL) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    p->exit_code = code & 0xFF;
    p->exit_sig = (sig > 0 && sig < 32) ? sig : 0;
    status = ((uint32_t)p->exit_sig << 8) | (uint32_t)(p->exit_code & 0xFF);
    p->state = PROC_ZOMBIE;

    parent_idx = proc_index_of_pid(p->ppid);
    if (parent_idx >= 0 && g_procs[parent_idx].state == PROC_BLOCKED)
        g_procs[parent_idx].state = PROC_RUNNABLE;      /* wake the waiting parent */

    if (parent_idx < 0) {
        /* No parent left: auto-reap instead of leaving an unreachable zombie. The
         * kernel stack cannot be freed here (we are running on it) - proc_reap()
         * keeps it for the next process. */
        proc_reap(p);
    }

    if (!proc_schedule()) {
        /* Nothing can run: the session is over, so leave it the way the root's
         * exit does (back to the kernel launcher). */
        ring3_resume_kernel_root((uint64_t)status);
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------- wait4 / getpid / getppid ---------------- */

int proc_wait4(int pid, uint64_t status_va, int options)
{
    proc_t *self = proc_current();
    const int self_idx = g_current;

    if (self == NULL) return -10;                            /* -ECHILD */
    if (status_va != 0 && !el0_ptr_ok(status_va, 4)) return -14;  /* -EFAULT */

    for (;;) {
        int zombie = -1;
        int live = 0;

        for (int i = 0; i < PROC_MAX; i++) {
            if (i == self_idx) continue;
            if (g_procs[i].state == PROC_FREE) continue;
            if (g_procs[i].ppid != self->pid) continue;
            if (pid > 0 && g_procs[i].pid != pid) continue;
            if (g_procs[i].state == PROC_ZOMBIE) { zombie = i; break; }
            live = 1;
        }

        if (zombie >= 0) {
            const int zpid = g_procs[zombie].pid;
            /* status = sig<<8 | code (the encoding the shell and the ring-3
             * forktest expect; a normal exit has sig == 0, so the low byte is the
             * exit code). */
            const uint32_t status =
                ((uint32_t)g_procs[zombie].exit_sig << 8) |
                (uint32_t)(g_procs[zombie].exit_code & 0xFF);
            if (status_va != 0) *(int32_t *)(uintptr_t)status_va = (int32_t)status;
            proc_reap(&g_procs[zombie]);
            return zpid;
        }
        if (!live) return -10;                               /* -ECHILD */
        if ((options & PROC_WNOHANG) != 0) return 0;

        /* Block and let the child run; we come back when it exits (or when
         * somebody else schedules us). */
        self->state = PROC_BLOCKED;
        (void)proc_schedule();
        if (g_procs[self_idx].state == PROC_BLOCKED) {
            /* nothing was runnable: don't sleep forever */
            g_procs[self_idx].state = PROC_RUNNING;
            return -11;                                      /* -EAGAIN */
        }
        self->state = PROC_RUNNING;
    }
}

int proc_getpid(void)
{
    proc_t *p = proc_current();
    if (p != NULL) return p->pid;
    return sched_cpu.current;
}

int proc_getppid(void)
{
    proc_t *p = proc_current();
    return (p != NULL) ? p->ppid : 0;
}

int proc_cow_fault_for_current(uint64_t fault_va)
{
    proc_t *p = proc_current();
    if (p == NULL) return -1;
    return userspace_cow_fault(&p->us, fault_va);
}

/* ---------------- acceptance (silent on success) ---------------- */

static uint32_t g_st_failed;

static void st_check(const char *what, int ok)
{
    if (ok) return;
    g_st_failed++;
    console_printf("fork selftest: %s ... FAIL\n", what);
}

int proc_selftest(void)
{
    proc_t *root, *child;
    user_context_t frame;
    uint64_t status = 0;
    int cpid, rc, slot;

    g_st_failed = 0;
    if (g_session) return 0;                    /* never disturb a live session */

    if (proc_session_begin() != 0) {
        console_puts("fork selftest: session ... FAIL\n");
        return 1;
    }
    /* A fabricated root: it is never entered (no iretq), it only has to be a
     * process so that fork/wait4/getppid have something to work with. The real
     * ring-3 path is covered by `run forktest`. */
    if (proc_root_create("selftest", (const uint8_t *)"\x90\x90\xeb\xfe", 4) != 0) {
        console_puts("fork selftest: root address space ... FAIL\n");
        proc_session_end();
        return 1;
    }
    root = &g_procs[0];

    memset(&frame, 0, sizeof(frame));
    frame.rax = 57;
    frame.rip = USERSPACE_VA;
    frame.rsp = USERSPACE_VA + USERSPACE_BYTES - 32;
    frame.rflags = 0x202;
    frame.cs = USER_CS;
    frame.ss = USER_SS;

    /* --- fork: parent gets the child's pid, child gets 0 --- */
    {
        const uint64_t pt0_before = ((uint64_t *)(uintptr_t)root->us.pt)[0];
        cpid = proc_fork(&frame);
        st_check("fork returns a pid in the parent", cpid > 0);
        if (cpid > 0) {
            slot = proc_index_of_pid(cpid);
            child = (slot >= 0) ? &g_procs[slot] : NULL;
            st_check("fork created a child process slot", child != NULL);
            if (child != NULL) {
                st_check("child resumes with rax = 0", child->ctx.rax == 0);
                st_check("child continues at the parent's user rip",
                         child->ctx.rip == frame.rip && child->ctx.rsp == frame.rsp);
                st_check("child is runnable", child->state == PROC_RUNNABLE);
                st_check("getppid in the child is the parent's pid",
                         ({ int save = g_current; int pp;
                            g_current = slot; pp = proc_getppid(); g_current = save;
                            pp == root->pid; }));
                /* --- copy-on-write: both sides read-only, one shared frame --- */
                {
                    const uint64_t ppte = ((uint64_t *)(uintptr_t)root->us.pt)[0];
                    const uint64_t cpte = ((uint64_t *)(uintptr_t)child->us.pt)[0];
                    st_check("fork marks both window PTEs read-only",
                             (pt0_before & PTE_W) != 0 &&
                             (ppte & PTE_W) == 0 && (cpte & PTE_W) == 0);
                    st_check("fork sets the COW bit on both sides",
                             (ppte & PTE_COW) != 0 && (cpte & PTE_COW) != 0);
                    st_check("parent and child share the same frame",
                             root->us.page_pa[0] == child->us.page_pa[0]);
                    st_check("the shared frame has two references",
                             userspace_frame_refs(root->us.page_pa[0]) == 2);
                }

                /* --- a write in the child must not reach the parent --- */
                ((uint8_t *)(uintptr_t)root->us.page_pa[0])[0] = 0xAA;
                {
                    const uint64_t shared = child->us.page_pa[0];
                    st_check("COW fault resolution succeeds for the child",
                             userspace_cow_fault(&child->us, USERSPACE_VA) == 0);
                    ((uint8_t *)(uintptr_t)child->us.page_pa[0])[0] = 0xBB;
                    st_check("the child got a private frame",
                             child->us.page_pa[0] != shared);
                    st_check("the parent's page is unchanged after the child wrote",
                             ((uint8_t *)(uintptr_t)root->us.page_pa[0])[0] == 0xAA);
                    st_check("the child sees its own byte",
                             ((uint8_t *)(uintptr_t)child->us.page_pa[0])[0] == 0xBB);
                    st_check("the old frame dropped to one reference",
                             userspace_frame_refs(shared) == 1);
                }
                /* --- the last reference is made writable without a copy --- */
                {
                    const uint64_t before = root->us.page_pa[0];
                    st_check("COW fault resolution succeeds for the parent",
                             userspace_cow_fault(&root->us, USERSPACE_VA) == 0);
                    st_check("the last reference is not copied again",
                             root->us.page_pa[0] == before);
                    st_check("the last reference becomes writable",
                             (((uint64_t *)(uintptr_t)root->us.pt)[0] & PTE_W) != 0);
                }

                /* --- wait4: the zombie child is reaped with its status --- */
                child->exit_code = 42;
                child->exit_sig = 0;
                child->state = PROC_ZOMBIE;
                rc = proc_wait4(cpid, (uint64_t)(uintptr_t)&status, 0);
                st_check("wait4 returns the child's pid", rc == cpid);
                st_check("wait4 reports the child's exit status",
                         ((uint32_t)status & 0xFF) == 42u &&
                         (((uint32_t)status >> 8) & 0xFF) == 0u);
                st_check("the reaped slot is free",
                         proc_index_of_pid(cpid) < 0);
                st_check("wait4 with no child left answers -ECHILD",
                         proc_wait4(-1, (uint64_t)(uintptr_t)&status, 0) == -10);

                /* --- WNOHANG with a live child: 0, not a pid and not ECHILD --- */
                cpid = proc_fork(&frame);
                st_check("a second fork succeeds", cpid > 0);
                if (cpid > 0) {
                    st_check("wait4(WNOHANG) answers 0 while the child is alive",
                             proc_wait4(cpid, (uint64_t)(uintptr_t)&status,
                                        PROC_WNOHANG) == 0);
                    slot = proc_index_of_pid(cpid);
                    if (slot >= 0) {
                        g_procs[slot].exit_code = 7;
                        g_procs[slot].state = PROC_ZOMBIE;
                        rc = proc_wait4(cpid, (uint64_t)(uintptr_t)&status, 0);
                        st_check("wait4 reaps the second child with its status",
                                 rc == cpid && ((uint32_t)status & 0xFF) == 7u);
                    }
                }
            }
        }
    }

    proc_session_end();
    /* Leave the kernel exactly as we found it. */
    g_current = -1;
    if (g_st_failed != 0) {
        console_printf("fork selftest: RESULT FAIL (%u check(s))\n",
                       (int64_t)g_st_failed);
        return 1;
    }
    return 0;
}
