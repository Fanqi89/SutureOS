/* SutureOS: x86_64 syscall 入口
 * CPU 已做 swapgs、加载 IA32_LSTAR、rcx=rip、r11=rflags
 * 约定：内核栈在 GS.base + 0 (per-cpu)，用户栈在 rsp
 * 保存所有寄存器 -> 调用 C 分发器 -> 恢复 -> sysret
 */
.section .text
.global syscall_entry
.type syscall_entry, @function

syscall_entry:
    /* 1. swapgs：用户 GS.base -> 内核 GS.base (存内核栈顶) */
    swapgs
    /* 2. 保存用户 rsp 到 GS:0，加载内核栈 GS:8 */
    movq %rsp, %gs:0
    movq %gs:8, %rsp

    /* 3. 保存所有易失寄存器（syscall 约定：rcx=rip, r11=rflags） */
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %rbx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* 4. 构造参数：rdi=ptr, rsi.. 由分发器从栈上取 */
    movq %rsp, %rdi                 /* arg1 = user_context_t* */

    call syscall_dispatch_c         /* 返回值在 rax */

    /* 5. 恢复寄存器 */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbx
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax

    /* 6. 恢复用户 rsp，切回用户 GS.base */
    movq %gs:0, %rsp
    swapgs

    /* 7. sysret 返回：rcx=rip, r11=rflags 已在寄存器 */
    sysretq
.size syscall_entry, .-syscall_entry

.extern syscall_dispatch_c

.section .note.GNU-stack,"",@progbits