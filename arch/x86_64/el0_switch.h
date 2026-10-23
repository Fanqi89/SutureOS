#ifndef STITCH_EL0_SWITCH_H
#define STITCH_EL0_SWITCH_H

#include <stitch/types.h>
#include <stitch/user_context.h>

void el0_save_user_ctx(user_context_t *ctx);
void el0_restore_user_ctx(user_context_t *ctx);
void el0_enter_user(user_context_t *uctx);

/* Saved kernel state for el0_resume_kernel(): [0] = rsp, [1] = kernel CR3. */
typedef struct el0_kernel_state {
    uint64_t rsp;                /* offset 0: kernel stack to resume on */
    uint64_t cr3;                /* offset 8: kernel address space */
    uint64_t gs_base;            /* IA32_GS_BASE to restore */
    uint64_t gs_kernel_base;     /* IA32_KERNEL_GS_BASE to restore */
} el0_kernel_state_t;

/* Enter ring 3 at `entry` with `stack_top`, running in the address space `pml4`
 * (0 = keep the current one). Records the kernel state in *state. */
void el0_enter_user_saved(uint64_t pml4, uint64_t entry, uint64_t stack_top,
                          el0_kernel_state_t *state);

/* Switch back to the kernel CR3, abandon the iretq frame and return from
 * el0_enter_user_saved() with `value`. */
void el0_resume_kernel(el0_kernel_state_t *state, uint64_t value) __attribute__((noreturn));

#endif