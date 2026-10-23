/* SutureOS: pipe implementation. See pipe.h for the semantics and limits. */
#include "pipe.h"

#include <stitch/console.h>
#include <stitch/string.h>

#define EINVAL_ 22
#define EPIPE_  32
#define EAGAIN_ 11

typedef struct pipe {
    int      in_use;
    int      readers;
    int      writers;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint8_t  buf[PIPE_BUF_SIZE];
} pipe_t;

static pipe_t g_pipes[PIPE_MAX_PIPES];

/* Encode/decode: slot in the high part, 0 = read end, 1 = write end. */
#define PIPE_ENC(slot, end) (PIPE_FD_BASE + (slot) * 2 + (end))
#define PIPE_DECODE(fd, slot, end)                       \
    do {                                                 \
        const int rel = (fd) - PIPE_FD_BASE;             \
        (slot) = rel / 2;                                \
        (end)  = rel % 2;                                \
    } while (0)

int pipe_is_fd(int fd)
{
    if (fd < PIPE_FD_BASE) return 0;
    {
        int slot, end;
        PIPE_DECODE(fd, slot, end);
        if (end < 0 || end > 1) return 0;
        if (slot < 0 || slot >= PIPE_MAX_PIPES) return 0;
        return g_pipes[slot].in_use;
    }
}

int pipe_create(int fds[2])
{
    int slot;

    if (fds == NULL) return -EINVAL_;
    for (slot = 0; slot < PIPE_MAX_PIPES; slot++) {
        if (!g_pipes[slot].in_use) break;
    }
    if (slot >= PIPE_MAX_PIPES) return -EAGAIN_;

    memset(&g_pipes[slot], 0, sizeof(g_pipes[slot]));
    g_pipes[slot].in_use = 1;
    g_pipes[slot].readers = 1;
    g_pipes[slot].writers = 1;
    fds[0] = PIPE_ENC(slot, 0);
    fds[1] = PIPE_ENC(slot, 1);
    return 0;
}

uint32_t pipe_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (g_pipes[i].in_use) n++;
    }
    return n;
}

int pipe_read_fd(int fd, void *buf, uint32_t len)
{
    int slot, end;
    pipe_t *p;
    uint32_t got = 0;
    int spins = 0;

    if (!pipe_is_fd(fd)) return -EINVAL_;
    PIPE_DECODE(fd, slot, end);
    if (end != 0) return -EINVAL_;
    p = &g_pipes[slot];
    if (buf == NULL || len == 0) return 0;

    /* Bounded wait for data: the writer may be another task. Never sleeps
     * forever - the shell is the only reader today. */
    while (p->count == 0) {
        if (p->writers == 0) return 0;          /* EOF */
        if (++spins > 200000) return -EAGAIN_;
        __asm__ volatile("pause");
    }

    while (got < len && p->count > 0) {
        ((uint8_t *)buf)[got++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    return (int)got;
}

int pipe_write_fd(int fd, const void *buf, uint32_t len)
{
    int slot, end;
    pipe_t *p;
    uint32_t put = 0;

    if (!pipe_is_fd(fd)) return -EINVAL_;
    PIPE_DECODE(fd, slot, end);
    if (end != 1) return -EINVAL_;
    p = &g_pipes[slot];
    if (p->readers == 0) return -EPIPE_;
    if (buf == NULL) return -EINVAL_;
    if (len == 0) return 0;

    while (put < len && p->count < PIPE_BUF_SIZE) {
        p->buf[p->head] = ((const uint8_t *)buf)[put++];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
    }
    return (int)put;                            /* short write when full */
}

int pipe_ref_fd(int fd)
{
    int slot, end;
    pipe_t *p;

    if (!pipe_is_fd(fd)) return -EINVAL_;
    PIPE_DECODE(fd, slot, end);
    p = &g_pipes[slot];
    if (end == 0) {
        p->readers++;
    } else {
        p->writers++;
    }
    return 0;
}

int pipe_close_fd(int fd)
{
    int slot, end;
    pipe_t *p;

    if (!pipe_is_fd(fd)) return -EINVAL_;
    PIPE_DECODE(fd, slot, end);
    p = &g_pipes[slot];
    if (end == 0) {
        if (p->readers > 0) p->readers--;
    } else {
        if (p->writers > 0) p->writers--;
    }
    if (p->readers == 0 && p->writers == 0) {
        memset(p, 0, sizeof(*p));
    }
    return 0;
}

/* ---------------- self test ---------------- */

static uint32_t g_checks;
static uint32_t g_failed;

static void st_report(const char *what, int ok)
{
    g_checks++;
    if (!ok) g_failed++;
    console_printf("pipe selftest: %s ... %s\n", what, ok ? "PASS" : "FAIL");
}

int pipe_selftest(void)
{
    int fds[2];
    char out[64];

    g_checks = 0;
    g_failed = 0;

    memset(&g_pipes, 0, sizeof(g_pipes));

    st_report("pipe() creates two fds", pipe_create(fds) == 0);
    st_report("read end is not a VFS fd", !pipe_is_fd(1) && !pipe_is_fd(42));
    st_report("both ends are pipe fds", pipe_is_fd(fds[0]) && pipe_is_fd(fds[1]));

    /* write then read */
    st_report("write 5 bytes", pipe_write_fd(fds[1], "hello", 5) == 5);
    memset(out, 0, sizeof(out));
    st_report("read 5 bytes back", pipe_read_fd(fds[0], out, 5) == 5);
    st_report("payload intact", strcmp(out, "hello") == 0);

    /* empty pipe + writers open => EAGAIN (bounded wait) */
    st_report("empty pipe returns -EAGAIN", pipe_read_fd(fds[0], out, 4) < 0);

    /* short write: fill the buffer, the rest is a short write */
    {
        char big[PIPE_BUF_SIZE + 64];
        memset(big, 'A', sizeof(big));
        {
            const int w = pipe_write_fd(fds[1], big, sizeof(big));
            st_report("short write when the pipe is full",
                      w == PIPE_BUF_SIZE);
        }
        /* drain everything */
        {
            uint32_t total = 0;
            for (;;) {
                const int r = pipe_read_fd(fds[0], out, sizeof(out));
                if (r <= 0) break;
                total += (uint32_t)r;
            }
            st_report("drained the whole buffer", total == PIPE_BUF_SIZE);
        }
    }

    /* EOF: close the write end, then read returns 0 */
    st_report("close write end", pipe_close_fd(fds[1]) == 0);
    st_report("read on EOF returns 0", pipe_read_fd(fds[0], out, 4) == 0);

    /* EPIPE: no readers left */
    {
        int fds2[2];
        st_report("second pipe created", pipe_create(fds2) == 0);
        st_report("close read end", pipe_close_fd(fds2[0]) == 0);
        st_report("write with no readers -> -EPIPE",
                  pipe_write_fd(fds2[1], "x", 1) == -EPIPE_);
        (void)pipe_close_fd(fds2[1]);
    }

    st_report("close read end frees the pipe", pipe_close_fd(fds[0]) == 0);
    st_report("no pipes left", pipe_count() == 0);

    console_printf("pipe selftest: RESULT %s (%u/%u)\n",
                   (g_failed == 0) ? "PASS" : "FAIL",
                   g_checks - g_failed, g_checks);
    return (g_failed == 0) ? 0 : 1;
}
