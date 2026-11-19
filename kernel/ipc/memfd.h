/* SutureOS: memfd + MAP_SHARED — the second Wayland prerequisite.
 *
 * A Wayland client allocates its pixel pool with memfd_create(), sizes it with
 * ftruncate(), mmaps it MAP_SHARED and tells the compositor the fd over the
 * socket (SCM_RIGHTS). This module provides the memfd object and the shared
 * mapping; fd passing comes next.
 *
 * Deliberate limits (documented, not pretended):
 *   - memfd fds live in their own encoded number space (>= MEMFD_FD_BASE) instead
 *     of the descriptor table, exactly like pipes/socketpairs;
 *   - up to 4 objects, each up to 64 pages (256 KiB) — enough for a Wayland pool
 *     prototype, far from a general allocator;
 *   - the same object mapped twice at different addresses shares the SAME pages
 *     (that is what MAP_SHARED means here, and what the selftest proves).
 */
#ifndef SUTUREOS_MEMFD_H
#define SUTUREOS_MEMFD_H

#include <stitch/types.h>

#define MEMFD_MAX_OBJECTS 4
#define MEMFD_MAX_PAGES   64
#define MEMFD_FD_BASE     300

/* Create an empty object; returns an encoded fd or -errno. */
int memfd_create_obj(void);

/* 1 when `fd` names a memfd object. */
int memfd_is_fd(int fd);

/* Resize to `bytes` (rounded up to pages); pages are allocated lazily. */
int memfd_resize(int fd, uint64_t bytes);

/* Byte count currently backed, or -errno. */
int memfd_size(int fd);

/* Physical page `idx` of the object, or NULL. */
void *memfd_page(int fd, int idx);

/* Map `len` bytes of the object into the CURRENT address space starting at
 * `vaddr` as user 4 KiB pages (shared: repeated maps share the same frames).
 * Returns 0 or -errno. */
int memfd_map(uint64_t vaddr, int fd, uint64_t len, int writable);

/* Unmap [vaddr, vaddr+len) by clearing the PTEs (4 KiB granularity). */
int memfd_unmap(uint64_t vaddr, uint64_t len);

/* Copy helpers over the object (offset based). */
int memfd_write(int fd, uint64_t off, const void *src, uint64_t len);
int memfd_read(int fd, uint64_t off, void *dst, uint64_t len);

/* Kernel self-test: create/resize/map twice/write-read across both mappings. */
int memfd_selftest(void);

#endif
