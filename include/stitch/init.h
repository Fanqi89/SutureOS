/* SutureOS Phase-2: Kernel hardware initialization API */
#ifndef STITCH_INIT_H
#define STITCH_INIT_H

#include <stitch/types.h>

struct moos_regs;

/* Called early in kmain after console init */
void kernel_arch_init(void);

/* Called from timer ISR (vector 0x20) */
void kernel_timer_tick(void);

/* Limine framebuffer parsing */
void limine_parse_fb(uint64_t mbi);
extern uint64_t limine_fb_addr, limine_fb_width, limine_fb_height,
                limine_fb_pitch, limine_fb_bpp;

#endif /* STITCH_INIT_H */