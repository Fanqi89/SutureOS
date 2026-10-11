/* SutureOS: EL0 入口 (修复版)
 * rdi = user_pml4, rsi = user_context_t*
 * 完整流程：
 *   1. 切换 CR3 到用户页表
 *   2. 从 user_context_t 恢复 FS/GS base (wrmsr)
 *   3. swapgs 回用户 GS.base
 *   4. 在用户栈上构造 sysret 所需帧
 *   5. 恢复通用寄存器
 *   6. sysretq
 */
.section .text
.global el0_enter_user
.type el0_enter_user, @function

el0_enter_user:
    pushq %rdi
    pushq %rsi

    popq %rdi
    movq %rdi, %cr3

    popq %rsi
    movq 112(%rsi), %rax            /* fs_base */
    movq %rax, %rcx
    movq $0xC0000100, %rdx          /* IA32_FS_BASE */
    wrmsr

    movq 120(%rsi), %rax            /* gs_base */
    movq $0xC0000101, %rcx          /* IA32_GS_BASE */
    wrmsr

    swapgs

    movq 104(%rsi), %rsp

    pushq $0x2B                     /* SS = 用户数据段 | DPL=3 */
    pushq 104(%rsi)                 /* RSP (用户栈顶) */
    pushq 128(%rsi)                 /* RFLAGS */
    pushq $0x23                     /* CS = 用户代码段 | DPL=3 */
    pushq 136(%rsi)                 /* RIP */

    movq (%rsi), %rax               /* rax */
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rcx
    movq 24(%rsi), %rdx
    movq 32(%rsi), %rsi
    movq 40(%rsi), %rdi
    movq 48(%rsi), %rbp
    movq 56(%rsi), %r8
    movq 64(%rsi), %r9
    movq 72(%rsi), %r10
    movq 80(%rsi), %r11
    movq 88(%rsi), %r12
    movq 96(%rsi), %r13
    movq 104(%rsi), %r14
    movq 112(%rsi), %r15

    sysretq
.size el0_enter_user, .-el0_enter_user

.section .note.GNU-stack,"",@progbits