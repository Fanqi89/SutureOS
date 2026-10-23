/* SutureOS: the first ring-3 program (user/init.S) and the runtime around it.
 *
 * Why this file exists: the audit found that every piece of ring-3 machinery was
 * present but never used - sys_execve had zero call sites, kmain never started
 * /init, and el0_enter_user() was broken in three independent ways (see
 * arch/x86_64/el0_switch.S). This wires the chain end to end:
 *
 *   ring3_run_init()  allocate PMM pages -> map them U/S=1 -> copy the embedded
 *                     user blob -> enable SSE -> enter CPL=3 via iretq
 *   syscall path      user/init.S runs `syscall` (LSTAR) -> syscall_entry ->
 *                     syscall_dispatch_c -> the kernel's normal syscall table
 *   ring3_exit_user() user calls exit(60) -> we abandon the iretq frame and
 *                     return to ring3_run_init() with the exit code
 *
 * Interrupts: the user program is entered with IF=0. A hardware interrupt taken
 * while at CPL=3 currently returns through the legacy same-CPL ISR stubs and
 * resumes in the kernel at a garbage rip, so the bring-up keeps them off;
 * the program does not sleep and needs no timer ticks. Re-enabling IF at CPL=3
 * (and the CPL=3 return path in arch/x86_64/isr_stubs.S) is the next step.
 *
 * Isolation: the kernel keeps one page table, so only the specific pages handed
 * out here are reachable from CPL=3 (U/S=1); kernel text, the heap and all other
 * RAM stay supervisor-only. el0_validate is switched on as well so the syscall
 * layer rejects pointers outside the granted range.
 */
#ifndef SUTUREOS_RING3_H
#define SUTUREOS_RING3_H

#include <stitch/types.h>

#define RING3_USER_PAGES   8                       /* 32 KiB: image + stack */
#define RING3_USER_BYTES   (RING3_USER_PAGES * 4096)

/* Run the embedded ring-3 program once and return its exit status (or -1 when it
 * could not be started). Prints "[ring3] ..." diagnostics. */
int ring3_run_init(void);

/* Called from sys_exit() when the caller is the ring-3 program: never returns
 * (it abandons the syscall frame and resumes ring3_run_init). */
void ring3_exit_user(uint64_t code) __attribute__((noreturn));

/* Fixed virtual address the ported userland is linked at (user/lib/user.ld). */
#define USER_VA_BASE        0x40000000ull
#define USER_CODE_PAGES     8
#define USER_STACK_PAGES    8
#define USER_VA_BYTES       ((USER_CODE_PAGES + USER_STACK_PAGES) * 4096ull)

/* Run a flat user blob (linked at USER_VA_BASE) in ring 3 and return its exit
 * status. `name` is only used for log lines. */
int user_run_blob(const char *name, const uint8_t *blob, uint64_t len);

/* Run one of the programs compiled into the kernel (see user/apps/*.c).
 * Returns the exit status, or -1 when the name is unknown. */
int user_run_builtin(const char *name);

/* Names of the built-in programs, space separated (for `run` with no argument). */
const char *user_builtin_list(void);

/* 1 while the ring-3 program is executing. */
int ring3_active(void);

/* Last exit code (for the shell's `ring3` command and the boot log). */
int ring3_last_exit(void);

/* 1 once the boot-time user program has been started (it may only run once). */
int ring3_ran_ever(void);

/* Acceptance: page permissions (user pages U=1, kernel text U=0), SSE state,
 * GS base for the syscall entry, execution of the embedded blob, and the exit
 * code. Prints "ring3 selftest: ... PASS/FAIL" + RESULT; 0 when all passed. */
int ring3_selftest(void);

#endif /* SUTUREOS_RING3_H */
