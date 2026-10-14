/* Host-harness shadow of include/stitch/console.h (see types.h next to it). */
#ifndef STITCH_CONSOLE_H
#define STITCH_CONSOLE_H

#include <stitch/types.h>

void console_init(void);
void console_putc(char c);
void console_puts(const char *s);
void console_printf(const char *fmt, ...);

#endif /* STITCH_CONSOLE_H */
