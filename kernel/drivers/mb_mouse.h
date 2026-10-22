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

/* Current cursor position in pixels and button mask (bit0 left, bit1 right,
 * bit2 middle). */
int mbos_mouse_x(void);
int mbos_mouse_y(void);
int mbos_mouse_buttons(void);

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

