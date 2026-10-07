/* SutureOS: Minimal timer interrupt handler (vector 0x20)
 * Properly handles same-CPL interrupt return in 64-bit mode.
 * iretq requires 5 stack values (RIP, CS, RFLAGS, SS, RSP) but
 * CPU only pushes 3 (RIP, CS, RFLAGS) for same-CPL interrupts.
 * We must synthesize SS/RSP before iretq.
 */

.section .text
.global timer_irq_handler_asm
.type timer_irq_handler_asm, @function

timer_irq_handler_asm:
    /* CPU has pushed: RIP, CS, RFLAGS (no error code for vector 0x20) */
    /* Stack: [RFLAGS] [CS] [RIP] <- top */

    /* Save registers we'll clobber */
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Call C handler to do EOI */
    movq %rsp, %rdi          /* Pass frame pointer (rsp) as first arg */
    call kernel_timer_tick_c

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Synthesize SS/RSP for iretq (kernel data segment 0x10, current RSP) */
    pushq $0x10              /* SS = kernel data segment */
    pushq %rsp               /* RSP = current stack pointer */

    iretq
.size timer_irq_handler_asm, . - timer_irq_handler_asm

/* C function called from assembly - just does EOI */
.global kernel_timer_tick_c
.type kernel_timer_tick_c, @function
kernel_timer_tick_c:
    /* Send EOI to PIC master */
    movb $0x20, %al
    outb %al, $0x20
    ret
.size kernel_timer_tick_c, . - kernel_timer_tick_c

.section .note.GNU-stack,"",@progbits