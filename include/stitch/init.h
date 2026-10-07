/* SutureOS Phase-2: Kernel hardware initialization API */
#ifndef STITCH_INIT_H
#define STITCH_INIT_H

#include <stitch/types.h>

struct moos_regs;

/* Called early in kmain after console init */
void kernel_arch_init(void);

/* Called from timer ISR (vector 0x20) */
void kernel_timer_tick(struct moos_regs *frame);

#endif /* STITCH_INIT_H */