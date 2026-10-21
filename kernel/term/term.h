/* SutureOS: framebuffer terminal (flanterm) adapter API.
 *
 * term.c wraps one vendored flanterm context (kernel/term/flanterm/, BSD-2-Clause)
 * and is the only place the kernel touches flanterm.  All entry points are safe
 * to call before initialisation: they become no-ops (or return 0) until
 * term_init_from_limine() has succeeded.
 */
#ifndef STITCH_TERM_H
#define STITCH_TERM_H

#include <stitch/types.h>

void     term_init_from_limine(void);   /* uses extern uint64_t limine_fb_addr/width/height/pitch/bpp */
int      term_available(void);          /* 1 once initialised successfully */
void     term_putc(char c);
void     term_write(const char *s, uint32_t n);
void     term_puts(const char *s);
void     term_clear(void);
void     term_clear_full(void);   /* clear + repaint every cell (a real wipe) */
void     term_refresh(void);
uint32_t term_cols(void);
uint32_t term_rows(void);
uint32_t term_fg(void);                 /* current default fg (0x00RRGGBB) */
uint32_t term_bg(void);

#endif /* STITCH_TERM_H */
