/* StitchOS module: interrupt stubs (moos_isr_stub_N)
 * Ported from: MOOS (C:\Users\fanqi\Desktop\自研操作系统\MOOS\NativeLib\interrupts_asm.asm)
 * Original license: Unlicense (MOOS LICENSE = public-domain dedication; MIT also cited)
 * Changes: Fixed interrupt frame layout for proper iretq in 64-bit mode.
 *          For same-CPL interrupts, CPU pushes RIP, CS, RFLAGS only.
 *          iretq expects: RIP, CS, RFLAGS (no SS/RSP for same CPL).
 *          We do NOT synthesize SS/RSP for same-CPL interrupts.
 */
    .section .text

/* No CPU error code: stub pushes a synthetic 0 to keep the frame uniform. */
.macro ISR_STUB_NOERR vec
.global moos_isr_stub_\vec
.type   moos_isr_stub_\vec, @function
moos_isr_stub_\vec:
    /* CPU pushed: RIP, CS, RFLAGS (no error code) */
    /* Stack top: RFLAGS, CS, RIP */

    pushq $0                      /* synthetic error code */
    pushq $\vec                    /* vector number */

    /* Save all volatile registers per SysV AMD64 ABI */
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

    /* rbp is frame pointer for C handler */
    movq %rsp, %rdi              /* arg1 = frame pointer */
    movl    $\vec, %esi             /* arg2 = vector number (32-bit immediate) */

    call moos_intr_handler

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

    /* Drop synthetic error code + vector */
    addq $16, %rsp

    /* iretq for same-CPL interrupt: stack has RIP, CS, RFLAGS (pushed by CPU) */
    iretq
.endm

/* CPU pushes an error code: stub must NOT add another one. */
.macro ISR_STUB_ERR vec
.global moos_isr_stub_\vec
.type   moos_isr_stub_\vec, @function
moos_isr_stub_\vec:
    /* CPU pushed: RIP, CS, RFLAGS, error_code */
    /* Stack top: error_code, RFLAGS, CS, RIP */

    pushq $\vec                    /* vector number */

    /* Save all volatile registers */
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

    movq %rsp, %rdi              /* arg1 = frame pointer */
    movl    $\vec, %esi             /* arg2 = vector (32-bit immediate) */

    call moos_intr_handler

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
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax

    /* Drop CPU error code + vector */
    addq $16, %rsp

    /* iretq for same-CPL interrupt: stack has RIP, CS, RFLAGS (pushed by CPU) */
    iretq
.endm

/* ---- vectors 0..47 WITHOUT a CPU error code ------------------------------ */
.irp vec, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
ISR_STUB_NOERR \vec
.endr

/* ---- vectors WITH a CPU error code (list from MOOS IDT.cs) --------------- */
.irp vec, 8,10,11,12,13,14,17,21,29,30
ISR_STUB_ERR \vec
.endr

/* ---- int 0x80 system call gate ------------------------------------------ */
ISR_STUB_NOERR 128

.section .note.GNU-stack,"",@progbits