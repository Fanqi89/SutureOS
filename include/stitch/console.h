/* StitchOS (缝合怪系统) - early console interface */
#ifndef STITCH_CONSOLE_H
#define STITCH_CONSOLE_H

#include <stitch/types.h>

void console_init(void);              /* bring up COM1 16550 */
void console_putc(char c);
void console_puts(const char *s);
void console_printf(const char *fmt, ...);

#endif /* STITCH_CONSOLE_H */
