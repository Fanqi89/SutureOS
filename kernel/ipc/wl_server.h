/* SutureOS: minimal Wayland server (wl_display / wl_registry / wl_compositor /
 * wl_surface / wl_shm / wl_shm_pool / wl_buffer / wl_seat / wl_callback).
 *
 * Scope: this is the first *real* Wayland server for SutureOS, deliberately cut
 * down to what a client needs to publish one shared-memory surface:
 *   - objects: wl_display(1), wl_registry, wl_callback, wl_compositor,
 *     wl_surface, wl_shm, wl_shm_pool, wl_buffer, wl_seat, wl_pointer (a real
 *     object since this batch; wl_keyboard is still inert);
 *   - requests: wl_display.get_registry/sync, wl_registry.bind,
 *     wl_compositor.create_surface, wl_shm.create_pool (memfd over SCM_RIGHTS),
 *     wl_shm_pool.create_buffer/destroy, wl_buffer.destroy,
 *     wl_surface.attach/damage/damage_buffer/frame/commit/destroy,
 *     wl_seat.get_pointer/get_keyboard/release, wl_pointer.set_cursor/release,
 *     wl_callback.destroy;
 *   - events: wl_registry.global, wl_callback.done, wl_buffer.release,
 *     wl_seat.capabilities, wl_display.error, wl_display.delete_id,
 *     wl_pointer.enter/motion/button.
 * Wire format is the Wayland one (little endian): u32 object id, u16 opcode,
 * u16 size (size includes the 8-byte header), then the arguments; strings are
 * u32 length including the NUL terminator followed by the bytes padded to 4.
 *
 * On wl_surface.commit the server hashes the buffer's bytes read out of the
 * memfd, logs `[wl] commit ... hash=...` and emits wl_buffer.release. The memfd
 * is *not* released when the client exits: the server keeps the descriptor and a
 * copy of the pixels, so the compositor can blit them later through
 * wl_server_latest_surface().
 *
 * wl_surface.frame(): the requested wl_callback is completed on the *next*
 * commit (wl_callback.done(serial), serial is a per-commit monotonic counter);
 * the object is kept marked-done so the client can still destroy it. Damage
 * rects are accumulated and logged at commit.
 *
 * wl_pointer: a synthetic *snapshot*, not a stream. SutureOS has one task and no
 * async input queue for clients, so wl_pointer.enter + wl_pointer.motion (and
 * wl_pointer.button when a button is held) are emitted once, on the first pump
 * after wl_seat.get_pointer, from the kernel's PS/2 state
 * (mbos_mouse_x/y/buttons) clamped into the committed surface.
 *
 * Still not implemented (documented, not pretended): scaling, subsurface/region
 * objects (create_region makes an inert object), wl_shm formats other than
 * XRGB8888 (1), multiple concurrent clients, wl_keyboard events, wl_pointer
 * frame grouping, and a real wl_display.sync/frame scheduler (the server runs
 * inside the client's own syscalls, so both complete synchronously).
 */
#ifndef SUTUREOS_WL_SERVER_H
#define SUTUREOS_WL_SERVER_H

#include <stitch/types.h>

#define WL_SERVER_MAX_OBJECTS 24
#define WL_SERVER_IN_MAX      2048      /* request reassembly buffer           */
#define WL_SERVER_OUT_MAX     4096      /* queued events (client ring is 2 KiB) */
#define WL_SURFACE_MAX_W      320
#define WL_SURFACE_MAX_H      200
#define WL_SURFACE_MAX_BYTES  (WL_SURFACE_MAX_W * WL_SURFACE_MAX_H * 4)

/* Start the minimal server: bind `name` (lazy, called from the first connect of
 * WL_SERVER_NAME). Returns 0 or -errno. */
int wl_server_autostart(void);

/* A new connection's server end arrived; protocol state for it is reset. */
void wl_server_accept(int server_fd);

/* Run the protocol for one connection: flush queued events, then read and
 * dispatch every complete request. Safe to call with nothing pending. */
void wl_server_pump(int server_fd);

/* Last committed surface: 1 = *w/*h/*stride/*px were filled, 0 = no commit yet.
 * `*px` points at the server's copy of the pixels (XRGB8888) and stays valid
 * after the client has exited. */
int wl_server_latest_surface(int *w, int *h, int *stride, const uint8_t **px);

/* FNV-1a 32 of the last committed surface (0 before the first commit). */
uint32_t wl_server_latest_hash(void);

/* Number of successful commits (for `usbinfo`-style diagnostics). */
uint32_t wl_server_commit_count(void);

/* 1 once the server has started (a name is bound / a client connected). */
int wl_server_active(void);

#endif /* SUTUREOS_WL_SERVER_H */
