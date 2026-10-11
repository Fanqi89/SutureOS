#ifndef STITCH_EL0_SWITCH_H
#define STITCH_EL0_SWITCH_H

#include <stitch/types.h>
#include <stitch/user_context.h>

void el0_save_user_ctx(user_context_t *ctx);
void el0_restore_user_ctx(user_context_t *ctx);
void el0_enter_user(uint64_t user_pml4, user_context_t *uctx);

#endif