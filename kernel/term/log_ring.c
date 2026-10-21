/* SutureOS: console log ring buffer.
 *
 * Every character written through console_putc() is also collected here, so the
 * boot log and runtime output can be reviewed later (`log` / `dmesg` in the
 * shell) even though the terminal itself only shows the last screenful.
 *
 * The buffer keeps the most recent LOG_RING_LINES completed lines verbatim
 * (up to LOG_RING_COLS characters each, longer lines are truncated with "..."),
 * which is all a "show me the log" command needs and costs ~20 KiB of .bss.
 *
 * Auto-scroll: printing the log goes through the normal console path, so the
 * terminal follows the newest line automatically - the tail of the log is what
 * stays visible, which is the behaviour a console log should have.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

#include "log_ring.h"

#define LOG_RING_LINES 128
#define LOG_RING_COLS  168

static char     g_lines[LOG_RING_LINES][LOG_RING_COLS];
static uint32_t g_head;          /* next slot to write                     */
static uint32_t g_count;         /* lines currently held (<= LOG_RING_LINES)*/
static uint64_t g_total;         /* lines ever completed                    */
static char     g_partial[LOG_RING_COLS];
static uint32_t g_partial_len;

/* Set while the ring is being printed back: the printed text reaches
 * console_putc() again and must not be re-captured (that would duplicate every
 * dumped line and eventually push the real log out of the ring). */
static int g_suspended;

static void log_ring_store_line(const char *line, uint32_t len)
{
    char *dst = g_lines[g_head];

    if (len >= LOG_RING_COLS) {
        /* keep the beginning, mark the cut */
        memcpy(dst, line, LOG_RING_COLS - 4);
        dst[LOG_RING_COLS - 4] = '.';
        dst[LOG_RING_COLS - 3] = '.';
        dst[LOG_RING_COLS - 2] = '.';
        dst[LOG_RING_COLS - 1] = '\0';
    } else {
        memcpy(dst, line, len);
        dst[len] = '\0';
    }

    g_head = (g_head + 1u) % LOG_RING_LINES;
    if (g_count < LOG_RING_LINES) {
        g_count++;
    }
    g_total++;
}

void log_ring_putc(char c)
{
    if (g_suspended) {
        return;
    }
    if (c == '\n') {
        log_ring_store_line(g_partial, g_partial_len);
        g_partial_len = 0;
        return;
    }
    if (g_partial_len + 1u < LOG_RING_COLS) {
        g_partial[g_partial_len++] = c;
    }
}

void log_ring_puts(const char *s)
{
    while (s != NULL && *s != '\0') {
        log_ring_putc(*s++);
    }
}

uint64_t log_ring_total_lines(void)
{
    return g_total;
}

int log_ring_count(void)
{
    return (int)g_count;
}

const char *log_ring_get(int i)
{
    if (i < 0 || (uint32_t)i >= g_count) {
        return NULL;
    }
    /* i = 0 is the oldest line still held */
    uint32_t first = (g_head + LOG_RING_LINES - g_count) % LOG_RING_LINES;
    return g_lines[(first + (uint32_t)i) % LOG_RING_LINES];
}

void log_ring_dump_tail(int n, int (*emit)(const char *))
{
    int count = (int)g_count;
    int start;

    if (n <= 0 || n > count) {
        n = count;
    }
    start = count - n;

    g_suspended = 1;
    for (int i = start; i < count; i++) {
        const char *line = log_ring_get(i);
        if (line == NULL) {
            continue;
        }
        if (emit != NULL) {
            (void)emit(line);
        } else {
            console_puts(line);
            console_putc('\n');
        }
    }
    g_suspended = 0;
}
