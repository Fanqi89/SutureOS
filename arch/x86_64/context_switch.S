/* SutureOS module: context switch assembly (cinux_ prefix)
 * Ported from: Cinux (C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\arch\x86_64\context_switch.S)
 * Original license: GPL-3.0
 * Changes: symbol prefix cinux_; no FPU/SSE state save; segment bases
 *          saved/restored via MSR_GS_BASE/MSR_FS_BASE; trampoline is a
 *          C function called as initial rip for new tasks.
 */
    .text
    .globl cinux_context_switch

/* void cinux_context_switch(cinux_cpu_context_t *from, cinux_cpu_context_t *to)
 * rdi = from, rsi = to
 * Struct offsets: r15=0x00 r14=0x08 r13=0x10 r12=0x18 rbp=0x20 rbx=0x28
 *                 rsp=0x30 rip=0x38 rflags=0x40 kgs_base=0x48 fs_base=0x50
 */
cinux_context_switch:
    /* Save current context to *from */
    movq %r15, 0x00(%rdi)
    movq %r14, 0x08(%rdi)
    movq %r13, 0x10(%rdi)
    movq %r12, 0x18(%rdi)
    movq %rbp, 0x20(%rdi)
    movq %rbx, 0x28(%rdi)
    movq %rsp, 0x30(%rdi)
    movq (%rsp), %rax          /* return address = saved rip */
    movq %rax, 0x38(%rdi)
    pushfq
    popq %rax
    movq %rax, 0x40(%rdi)      /* rflags */
    movq $0xC0000101, %rcx     /* MSR_GS_BASE */
    rdmsr
    shlq $32, %rdx
    orq  %rax, %rdx
    movq %rdx, 0x48(%rdi)      /* kgs_base */
    movq $0xC0000100, %rcx     /* MSR_FS_BASE */
    rdmsr
    shlq $32, %rdx
    orq  %rax, %rdx
    movq %rdx, 0x50(%rdi)      /* fs_base */

    /* Load new context from *to */
    movq 0x00(%rsi), %r15
    movq 0x08(%rsi), %r14
    movq 0x10(%rsi), %r13
    movq 0x18(%rsi), %r12
    movq 0x20(%rsi), %rbp
    movq 0x28(%rsi), %rbx
    movq 0x30(%rsi), %rsp
    movq 0x40(%rsi), %rax      /* rflags */
    pushq %rax
    popfq
    movq 0x48(%rsi), %rdx      /* kgs_base */
    movq %rdx, %rax
    shrq $32, %rdx
    movq $0xC0000101, %rcx     /* MSR_GS_BASE */
    wrmsr
    movq 0x50(%rsi), %rdx      /* fs_base */
    movq %rdx, %rax
    shrq $32, %rdx
    movq $0xC0000100, %rcx     /* MSR_FS_BASE */
    wrmsr
    movq 0x38(%rsi), %rax      /* target rip */
    jmp *%rax

    .section .note.GNU-stack,"",@progbits
