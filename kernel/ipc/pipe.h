/* SutureOS: pipes - the kernel half of IPC.
 *
 * kernel/ipc/fifo.c already had a generic ring buffer but nothing exposed it, so
 * `pipe()` did not exist. This module gives pipes real file descriptors without
 * disturbing the VFS: pipe fds live in their own number space (>= PIPE_FD_BASE)
 * and the three fd-taking syscalls (read/write/close) check for them first.
 *
 * Semantics match POSIX closely enough to be useful:
 *   - pipe(fds) returns fds[0] = read end, fds[1] = write end;
 *   - read() returns 0 at EOF once every write end is closed;
 *   - write() of more than the free space writes what fits (short write);
 *   - write() to a pipe with no readers fails with -EPIPE;
 *   - reads block for a bounded time when the pipe is empty but writers exist
 *     (single-task kernel: a blocking wait would deadlock the shell, so this
 *     spins with `pause` and then returns -EAGAIN).
 */
#ifndef SUTUREOS_PIPE_H
#define SUTUREOS_PIPE_H

#include <stitch/types.h>

#define PIPE_MAX_PIPES   8
#define PIPE_BUF_SIZE    1024
#define PIPE_FD_BASE     100        /* pipe fds are PIPE_FD_BASE + slot*2 + end */

/* Create a pipe; fds[0] reads, fds[1] writes. Returns 0 or a negative errno. */
int pipe_create(int fds[2]);

/* 1 when `fd` belongs to this module (the fd syscalls must route it here). */
int pipe_is_fd(int fd);

/* Pipe operations on an encoded fd. Return bytes transferred or -errno. */
int pipe_read_fd(int fd, void *buf, uint32_t len);
int pipe_write_fd(int fd, const void *buf, uint32_t len);
int pipe_close_fd(int fd);

/* Take one more reference on an existing pipe end. dup()/dup2() call this so the
 * reader/writer counts stay right: with a duplicated write end the read side
 * must not see EOF just because the original descriptor was closed. */
int pipe_ref_fd(int fd);

/* Number of live pipes (diagnostics / self test). */
uint32_t pipe_count(void);

/* Self test: create/write/read, EOF after close, short write, EPIPE, fd
 * namespace isolation. Prints "pipe selftest: ... PASS/FAIL"; 0 when all pass. */
int pipe_selftest(void);

#endif /* SUTUREOS_PIPE_H */
