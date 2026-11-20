/* SutureOS module: VMware SVGA II (vmsvga) 2D display backend
 *
 * Origin and licensing
 * --------------------
 * SutureOS original, written from the public VMware SVGA II register and
 * command specification and from the observed behaviour of QEMU's
 * `-device vmware-svga` model (read back through the register file itself).
 * No third-party source file was copied.  SutureOS is GPL-3.0; this file is a
 * SutureOS original.  See kernel/drivers/vmsvga.c for the full rationale.
 *
 * Design constraints (why this file is shaped the way it is)
 * ---------------------------------------------------------
 *  - The frozen boot presentation must not change and no boot-time self-test
 *    may be added.  So there is NO boot-time call: nothing here runs unless
 *    the user types `vmsvga on`.  On a normal `-vga std` boot no VMware SVGA
 *    II PCI function exists and not a single `[vmsvga]` line is printed.
 *  - The scan buffer is a static .bss array (no PMM/heap dependency), capped
 *    at 1024x768x4 = 3 MiB.
 *  - This is a *display output*, not a framebuffer replacement: fbcon /
 *    kernel/term / kernel/gui are never touched.
 *
 * Public API used by kernel/shell/shell.c (`vmsvga [on|off|status]`).
 */
#ifndef SUTUREOS_VMSVGA_H
#define SUTUREOS_VMSVGA_H

#include <stitch/types.h>

/* Largest scanout the static backing store can hold (3 MiB of .bss). */
#define VMSVGA_MAX_WIDTH   1024u
#define VMSVGA_MAX_HEIGHT  768u

/* 1 = a VMware SVGA II PCI function (vendor 0x15AD, device 0x0405, class
 * display) is present.  Only PCI config space is read: no BAR is mapped and
 * no register is written. */
int vmsvga_probe(void);

/* Bring-up on first use: PCI probe, BAR0 index/value register pair verified by
 * round-tripping SVGA_REG_ID, scanout mode programmed/read back, BAR1 (VRAM)
 * and BAR2 (FIFO) identity-mapped, FIFO configured (CONFIG_DONE), command
 * channel proven with a SYNC/BUSY round trip, and only then SVGA_REG_ENABLE.
 * Idempotent; returns 0 on success. */
int vmsvga_init(void);

/* `vmsvga on`: init if needed, paint frame 0 and push the full frame through
 * SVGA_CMD_UPDATE, then start the periodic animation. */
int vmsvga_on(void);

/* `vmsvga off`: stop the periodic animation.  The scanout and the command
 * channel stay up (the last frame is still in VRAM); the caller decides what
 * happens on screen next - the shell re-enables the terminal, which repaints
 * its grid over the frame. */
int vmsvga_off(void);

/* Print the live state (PCI address, geometry, FIFO cursors, update count). */
void vmsvga_status(void);

/* Periodic service called from the shell idle loop: no-op unless `vmsvga on`
 * is in effect, and paced by the 100 Hz system tick.  Applies the animation
 * step, then pushes the changed strip with SVGA_CMD_UPDATE. */
void vmsvga_service(void);

#endif /* SUTUREOS_VMSVGA_H */
