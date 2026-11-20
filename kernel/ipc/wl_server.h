/* SutureOS: minimal-but-real Wayland server (wl_display / wl_registry /
 * wl_callback / wl_compositor / wl_surface / wl_region / wl_shm / wl_shm_pool /
 * wl_buffer / wl_seat / wl_pointer / wl_keyboard / wl_subcompositor /
 * wl_subsurface).
 *
 * Scope (what is actually implemented, and what is deliberately not):
 *   - objects: wl_display(1), wl_registry, wl_callback, wl_compositor,
 *     wl_surface, wl_region, wl_shm, wl_shm_pool, wl_buffer, wl_seat,
 *     wl_pointer, wl_keyboard, wl_subcompositor, wl_subsurface;
 *   - globals: wl_compositor v4, wl_shm v1, wl_seat v7, wl_subcompositor v1
 *     (the version a client binds at is what gates wl_pointer.frame, which is
 *     a v5 addition);
 *   - requests: wl_display.get_registry/sync, wl_registry.bind,
 *     wl_compositor.create_surface/create_region, wl_region.add/subtract/
 *     destroy, wl_shm.create_pool (memfd over SCM_RIGHTS),
 *     wl_shm_pool.create_buffer/destroy, wl_buffer.destroy,
 *     wl_surface.attach/damage/damage_buffer/frame/set_opaque_region/
 *     set_input_region/set_buffer_transform/set_buffer_scale/offset/commit/
 *     destroy, wl_seat.get_pointer/get_keyboard/release, wl_pointer.set_cursor/
 *     release, wl_keyboard.release, wl_callback.destroy,
 *     wl_subcompositor.get_subsurface/destroy, wl_subsurface.set_position/
 *     set_sync/set_desync/place_above/place_below/destroy;
 *   - events: wl_registry.global, wl_callback.done, wl_buffer.release,
 *     wl_seat.capabilities, wl_display.error, wl_display.delete_id,
 *     wl_pointer.enter/leave/motion/button/frame,
 *     wl_keyboard.keymap/enter/leave/key/modifiers.
 * Wire format is the Wayland one (little endian): u32 object id, u16 opcode,
 * u16 size (size includes the 8-byte header), then the arguments; strings are
 * u32 length including the NUL terminator followed by the bytes padded to 4.
 *
 * Multi-client: every connection keeps its *own* copy of its last committed
 * surface (pixels + geometry + scale + serial), so two clients no longer
 * overwrite each other.  wl_server_latest_surface() keeps its old meaning (the
 * most recent commit by any client, still valid after that client exits); the
 * new wl_server_surface_count()/_at() expose all retained surfaces.
 *
 * wl_keyboard is a real event source: kernel/drivers/mb_kbd.c mirrors every
 * decoded scancode into a small ring, and the server's pump turns those into
 * wl_keyboard.key(serial, time, evdev_keycode, state) with press *and* release.
 * This kernel has no XKB data, so wl_keyboard.keymap goes out with the
 * specification's own "no keymap" format (0) and no descriptor: the client maps
 * the evdev keycodes itself.
 *
 * wl_pointer is a real event stream (enter once, motion whenever the position
 * changes, button on both press and release, leave when the client releases the
 * pointer), sourced from the kernel's PS/2 mouse state.  Coordinates are
 * clamped into the committed surface.
 *
 * wl_region/wl_subsurface are *semantic*, not composited: rectangles, areas and
 * parent/child offsets are recorded, logged and exported, but nothing is clipped
 * or rearranged on screen (SutureOS has no compositor for them yet).
 */
#ifndef SUTUREOS_WL_SERVER_H
#define SUTUREOS_WL_SERVER_H

#include <stitch/types.h>

/* Must match WL_SOCK_MAX_CONNS in kernel/ipc/wl_sock.h: a connection slot here
 * maps 1:1 to a named-endpoint connection there. */
#define WL_SERVER_MAX_CONNS   4

#define WL_SERVER_MAX_OBJECTS 32
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

/* Run the protocol for one connection: flush queued events, read and dispatch
 * every complete request, then pump the keyboard/pointer stream for that
 * connection (only when it bound the matching capability, so a client without a
 * keyboard/pointer never steals input from the shell or GUI). */
void wl_server_pump(int server_fd);

/* Last committed surface (most recent commit by any client): returns 1
 * and fills w, h, stride and px, or 0 when nothing was committed yet.  `*px` points at the server's copy of
 * the pixels (XRGB8888) and stays valid after the client has exited.  This is
 * what the compositor (kernel/gui/wl_view.c) blits. */
int wl_server_latest_surface(int *w, int *h, int *stride, const uint8_t **px);

/* FNV-1a 32 of the last committed surface (0 before the first commit). */
uint32_t wl_server_latest_hash(void);

/* Number of successful commits (all clients). */
uint32_t wl_server_commit_count(void);

/* Number of surfaces currently retained (one per connection that has committed;
 * 0..WL_SERVER_MAX_CONNS). */
int wl_server_surface_count(void);

/* Retained surface `idx` (0..surface_count()-1): geometry, logical scale, commit
 * serial and the server's pixel copy.  Returns 1 when filled, 0 when `idx` is out
 * of range.  Any out pointer may be NULL. */
int wl_server_surface_at(int idx, int *w, int *h, int *stride, int *scale,
                         uint32_t *serial, const uint8_t **px);

/* Live wl_subsurface objects: count and per-index (surface id, parent id,
 * position, sync mode).  Semantic only - nothing is composited. */
int wl_server_subsurface_count(void);
int wl_server_subsurface_at(int idx, int *surface_id, int *parent_id,
                            int *x, int *y, int *sync_mode);

/* Input counters: wl_keyboard.key events and wl_pointer events emitted so far,
 * and events dropped because a client was not reading. */
uint32_t wl_server_key_event_count(void);
uint32_t wl_server_pointer_event_count(void);
uint32_t wl_server_events_dropped(void);

/* 1 once the server has started (a name is bound / a client connected). */
int wl_server_active(void);

#endif /* SUTUREOS_WL_SERVER_H */
