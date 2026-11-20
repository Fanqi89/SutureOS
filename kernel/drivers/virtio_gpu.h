/* SutureOS module: VirtIO-GPU 2D display backend (modern virtio 1.0 PCI)
 *
 * Origin and licensing
 * --------------------
 * Written for SutureOS from the public VirtIO 1.2 specification (OASIS,
 * "Virtual I/O Device (VIRTIO) Version 1.2", sections 2.7 "Virtqueues",
 * 4.1 "Virtio Over PCI Bus" and 5.7 "GPU Device") and by reading QEMU's
 * device model behaviour from the outside (QMP `info pci` / `info qtree`,
 * screendump).  No third-party source file was copied; the split-virtqueue
 * mechanics (descriptor / available / used layout, bounded used-ring polling)
 * follow kernel/drivers/virtio_blk.c, which is the in-tree legacy reference.
 * SutureOS is GPL-3.0; this file is a SutureOS original.
 *
 * Design constraints (why this file is shaped the way it is)
 * ---------------------------------------------------------
 *  - The frozen boot presentation must not change.  There is therefore NO
 *    boot-time call: nothing here runs unless the user types `vga2d on`.
 *    On a normal `-vga std` boot not a single `[vga2d]` line is printed.
 *  - The scanout buffer is a static .bss array (no PMM/heap dependency, and
 *    the kernel is identity-mapped at 1 MiB, so virtual == physical for the
 *    device's DMA), capped at 1024x768x4 = 3 MiB.
 *  - The backend is a *display output*, not a framebuffer replacement: it
 *    never touches fbcon / kernel/term / kernel/gui.
 *
 * Public API used by kernel/shell/shell.c (`vga2d [on|off|status]`).
 */
#ifndef SUTUREOS_VIRTIO_GPU_H
#define SUTUREOS_VIRTIO_GPU_H

#include <stitch/types.h>

/* Largest scanout the static backing store can hold (3 MiB of .bss). */
#define VIRTIO_GPU_MAX_WIDTH   1024u
#define VIRTIO_GPU_MAX_HEIGHT  768u

/* 1 = a virtio-gpu PCI function is present (no I/O is performed beyond
 * config-space reads, no BAR is mapped, no state is changed). */
int virtio_gpu_probe(void);

/* Modern virtio 1.0 bring-up on first use: PCI probe, BAR map, capability
 * parse, reset, VIRTIO_F_VERSION_1, control queue 0, DRIVER_OK, and one
 * GET_DISPLAY_INFO.  Idempotent; returns 0 on success. */
int virtio_gpu_init(void);

/* `vga2d on`: init if needed, create the 2D resource, attach the static
 * backing, point scanout 0 at it, then start the periodic TRANSFER+FLUSH. */
int virtio_gpu_on(void);

/* `vga2d off`: stop the periodic TRANSFER+FLUSH.  The resource and the
 * scanout stay attached so the last frame keeps being displayed. */
int virtio_gpu_off(void);

/* Print the live state (PCI address, display size, resource, frame count). */
void virtio_gpu_status(void);

/* Periodic service called from the shell idle loop: no-op unless `vga2d on`
 * is in effect, and paced by the 100 Hz system tick.  Applies the animation
 * step, then TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. */
void virtio_gpu_service(void);

#endif /* SUTUREOS_VIRTIO_GPU_H */
