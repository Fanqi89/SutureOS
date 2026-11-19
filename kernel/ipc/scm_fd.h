/* SutureOS: SCM_RIGHTS helpers — the third Wayland prerequisite.
 *
 * A Wayland client hands its wl_shm pool to the compositor by sending the memfd
 * over the AF_UNIX socket inside a SCM_RIGHTS control message. This file owns the
 * small per-direction queue that carries those descriptors next to the byte
 * stream, plus the selftest that proves sendmsg/recvmsg end-to-end.
 *
 * Simplification, stated rather than hidden: SutureOS' IPC fds (pipe, unix,
 * memfd) live in encoded number spaces, so "passing" one is copying that number;
 * the receiver therefore sees the SAME number, and there is no per-fd refcount.
 * VFS file descriptors cannot be passed yet (sendmsg returns -EINVAL for them).
 */
#ifndef SUTUREOS_SCM_FD_H
#define SUTUREOS_SCM_FD_H

#include <stitch/types.h>

#define UNIX_CTL_MAX 8       /* at most 8 descriptors per direction */
#define SCM_SOL_SOCKET_ 1
#define SCM_RIGHTS_     1

/* Queue `n` descriptors on the direction a write on `fd` feeds. 0 or -errno. */
int unix_send_fds(int fd, const int *fds, int n);

/* Dequeue up to `max` descriptors from the direction a read on `fd` consumes.
 * Returns the count (0 when nothing is queued) or -errno. */
int unix_recv_fds(int fd, int *out, int max);

/* 1 when `fd` can be handed over SCM_RIGHTS (an encoded IPC fd). */
int unix_fd_passeable(int fd);

/* Self-test: memfd -> sendmsg(SCM_RIGHTS) -> recvmsg -> payload readable. */
int scmfd_selftest(void);

#endif
