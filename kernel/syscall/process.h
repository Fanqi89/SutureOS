/* SutureOS: ring-3 processes (fork / COW / wait4 / getppid) - see process.c. */
#ifndef SUTUREOS_PROCESS_H
#define SUTUREOS_PROCESS_H

#include <stitch/types.h>
#include <stitch/user_context.h>
#include "userspace.h"

/* Slots in the process table. The slot index is *not* the pid; pids are handed out
 * from a counter so a name is never reused while a wait4 could still match it. */
#define PROC_MAX 8

/* Per-process descriptor/signal slot base. kernel/syscall/syscalls.c keeps its fd
 * table (`fd_tables[SCHED_MAX_TASKS][32]`) and kernel/syscall/signal.c keeps its
 * signal state in arrays indexed by a "task slot"; processes use the top half of
 * that range (8..15 for PROC_MAX == 8) so a fork can really clone the descriptor
 * table instead of every process of a session sharing the kernel task's one. */
#define PROC_SLOT_BASE 8

#if (PROC_SLOT_BASE + PROC_MAX) > 16
#error "process slots do not fit the 16-entry fd/signal arrays"
#endif

enum {
    PROC_FREE = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
};

/* ---- session lifecycle (one ring-3 program run + its children) ---- */
int  proc_session_begin(void);     /* 0 on success, -1 when a session is active */
void proc_session_end(void);       /* destroy every process of the session */

/* Create the session's root process (pid 1) and load `blob` into its window.
 * Returns 0, or -1 when the address space/kernel stack could not be allocated. */
int  proc_root_create(const char *name, const uint8_t *blob, uint64_t len);

/* The address space of the currently running process (NULL outside a session). */
userspace_t *proc_current_us(void);
userspace_t *proc_root_us(void);

int  proc_current_pid(void);       /* pid of the running process, or -1 */
int  proc_current_is_root(void);

/* Top of the running process's kernel stack (0 outside a process). The `syscall`
 * entry loads %rsp from it, so every process syscall starts on its own stack. */
uint64_t proc_current_kstack_top(void);

/* fork(): clone the current process. `parent_ctx` is the syscall frame of the
 * caller (its user_context_t), which becomes the child's starting point with
 * rax = 0. Returns the child's pid in the parent, 0 in the child (the child is
 * resumed later by proc_schedule), or -errno. */
int  proc_fork(user_context_t *parent_ctx);

/* wait4(): reap a zombie child. Returns the child's pid, 0 for WNOHANG with
 * nothing ready, or -errno (-ECHILD when no child matches). */
int  proc_wait4(int pid, uint64_t status_va, int options);

/* getpid()/getppid() for the running process. Outside a process they fall back to
 * the kernel scheduler's current task slot, which is what the shell sees. */
int  proc_getpid(void);
int  proc_getppid(void);

/* Terminate the running (non-root) process: it becomes a zombie until the parent
 * reaps it, or is destroyed at once when the parent is already gone. Never
 * returns. */
void proc_exit_current(int code, int sig) __attribute__((noreturn));

/* Yield to the next runnable process of the session (no-op outside one). */
void proc_yield(void);

/* Resolve a copy-on-write #PF for the running process. 0 = the faulting
 * instruction can be retried, -1 = not a COW fault of this process. */
int  proc_cow_fault_for_current(uint64_t fault_va);

/* Signal/fd slot of the running process (see PROC_SLOT_BASE), or -1. */
int  proc_current_slot(void);

/* Map a pid to its descriptor/signal slot. Returns the slot, or -1 for "not a
 * process of this session" - callers then fall back to the kernel task slot. */
int  proc_slot_for_pid(int pid);

/* First-entry trampoline target (arch/x86_64/el0_switch.S `proc_child_start`
 * calls this): switch to the process address space and iretq into user mode with
 * the context proc_fork() prepared. Never returns. */
void proc_enter_user_first(void) __attribute__((noreturn));

/* Kernel stack helper for the switch (arch/x86_64/el0_switch.S):
 *   proc_ctx_switch(uint64_t *from_ksp, uint64_t *to_ksp) */
void proc_ctx_switch(uint64_t *from_ksp, uint64_t *to_ksp);

/* Silent-on-success self test: fork returns >0 in the parent and 0 in the child,
 * wait4 returns the child pid and exit status, getppid returns the parent, and a
 * COW write in the child leaves the parent's page untouched. Prints a
 * "fork selftest: ... FAIL" line only when a check fails. */
int  proc_selftest(void);

#endif /* SUTUREOS_PROCESS_H */
