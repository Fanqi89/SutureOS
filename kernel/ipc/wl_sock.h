/* SutureOS: AF_UNIX named endpoints — the socket half of the minimal Wayland
 * server (kernel/ipc/wl_server.c owns the protocol).
 *
 * Why this exists: kernel/ipc/unix_socket.c gives anonymous socketpairs. A
 * Wayland client finds its compositor through a *name* ("wayland-0"), so
 * something has to turn a name into a connected pair. This module keeps that
 * name table and creates the pair; it deliberately contains no protocol code.
 *
 * Where it sits (all of it already existed, none of it changed):
 *   - kernel/ipc/unix_socket.c  the byte-stream pair and its fd number space;
 *   - kernel/ipc/scm_fd.c       SCM_RIGHTS: the client hands its wl_shm pool
 *                               memfd over the same socket;
 *   - kernel/ipc/memfd.c        the pool object the compositor reads pixels from.
 *
 * Driving model: SutureOS has one task and no event loop for this, so the
 * server is pumped from the client's own syscalls — wl_sock_pump_fd() is called
 * from the read/write path in kernel/syscall/syscalls.c for any descriptor that
 * is the client end of a Wayland connection (cheap table look-up otherwise).
 * That makes a Wayland round trip synchronous: by the time write() returns, the
 * request has been dispatched and the reply is already queued on the socket.
 *
 * Lazy init: nothing here runs until the first connect(), so kmain is untouched
 * (see wl_server_autostart()).
 *
 * Known limitation: one active connection at a time is what a compositor with a
 * single client needs; a second connect() replaces the active connection but the
 * earlier protocol objects stay allocated until the pool of connections recycles.
 */
#ifndef SUTUREOS_WL_SOCK_H
#define SUTUREOS_WL_SOCK_H

#include <stitch/types.h>

#define WL_SOCK_NAME_MAX   32
#define WL_SOCK_MAX_NAMES  4
#define WL_SOCK_MAX_CONNS  4

/* The name a SutureOS compositor listens on (matches the Wayland convention). */
#define WL_SERVER_NAME     "wayland-0"

/* Register `name` on the AF_UNIX endpoint `fd` (the server side of a pair).
 * Returns 0, -EINVAL (not a unix fd / bad name) or -EADDRINUSE (name taken by a
 * live endpoint; a stale binding whose fd was closed is recycled). */
int wl_named_bind(int fd, const char *name);

/* Connect to `name` and return the *client* end as an encoded unix fd (usable
 * with read/write/close, exactly like a socketpair end). When `name` is
 * WL_SERVER_NAME and nothing is bound yet, the minimal server autostarts and
 * binds it (lazy init). Returns the fd or -ENOENT / -EAGAIN / -EINVAL. */
int wl_named_connect(const char *name);

/* 1 when `fd` is the client end of a Wayland connection. */
int wl_sock_is_client_fd(int fd);

/* Server end of the connection owning `client_fd`, or -1. */
int wl_sock_server_fd(int client_fd);

/* Drive the protocol for the connection owning `fd`: flush queued events, then
 * read and dispatch every complete request. No-op for any other descriptor. */
void wl_sock_pump_fd(int fd);

/* Number of live connections (0..WL_SOCK_MAX_CONNS). */
int wl_sock_conn_count(void);

/* Name bound at `idx` (NULL when free) and its endpoint fd — diagnostics. */
const char *wl_sock_name_at(int idx, int *fd);

/* Drop every connection/name (test hook; also used by the self-test). */
void wl_sock_reset(void);

#endif /* SUTUREOS_WL_SOCK_H */
