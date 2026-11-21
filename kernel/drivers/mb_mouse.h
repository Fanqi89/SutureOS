/* SutureOS: PS/2 mouse driver API (see mb_mouse.c). */
#ifndef SUTUREOS_MB_MOUSE_H
#define SUTUREOS_MB_MOUSE_H

#include <stitch/types.h>

/* Bring up the 8042 aux device. Returns 1 when a mouse answered with its ACK,
 * 0 when there is none (the GUI then simply runs without a pointer). */
int mbos_mouse_init(void);
int mbos_mouse_present(void);

/* Drain pending packets; call once per GUI frame (cheap when idle). */
void mbos_mouse_poll(void);

/* Hand one raw aux-port byte to the packet decoder. Used by the keyboard
 * poller: the 8042 output buffer is shared, so whoever drains it must forward
 * mouse bytes here instead of leaving them to block the keyboard. */
void mbos_mouse_feed_byte(uint8_t b);

/* Current cursor position in pixels and button mask (bit0 left, bit1 right,
 * bit2 middle). */
int mbos_mouse_x(void);
int mbos_mouse_y(void);
int mbos_mouse_buttons(void);

/* v0.65: pop the accumulated wheel delta (positive = scroll up / away from
 * the user), 0 when there is none or the device has no wheel. One notch is
 * +-1 on most IntelliMouse devices; the value is clamped to +-64. */
int mbos_mouse_take_wheel(void);

/* Diagnostics: auxiliary device id (0 = 3-byte, 3 = wheel, 4 = 5-button, -1
 * unknown), the packet size the decoder uses, and how many wheel motions were
 * decoded since boot. */
int mbos_mouse_device_id(void);

/* v0.65: feed synthetic 4-byte IntelliMouse packets through the real decoder
 * (z=+1, z=-1, accumulation). Returns 0 on PASS. */
int mbos_mouse_wheel_selftest(void);

/* v0.65: one queued button transition (a press or a release), 0 when the queue
 * is empty. Replaying these is what makes press+release bursts visible to the
 * compositor instead of being sampled away. */
int mbos_mouse_take_button(int *mask);
int mbos_mouse_packet_size(void);
uint32_t mbos_mouse_wheel_events(void);

/* Pop one edge-triggered click. Returns 1 and fills x/y/button (0=left,
 * 1=right, 2=middle), or 0 when the queue is empty. */
int mbos_mouse_take_click(int *x, int *y, int *button);

/* Diagnostics. */
uint32_t mbos_mouse_packets(void);
uint32_t mbos_mouse_obf_seen(void);
uint32_t mbos_mouse_aux_seen(void);
uint32_t mbos_mouse_dropped(void);

/* Teleport the pointer (used to centre it when the GUI starts). */
void mbos_mouse_move_to(int x, int y);

#endif /* SUTUREOS_MB_MOUSE_H */

