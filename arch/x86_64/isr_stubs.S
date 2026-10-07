/* StitchOS module: interrupt stubs (moos_isr_stub_N)
 * Ported from: MOOS (C:\Users\fanqi\Desktop\自研操作系统\MOOS\NativeLib\interrupts_asm.asm)
 * Original license: Unlicense (MOOS LICENSE = public-domain dedication; MIT also cited)
 * Changes: MASM (pushaq MACRO / intr MACRO, hand-listed intr 0..255) rewritten
 *          as GAS .macro + .irp for clang's integrated assembler. Differences:
 *          - only vectors 0..47 and 0x80 are generated (those are the gates
 *            module A installs); MOOS generated all 256;
 *          - error-code vectors (8,10,11,12,13,14,17,21,29,30 - the list MOOS
 *            IDT.cs switches on) skip the synthetic 0 push;
 *          - MOOS kept rbp outside pushaq as its frame base; here rbp is part
 *            of the saved-GPR block and matches struct moos_regs;
 *          - MOOS called intr_handler via the Windows x64 ABI (rcx=vector,
 *            rdx=frame); that pair is still loaded for provenance, then
 *            mirrored into SysV AMD64 rdi/rsi which clang's ABI reads.
 *
 * Frame layout is locked against kernel\interrupts\intr.h struct moos_regs:
 *   push order (first = highest address): rbp, r15..r8, rdi, rsi, rbx, rdx,
 *   rcx, rax (last = lowest address) => memory low->high:
 *   rax rcx rdx rbx rsi rdi r8..r15 rbp vector error rip cs rflags [rsp ss]
 */

    .text

/* No CPU error code: stub pushes a synthetic 0 to keep the frame uniform. */
    .macro ISR_STUB_NOERR vec
    .global moos_isr_stub_\vec
    .type   moos_isr_stub_\vec, @function
moos_isr_stub_\vec:
    pushq   $0                      /* synthetic error code */
    pushq   $\vec                   /* vector number         */
    pushq   %rbp
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %r11
    pushq   %r10
    pushq   %r9
    pushq   %r8
    pushq   %rdi
    pushq   %rsi
    pushq   %rbx
    pushq   %rdx
    pushq   %rcx
    pushq   %rax
    movq    %rsp, %rdx              /* MOOS ABI: frame pointer in rdx */
    movq    $\vec, %rcx             /* MOOS ABI: vector number in rcx  */
    movq    %rdx, %rsi              /* SysV ABI: arg2 = frame  */
    movq    %rcx, %rdi              /* SysV ABI: arg1 = vector */
    call    moos_intr_handler
    popq    %rax
    popq    %rcx
    popq    %rdx
    popq    %rbx
    popq    %rsi
    popq    %rdi
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15
    popq    %rbp
    addq    $16, %rsp               /* drop vector + synthetic error code */
    iretq
    .endm

/* CPU pushes an error code: stub must NOT add another one. */
    .macro ISR_STUB_ERR vec
    .global moos_isr_stub_\vec
    .type   moos_isr_stub_\vec, @function
moos_isr_stub_\vec:
    pushq   $\vec                   /* vector number (error code already on stack) */
    pushq   %rbp
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %r11
    pushq   %r10
    pushq   %r9
    pushq   %r8
    pushq   %rdi
    pushq   %rsi
    pushq   %rbx
    pushq   %rdx
    pushq   %rcx
    pushq   %rax
    movq    %rsp, %rdx              /* MOOS ABI: frame pointer in rdx */
    movq    $\vec, %rcx             /* MOOS ABI: vector number in rcx  */
    movq    %rdx, %rsi              /* SysV ABI: arg2 = frame  */
    movq    %rcx, %rdi              /* SysV ABI: arg1 = vector */
    call    moos_intr_handler
    popq    %rax
    popq    %rcx
    popq    %rdx
    popq    %rbx
    popq    %rsi
    popq    %rdi
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15
    popq    %rbp
    addq    $16, %rsp               /* drop vector + CPU error code */
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
