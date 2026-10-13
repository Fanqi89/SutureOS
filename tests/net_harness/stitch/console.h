/* Test-only shadow of <stitch/console.h>: the harness prints straight to stdout
 * so every "[net] ..." line the kernel would emit is visible in the test log. */
#ifndef TEST_STITCH_CONSOLE_H
#define TEST_STITCH_CONSOLE_H

#include <stitch/types.h>
#include <stdio.h>

void console_init(void);
void console_putc(char c);
void console_puts(const char *s);
void console_printf(const char *fmt, ...);

#endif /* TEST_STITCH_CONSOLE_H */
