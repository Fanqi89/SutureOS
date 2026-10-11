/* SutureOS: 用户态上下文结构（syscall/sysret 返回帧） */
#ifndef STITCH_USER_CONTEXT_H
#define STITCH_USER_CONTEXT_H

#include <stitch/types.h>

/* 用户态上下文：syscall 入口保存、sysret 返回帧
 * 对应 syscall.S 中的压栈顺序，必须保持同步 */
typedef struct user_context {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
    uint64_t fs_base, gs_base;
} user_context_t;

_Static_assert(sizeof(user_context_t) == 176, "user_context_t size");

/* 系统调用参数：rdi, rsi, rdx, r10, r8, r9 对应 1~6 号参数
 * 返回值在 rax */
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t);

void user_context_init(user_context_t *ctx,
                       uint64_t entry, uint64_t stack_top,
                       uint64_t arg);
void syscall_init(void);
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5,
                          uint64_t a6);

#endif