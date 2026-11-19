/* SutureOS: AF_UNIX socketpair — the first Wayland prerequisite.
 *
 * Wayland needs a bidirectional local socket pair between the compositor and each
 * client, plus SCM_RIGHTS fd passing and poll(). This module delivers the socket
 * pair half (and poll readiness) without touching the network stack:
 *
 *   - `unix_socketpair(fds)` creates two connected endpoints (a 2x2 ring buffer
 *     pair), fds[0] and fds[1] being the two ends;
 *   - fds live in their own number space (>= UNIX_FD_BASE) exactly like pipes, so
 *     the ordinary read/write/close syscalls route here without a VFS fd;
 *   - writes are short-write friendly, reads return 0 at EOF and -EAGAIN when the
 *     peer is open but silent (single-task kernel: no blocking wait);
 *   - `unix_poll_fd()` reports POLLIN/POLLOUT readiness for poll(2).
 *
 * Not implemented yet: SOCK_SEQPACKET semantics, ancillary data (SCM_RIGHTS),
 * credentials (SO_PEERCRED), and pathname binding (AF_UNIX bind/listen/accept).
 * Those are the next Wayland prerequisites and are listed in the roadmap.
 */
#ifndef SUTUREOS_UNIX_SOCKET_H
#define SUTUREOS_UNIX_SOCKET_H

#include <stitch/types.h>

#define UNIX_MAX_PAIRS   4
#define UNIX_BUF_SIZE    2048
#define UNIX_FD_BASE     200      /* fds are UNIX_FD_BASE + slot*2 + end */

/* poll(2) event bits (the Linux values). */
#define UNIX_POLLIN      0x001
#define UNIX_POLLOUT     0x004
#define UNIX_POLLERR     0x008
#define UNIX_POLLHUP     0x010

/* Create a connected pair; fds[0]/fds[1] are the two ends. 0 or -errno. */
int unix_socketpair(int fds[2]);

/* 1 when `fd` belongs to this module. */
int unix_is_fd(int fd);

/* Bytes transferred, 0 at EOF, or -errno. */
int unix_read_fd(int fd, void *buf, uint32_t len);
int unix_write_fd(int fd, const void *buf, uint32_t len);

/* Close one end. 0 or -errno. */
int unix_close_fd(int fd);

/* Ready events for poll(2): a combination of UNIX_POLL*. */
int unix_poll_fd(int fd);

/* Kernel self-test: create/verify/poll/close a pair, log PASS/FAIL lines.
 * Returns 0 when every check passed. */
int unix_selftest(void);

#endif
