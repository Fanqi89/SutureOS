/* StitchOS module: PS/2 keyboard scancode decoder (mbos_ prefix)
 * Ported from: MandelbrotOS (C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS\src\kernel\kbd.c,
 *              C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS\src\include\kernel\kbd.h)
 * Original license: MPL-2.0 (file-level copyleft; this file remains under MPL-2.0 terms)
 * Changes: keymap + shift state machine extracted into a pure decoder that
 *          takes injected scancodes instead of owning an IRQ; caps-lock case
 *          folding and E0-prefix retention added; irq_install_handler /
 *          gets()/cls() dependencies dropped (re-attach in phase 2 through
 *          moos_irq_register(33, ...)).
 */
#ifndef STITCH_MB_KBD_H
#define STITCH_MB_KBD_H

#include <stitch/types.h>

/* PS/2 controller ports (from MandelbrotOS kbd.h) */
#define MBOS_KBD_DATA     0x60u
#define MBOS_KBD_STAT     0x64u
#define MBOS_KBD_CMD      0x64u
#define MBOS_KBD_STAT_OBF 0x01u     /* output buffer full */

/* modifier bits kept in the decoder state (MandelbrotOS kbd.h) */
#define MBOS_KBD_SHIFT (1u << 0)
#define MBOS_KBD_CTRL  (1u << 1)
#define MBOS_KBD_ALT   (1u << 2)
#define MBOS_KBD_E0ESC (1u << 6)

#define MBOS_KBD_LSHIFT 0x2Au
#define MBOS_KBD_RSHIFT 0x36u
#define MBOS_KBD_CAPS   0x3Au

#define MBOS_KBD_IS_RELEASE(sc) (((sc) & 0x80u) != 0)
#define MBOS_KBD_IS_ESCAPE(sc)  ((sc) == 0xE0u)

/* Reset decoder state (shift/ctrl/alt/e0/caps). */
void mbos_kbd_init(void);

/* Feed one set-1 scancode; returns the ASCII character (0 = no character:
 * break codes, modifiers, caps toggle, unmapped keys). */
int mbos_kbd_decode(uint8_t scancode);

/* Polling read (source kbdhandler used the same two ports): returns the next
 * scancode from 0x60 when 0x64 bit0 says output-buffer-full, else -1.
 * Self test never calls it - it injects synthetic codes into the decoder. */
int mbos_kbd_poll(void);

/* boot-time self test (st_run("mandelbrot_kbd", ...)) */
void st_mbos_kbd_test(void);

#endif /* STITCH_MB_KBD_H */
