/* SutureOS: SCM_RIGHTS queue + memfd-over-socket selftest. See scm_fd.h. */
#include "scm_fd.h"
#include "unix_socket.h"
#include "memfd.h"

#include <stitch/console.h>
#include <stitch/string.h>

#define EINVAL_ 22
#define EAGAIN_ 11

/* Direction index for the queue: a write on end E feeds queue (E ? 1 : 0) and a
 * read on end E drains queue (E ? 0 : 1) — same convention as unix_socket.c. */
static int g_ctl[2][UNIX_CTL_MAX];
static int g_ctl_count[2];

static int unix_end_of(int fd)
{
    const int rel = fd - UNIX_FD_BASE;
    if (rel < 0 || rel >= UNIX_MAX_PAIRS * 2) return -1;
    return rel % 2;
}

int unix_fd_passeable(int fd)
{
    return unix_is_fd(fd) || memfd_is_fd(fd);
}

int unix_send_fds(int fd, const int *fds, int n)
{
    const int end = unix_end_of(fd);
    int dir, i;

    if (end < 0 || fds == NULL || n <= 0) return -EINVAL_;
    dir = end ? 1 : 0;
    if (g_ctl_count[dir] + n > UNIX_CTL_MAX) return -EAGAIN_;
    for (i = 0; i < n; i++) {
        if (!unix_fd_passeable(fds[i])) return -EINVAL_;   /* VFS fds unsupported */
        g_ctl[dir][g_ctl_count[dir]++] = fds[i];
    }
    return 0;
}

int unix_recv_fds(int fd, int *out, int max)
{
    const int end = unix_end_of(fd);
    int dir, n = 0;

    if (end < 0 || out == NULL || max <= 0) return -EINVAL_;
    dir = end ? 0 : 1;
    while (n < max && g_ctl_count[dir] > 0) {
        out[n++] = g_ctl[dir][0];
        for (int i = 1; i < g_ctl_count[dir]; i++) {
            g_ctl[dir][i - 1] = g_ctl[dir][i];
        }
        g_ctl_count[dir]--;
    }
    return n;
}

/* ------------------------------------------------------------------ selftest */

static int g_ok, g_fail;

static void scm_check(int cond, const char *what)
{
    if (cond) { g_ok++; console_printf("[fdpass]   ok   %s\n", what); }
    else      { g_fail++; console_printf("[fdpass]   FAIL %s\n", what); }
}

int scmfd_selftest(void)
{
    int pair[2];
    int pool;
    int rx[4];
    uint8_t payload[6];
    uint8_t got[6];
    uint8_t probe[4];

    g_ok = g_fail = 0;
    console_printf("[fdpass] SCM_RIGHTS selftest (memfd over socketpair)\n");

    pair[0] = pair[1] = -1;
    pool = memfd_create_obj();
    scm_check(pool >= MEMFD_FD_BASE, "create the memfd pool to pass");
    scm_check(memfd_resize(pool, 4096) == 0, "size the pool to one page");
    if (pool < 0 || memfd_size(pool) != 4096) {
        console_printf("[fdpass] selftest: %d ok, %d FAIL\n", g_ok, g_fail);
        return -1;
    }
    memcpy(got, "shm-rt", 6);
    scm_check(memfd_write(pool, 0, got, 6) == 6, "seed the pool with a payload");
    scm_check(unix_socketpair(pair) == 0, "create the AF_UNIX socketpair");

    /* the sender queues the memfd next to a short data message */
    memcpy(got, "hello", 5);
    scm_check(unix_write_fd(pair[0], got, 5) == 5, "send 5 bytes of data");
    scm_check(unix_send_fds(pair[0], &pool, 1) == 0, "queue the memfd as SCM_RIGHTS");
    scm_check(unix_fd_passeable(pool), "the memfd is a passable descriptor");
    scm_check(!unix_fd_passeable(0), "a console fd is not passable (honest refusal)");

    /* the receiver drains data and the descriptor */
    memset(got, 0, sizeof got);
    scm_check(unix_read_fd(pair[1], got, 5) == 5, "receive the 5 data bytes");
    scm_check(memcmp(got, "hello", 5) == 0, "the data payload is byte-exact");
    rx[0] = rx[1] = rx[2] = rx[3] = -1;
    {
        const int n = unix_recv_fds(pair[1], rx, 4);
        scm_check(n == 1, "recvmsg delivers exactly one descriptor");
        scm_check(rx[0] == pool, "the received descriptor names the same pool");
        scm_check(memfd_is_fd(rx[0]), "the received descriptor is usable as a memfd");
        scm_check(memfd_size(rx[0]) == 4096, "the received pool still reports one page");
    }

    /* payload readable through the received descriptor proves it is the same object */
    memset(probe, 0, sizeof probe);
    scm_check(memfd_read(rx[0], 0, probe, 6) == 6, "read the pool through the received fd");
    scm_check(memcmp(probe, "shm-rt", 6) == 0, "the payload behind the passed fd is exact");

    /* queue is drained: a second recvmsg must come back empty */
    scm_check(unix_recv_fds(pair[1], rx, 4) == 0, "the queue is empty after recvmsg");

    (void)unix_close_fd(pair[0]);
    (void)unix_close_fd(pair[1]);

    console_printf("[fdpass] selftest: %d ok, %d FAIL\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : -1;
}
