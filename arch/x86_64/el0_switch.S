/* SutureOS: ring-3 entry / exit primitives.
 *
 * The previous version of el0_enter_user() was unusable and had never been
 * called, so nothing caught it:
 *   - it pushed rdi/rsi and then popped them in the wrong order, so CR3 was
 *     loaded with the *context pointer* and the context was read from the PML4;
 *   - every user_context_t offset was wrong (fs_base was read from offset 112,
 *     which is rax; r14/r15 from 104/112);
 *   - it built a 6-entry iret frame and then executed sysretq, which ignores
 *     that frame and needs rcx=rip / r11=rflags instead.
 *
 * This file now provides three small, honest primitives:
 *
 *   el0_enter_user(ctx)                      iretq into ring 3 from a context
 *   el0_enter_user_saved(entry, stack, slot) same, but remembers the kernel rsp
 *                                            so the user program can come back
 *   el0_resume_kernel(rsp, value)            "return from the user program":
 *                                            restore the saved kernel stack and
 *                                            return to C with value in rax
 *
 * Address space: SutureOS keeps ONE page table (the kernel identity map). The
 * user code and stack are PMM-owned pages exposed with U/S=1 by
 * mmu_map_user_identity(); every other page stays supervisor-only, so ring 3
 * cannot reach kernel memory without a CR3 switch.
 *
 * Field offsets of user_context_t (include/stitch/user_context.h):
 *   r15 0, r14 8, r13 16, r12 24, r11 32, r10 40, r9 48, r8 56, rbp 64,
 *   rdi 72, rsi 80, rdx 88, rcx 96, rbx 104, rax 112, rip 120, cs 128,
 *   rflags 136, rsp 144, ss 152, fs_base 160, gs_base 168
 */

.section .text

/* void el0_enter_user(user_context_t *ctx)   [rdi = ctx] */
.global el0_enter_user
.type el0_enter_user, @function
el0_enter_user:
    movq    120(%rdi), %rcx            /* scratch: rip    */
    movq    136(%rdi), %rdx            /* scratch: rflags */
    movq    144(%rdi), %r8             /* scratch: rsp    */
    pushq   $0x23                      /* SS   = user data, DPL=3 */
    pushq   %r8                        /* RSP  */
    pushq   %rdx                       /* RFLAGS */
    pushq   $0x2B                      /* CS   = user code, DPL=3, L=1 */
    pushq   %rcx                       /* RIP  */
    /* restore GPRs; rdi (the context pointer) must be last */
    movq      0(%rdi), %r15
    movq      8(%rdi), %r14
    movq     16(%rdi), %r13
    movq     24(%rdi), %r12
    movq     32(%rdi), %r11
    movq     40(%rdi), %r10
    movq     48(%rdi), %r9
    movq     56(%rdi), %r8
    movq     64(%rdi), %rbp
    movq     88(%rdi), %rdx
    movq     96(%rdi), %rcx
    movq    104(%rdi), %rbx
    movq    112(%rdi), %rax
    movq     80(%rdi), %rsi
    movq     72(%rdi), %rdi
    iretq
.size el0_enter_user, .-el0_enter_user

/* void el0_enter_user_saved(uint64_t pml4, uint64_t entry, uint64_t stack_top,
 *                           uint64_t *slot)
 *   rdi = process PML4 (0 = keep the current one), rsi = entry,
 *   rdx = user stack top, rcx = where to store the kernel state (rsp + kernel CR3) */
.global el0_enter_user_saved
.type el0_enter_user_saved, @function
el0_enter_user_saved:
    /* Save the callee-saved registers first: coming back through
     * el0_resume_kernel() must restore them or the C caller's locals (which live
     * in rbx/rbp/r12-r15) are whatever the user program left behind. */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    /* [slot] = rsp, [slot+8] = kernel CR3: el0_resume_kernel() needs both. */
    movq    %rsp, (%rcx)
    movq    %cr3, %rax
    movq    %rax, 8(%rcx)
    /* build the iretq frame: SS, RSP, RFLAGS, CS, RIP */
    pushq   $0x23
    pushq   %rdx
    pushq   $0x202                     /* RFLAGS: IF=1 - interrupts stay enabled at CPL=3 */
    pushq   $0x2B
    pushq   %rsi
    /* Switch to the process address space now: the zeroing block below clears
     * %rdi, which still holds the PML4 at this point. 0 = keep the kernel's. */
    testq   %rdi, %rdi
    jz      1f
    movq    %rdi, %cr3
1:
    /* a fresh user program starts with clean registers */
    xorq    %rax, %rax
    xorq    %rbx, %rbx
    xorq    %rcx, %rcx
    xorq    %rdx, %rdx
    xorq    %rsi, %rsi
    xorq    %rdi, %rdi
    xorq    %rbp, %rbp
    xorq    %r8, %r8
    xorq    %r9, %r9
    xorq    %r10, %r10
    xorq    %r11, %r11
    xorq    %r12, %r12
    xorq    %r13, %r13
    xorq    %r14, %r14
    xorq    %r15, %r15
    iretq
.size el0_enter_user_saved, .-el0_enter_user_saved

/* void el0_resume_kernel(saved_state, value)
 *   rdi = pointer to {kernel rsp, kernel CR3}, rsi = value to return */
.global el0_resume_kernel
.type el0_resume_kernel, @function
el0_resume_kernel:
    movq    8(%rdi), %rax
    testq   %rax, %rax
    jz      1f
    movq    %rax, %cr3                 /* back to the kernel address space */
1:  movq    (%rdi), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    movq    %rsi, %rax
    ret
.size el0_resume_kernel, .-el0_resume_kernel

/* ---------------- kernel-stack context switch (fork/wait4 scheduler) ----------------
 *
 * void proc_ctx_switch(uint64_t *from_ksp, uint64_t *to_ksp)   [rdi, rsi]
 *
 * Every ring-3 process owns a kernel stack (kernel/syscall/process.c). Switching
 * processes is therefore a kernel stack switch: save the callee-saved registers
 * the SysV ABI requires to survive a call, store rsp in *from_ksp, load *to_ksp,
 * restore the registers and `ret` - which returns into the *other* process's
 * suspended proc_ctx_switch() call. A process that has never run has a hand-built
 * frame here whose `ret` target is proc_child_start below.
 *
 * Why this is enough: the whole kernel map is shared by every process address
 * space (userspace.c copies the kernel PML4), so a kernel stack is mapped
 * everywhere and the caller may (and does) switch CR3 before switching stacks.
 * Interrupts are off here (both gates enter with IF=0), so the stack swap is not
 * preemptible. */
.global proc_ctx_switch
.type proc_ctx_switch, @function
proc_ctx_switch:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)               /* from->ksp = current rsp */
    movq    (%rsi), %rsp               /* rsp = to->ksp          */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret
.size proc_ctx_switch, .-proc_ctx_switch

/* First instruction of a forked process: proc_ctx_switch() has just "returned"
 * here with a 16-byte aligned rsp (so a call pushes 8 and the C callee sees the
 * usual rsp % 16 == 8). proc_enter_user_first() switches to the child address
 * space and iretqs with the context fork prepared - it never returns; the ud2 is
 * only there so a bug is a fault instead of a wild jump. */
.global proc_child_start
.type proc_child_start, @function
proc_child_start:
    call    proc_enter_user_first
    ud2
.size proc_child_start, .-proc_child_start

.extern proc_enter_user_first

.section .note.GNU-stack,"",@progbits
