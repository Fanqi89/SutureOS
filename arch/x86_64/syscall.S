/* SutureOS: x86_64 `syscall` entry (the ring-3 -> kernel path).
 *
 * Per the SDM, `syscall` does NOT swap GS or save anything: it sets rcx = user
 * rip, r11 = user rflags, loads CS/SS from STAR and jumps to IA32_LSTAR. So the
 * entry code owns the whole job:
 *
 *   1. swapgs           -> %gs becomes the kernel per-CPU block
 *   2. save user rsp in gs:0, load the kernel stack top from gs:8
 *   3. build a COMPLETE user_context_t on the kernel stack. The old version only
 *      pushed the 15 GPRs, so ctx->rip / ctx->cs / ctx->rflags / ctx->rsp were
 *      read from whatever was on the stack above the frame - which made the
 *      signal-delivery hook on the return path (kernel/syscall/syscall.c) read a
 *      garbage cs and never fire.
 *   4. call syscall_dispatch_c(ctx), put its return value into ctx->rax
 *   5. drop the GPR part of the frame and iretq (not sysretq: iretq consumes the
 *      CS/SS/RSP/RFLAGS we just saved, so a signal handler that rewrote them is
 *      honoured, and it cannot be confused by a non-canonical rcx the way sysret
 *      can).
 *
 * Two extra slots (fs_base/gs_base) are pushed above the iret frame so that the
 * layout matches user_context_t exactly; iretq leaves them on the kernel stack,
 * where the next entry overwrites them (rsp is reset from gs:8 every time).
 */
.section .text
.global syscall_entry
.type syscall_entry, @function

syscall_entry:
    swapgs
    movq    %rsp, %gs:0                /* save the user rsp              */
    movq    %gs:8, %rsp                /* load this CPU's kernel stack   */

    /* user_context_t tail: gs_base, fs_base, ss, rsp, rflags, cs, rip */
    pushq   $0                         /* gs_base (offset 168) */
    pushq   $0                         /* fs_base (offset 160) */
    pushq   $0x23                      /* ss      (offset 152) */
    pushq   %gs:0                      /* rsp     (offset 144) */
    pushq   %r11                       /* rflags  (offset 136) */
    pushq   $0x2B                      /* cs      (offset 128) */
    pushq   %rcx                       /* rip     (offset 120) */

    /* then the GPRs in reverse field order, so r15 lands at offset 0 */
    pushq   %rax                       /* 112 */
    pushq   %rbx                       /* 104 */
    pushq   %rcx                       /*  96 (rcx is clobbered by syscall) */
    pushq   %rdx                       /*  88 */
    pushq   %rsi                       /*  80 */
    pushq   %rdi                       /*  72 */
    pushq   %rbp                       /*  64 */
    pushq   %r8                        /*  56 */
    pushq   %r9                        /*  48 */
    pushq   %r10                       /*  40 */
    pushq   %r11                       /*  32 */
    pushq   %r12                       /*  24 */
    pushq   %r13                       /*  16 */
    pushq   %r14                       /*   8 */
    pushq   %r15                       /*   0 */

    movq    %rsp, %rdi                 /* rdi = user_context_t* */
    call    syscall_dispatch_c         /* returns the syscall result in rax */

    movq    %rax, 112(%rsp)            /* ctx->rax = result */

    /* Restore EVERY general-purpose register from the frame, not just the iretq
     * part. The frame is the kernel's authoritative copy of the interrupted user
     * state: the signal machinery (kernel/syscall/signal.c) rewrites ctx->rdi to
     * pass the signal number to a handler and ctx->rip/rsp to enter it, and
     * rt_sigreturn(15) copies a whole saved user_context_t back over the frame.
     * Dropping the 15 GPR slots (the old `addq $120,%rsp`) threw those edits away
     * and returned to user mode with whatever registers the C dispatcher left
     * behind - which is why a signal handler saw a garbage argument even though
     * ctx->rdi had been set correctly. It also silently broke the SysV
     * callee-saved registers rbx/rbp/r12-r15 across a syscall. */
    movq     0(%rsp), %r15
    movq     8(%rsp), %r14
    movq    16(%rsp), %r13
    movq    24(%rsp), %r12
    movq    32(%rsp), %r11
    movq    40(%rsp), %r10
    movq    48(%rsp), %r9
    movq    56(%rsp), %r8
    movq    64(%rsp), %rbp
    movq    72(%rsp), %rdi
    movq    80(%rsp), %rsi
    movq    88(%rsp), %rdx
    movq    96(%rsp), %rcx
    movq   104(%rsp), %rbx
    movq   112(%rsp), %rax

    addq    $120, %rsp                 /* drop the 15 GPR slots */
    swapgs                             /* back to the user GS */
    iretq                              /* rip, cs, rflags, rsp, ss */
.size syscall_entry, .-syscall_entry

.extern syscall_dispatch_c

.section .note.GNU-stack,"",@progbits
