/* SutureOS: AF_UNIX socketpair implementation. See unix_socket.h. */
#include "unix_socket.h"

#include <stitch/console.h>
#include <stitch/string.h>

#define EINVAL_ 22
#define EPIPE_  32
#define EAGAIN_ 11

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint8_t  buf[UNIX_BUF_SIZE];
} unix_dir_t;

typedef struct {
    int        in_use;
    int        ends;              /* how many fds are still open (0..2) */
    unix_dir_t d[2];              /* d[0]: 0 -> 1, d[1]: 1 -> 0 */
} unix_pair_t;

static unix_pair_t g_pairs[UNIX_MAX_PAIRS];

#define UNIX_ENC(slot, end) ((UNIX_FD_BASE) + (slot) * 2 + (end))

static int unix_decode(int fd, int *slot, int *end)
{
    const int rel = fd - UNIX_FD_BASE;
    if (rel < 0) return -1;
    *slot = rel / 2;
    *end = rel % 2;
    if (*slot < 0 || *slot >= UNIX_MAX_PAIRS) return -1;
    if (*end < 0 || *end > 1) return -1;
    if (!g_pairs[*slot].in_use) return -1;
    return 0;
}

int unix_is_fd(int fd)
{
    int slot, end;
    return unix_decode(fd, &slot, &end) == 0;
}

int unix_socketpair(int fds[2])
{
    int slot;

    if (fds == NULL) return -EINVAL_;
    for (slot = 0; slot < UNIX_MAX_PAIRS; slot++) {
        if (!g_pairs[slot].in_use) break;
    }
    if (slot >= UNIX_MAX_PAIRS) return -EAGAIN_;

    memset(&g_pairs[slot], 0, sizeof(g_pairs[slot]));
    g_pairs[slot].in_use = 1;
    g_pairs[slot].ends = 2;
    fds[0] = UNIX_ENC(slot, 0);
    fds[1] = UNIX_ENC(slot, 1);
    return 0;
}

/* direction a read on `end` consumes from: end 0 reads d[1], end 1 reads d[0] */
static unix_dir_t *read_dir(unix_pair_t *p, int end) { return &p->d[end ? 0 : 1]; }
static unix_dir_t *write_dir(unix_pair_t *p, int end) { return &p->d[end ? 1 : 0]; }

int unix_read_fd(int fd, void *buf, uint32_t len)
{
    int slot, end;
    unix_dir_t *d;
    uint32_t n = 0;

    if (unix_decode(fd, &slot, &end) != 0) return -EINVAL_;
    if (buf == NULL && len) return -EINVAL_;
    d = read_dir(&g_pairs[slot], end);
    while (n < len && d->count > 0) {
        ((uint8_t *)buf)[n++] = d->buf[d->tail];
        d->tail = (d->tail + 1) % UNIX_BUF_SIZE;
        d->count--;
    }
    if (n == 0 && len > 0) {
        /* nothing buffered: EOF only when the peer has closed */
        if (g_pairs[slot].ends <= 1) return 0;
        return -EAGAIN_;
    }
    return (int)n;
}

int unix_write_fd(int fd, const void *buf, uint32_t len)
{
    int slot, end;
    unix_dir_t *d;
    uint32_t n = 0;

    if (unix_decode(fd, &slot, &end) != 0) return -EINVAL_;
    if (buf == NULL && len) return -EINVAL_;
    if (g_pairs[slot].ends <= 1) return -EPIPE_;
    d = write_dir(&g_pairs[slot], end);
    while (n < len && d->count < UNIX_BUF_SIZE) {
        d->buf[d->head] = ((const uint8_t *)buf)[n++];
        d->head = (d->head + 1) % UNIX_BUF_SIZE;
        d->count++;
    }
    if (n == 0) return -EAGAIN_;
    return (int)n;
}

int unix_close_fd(int fd)
{
    int slot, end;

    if (unix_decode(fd, &slot, &end) != 0) return -EINVAL_;
    if (g_pairs[slot].ends > 0) g_pairs[slot].ends--;
    if (g_pairs[slot].ends == 0) {
        memset(&g_pairs[slot], 0, sizeof(g_pairs[slot]));
    }
    return 0;
}

int unix_poll_fd(int fd)
{
    int slot, end;
    int ev = 0;

    if (unix_decode(fd, &slot, &end) != 0) return 0;
    if (read_dir(&g_pairs[slot], end)->count > 0) {
        ev |= UNIX_POLLIN;
    } else if (g_pairs[slot].ends <= 1) {
        ev |= UNIX_POLLIN | UNIX_POLLHUP;   /* readable-EOF */
    }
    if (g_pairs[slot].ends <= 1) {
        ev |= UNIX_POLLERR;
    } else if (write_dir(&g_pairs[slot], end)->count < UNIX_BUF_SIZE) {
        ev |= UNIX_POLLOUT;
    }
    return ev;
}

/* ------------------------------------------------------------------ selftest */

static int g_selftest_ok;
static int g_selftest_fail;

static void unix_check(int cond, const char *what)
{
    if (cond) {
        g_selftest_ok++;
        console_printf("[unix]   ok   %s\n", what);
    } else {
        g_selftest_fail++;
        console_printf("[unix]   FAIL %s\n", what);
    }
}

int unix_selftest(void)
{
    int fds[2];
    int rc;
    uint8_t wbuf[8];
    uint8_t rbuf[8];

    g_selftest_ok = 0;
    g_selftest_fail = 0;
    console_printf("[unix] socketpair selftest\n");

    rc = unix_socketpair(fds);
    unix_check(rc == 0, "socketpair() creates a pair");
    if (rc != 0) {
        console_printf("[unix] selftest: %d ok, %d FAIL (cannot continue)\n",
                       g_selftest_ok, g_selftest_fail);
        return -1;
    }
    unix_check(unix_is_fd(fds[0]) && unix_is_fd(fds[1]),
               "both ends are unix fds");
    unix_check(fds[0] != fds[1], "the two ends differ");

    for (int i = 0; i < 8; i++) wbuf[i] = (uint8_t)(0x40 + i);

    unix_check((unix_poll_fd(fds[1]) & UNIX_POLLOUT) != 0,
               "fresh pair is writable (POLLOUT)");
    unix_check((unix_poll_fd(fds[1]) & UNIX_POLLIN) == 0,
               "fresh pair is not readable (no POLLIN)");

    rc = unix_write_fd(fds[0], wbuf, sizeof wbuf);
    unix_check(rc == (int)sizeof wbuf, "write() sends all 8 bytes");
    unix_check((unix_poll_fd(fds[1]) & UNIX_POLLIN) != 0,
               "peer becomes readable after the write (POLLIN)");

    memset(rbuf, 0, sizeof rbuf);
    rc = unix_read_fd(fds[1], rbuf, sizeof rbuf);
    unix_check(rc == (int)sizeof rbuf, "read() receives all 8 bytes");
    unix_check(memcmp(rbuf, wbuf, sizeof wbuf) == 0, "payload is byte-exact");
    unix_check((unix_poll_fd(fds[1]) & UNIX_POLLIN) == 0,
               "buffer drains back to not-readable");

    /* the other direction must work too (bidirectional pair) */
    rc = unix_write_fd(fds[1], (const uint8_t *)"ok", 2);
    unix_check(rc == 2, "reverse direction write works");
    memset(rbuf, 0, sizeof rbuf);
    rc = unix_read_fd(fds[0], rbuf, 2);
    unix_check(rc == 2 && rbuf[0] == 'o' && rbuf[1] == 'k',
               "reverse direction payload is exact");

    /* EOF after closing the peer end */
    (void)unix_close_fd(fds[0]);
    rc = unix_read_fd(fds[1], rbuf, sizeof rbuf);
    unix_check(rc == 0, "read() returns 0 (EOF) after the peer closes");
    unix_check((unix_poll_fd(fds[1]) & UNIX_POLLHUP) != 0,
               "closed peer reports POLLHUP");
    (void)unix_close_fd(fds[1]);

    console_printf("[unix] socketpair selftest: %d ok, %d FAIL\n",
                   g_selftest_ok, g_selftest_fail);
    return g_selftest_fail == 0 ? 0 : -1;
}
