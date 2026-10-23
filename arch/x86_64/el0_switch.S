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

/* void el0_enter_user_saved(uint64_t entry, uint64_t stack_top, uint64_t *slot)
 *   rdi = entry, rsi = user stack top, rdx = where to store the kernel rsp */
.global el0_enter_user_saved
.type el0_enter_user_saved, @function
el0_enter_user_saved:
    movq    %rsp, (%rdx)               /* remember how to get back */
    pushq   $0x23
    pushq   %rsi
    pushq   $0x2                       /* RFLAGS: IF=0 - see ring3.h; bit1 is fixed 1 */
    pushq   $0x2B
    pushq   %rdi
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

/* void el0_resume_kernel(uint64_t saved_rsp, uint64_t value)
 *   rdi = saved kernel rsp, rsi = value to return from el0_enter_user_saved() */
.global el0_resume_kernel
.type el0_resume_kernel, @function
el0_resume_kernel:
    movq    %rdi, %rsp
    movq    %rsi, %rax
    ret
.size el0_resume_kernel, .-el0_resume_kernel

.section .note.GNU-stack,"",@progbits
