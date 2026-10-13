/* SutureOS: Framebuffer Console (UEFI GOP) */
#ifndef STITCH_FBCON_H
#define STITCH_FBCON_H

#include <stitch/types.h>

void fbcon_init(uint64_t phys, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp);
void fbcon_clear(void);
void fbcon_putc(char c);
void fbcon_puts(const char *s);

/* Timer tick callback for cursor blink (called from timer ISR) */
void fbcon_timer_tick(void);

#endif /* STITCH_FBCON_H */