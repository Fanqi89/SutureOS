#ifndef STITCH_EL0_SWITCH_H
#define STITCH_EL0_SWITCH_H

#include <stitch/types.h>
#include <stitch/user_context.h>

void el0_save_user_ctx(user_context_t *ctx);
void el0_restore_user_ctx(user_context_t *ctx);
void el0_enter_user(user_context_t *uctx);

/* Enter ring 3 at entry/stack_top; record the current kernel rsp in *slot so
 * el0_resume_kernel() can return to the caller when the program exits. */
void el0_enter_user_saved(uint64_t entry, uint64_t stack_top, uint64_t *slot);

/* Abandon the iretq frame and return from el0_enter_user_saved() with `value`. */
void el0_resume_kernel(uint64_t saved_rsp, uint64_t value) __attribute__((noreturn));

#endif