/* SutureOS: Minimal timer interrupt handler (vector 0x20)
 * Properly handles same-CPL interrupt return in 64-bit mode.
 * For same-CPL interrupts, CPU pushes RIP, CS, RFLAGS only.
 * iretq expects: RIP, CS, RFLAGS (no SS/RSP for same CPL).
 */
.section .text
.global timer_irq_handler_asm
.type timer_irq_handler_asm, @function

timer_irq_handler_asm:
    /* CPU has pushed: RIP, CS, RFLAGS (no error code for vector 0x20) */
    /* Stack top: RFLAGS, CS, RIP */

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

    /* Call C handler to tick scheduler and send EOI */
    call kernel_timer_tick_c

    /* Restore registers in reverse order */
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
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax

    /* iretq for same-CPL interrupt: stack has RIP, CS, RFLAGS (pushed by CPU) */
    iretq
.size timer_irq_handler_asm, . - timer_irq_handler_asm

.section .note.GNU-stack,"",@progbits