/* SutureOS: console log ring buffer (see log_ring.c). */
#ifndef SUTUREOS_LOG_RING_H
#define SUTUREOS_LOG_RING_H

#include <stitch/types.h>

/* Capture one character of console output (called from console_putc). */
void log_ring_putc(char c);
void log_ring_puts(const char *s);

/* Lines currently held (<= 128) and lines ever completed. */
int      log_ring_count(void);
uint64_t log_ring_total_lines(void);

/* i = 0 is the oldest line still held; NULL when out of range. */
const char *log_ring_get(int i);

/* Print the last `n` lines (n <= 0 or too large = everything held).
 * `emit` NULL = print through the console (which also scrolls the terminal to
 * the newest line); a non-NULL emit receives one line at a time. */
void log_ring_dump_tail(int n, int (*emit)(const char *));

#endif /* SUTUREOS_LOG_RING_H */
