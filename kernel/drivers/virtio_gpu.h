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
 *  - On a normal `-vga std` boot the frozen presentation must not change:
 *    there is no boot-time call and not a single `[vga2d]` line is printed.
 *    `vga2d on` remains the opt-in demo output.
 *  - The scanout buffer is a static .bss array (no PMM/heap dependency, and
 *    the kernel is identity-mapped at 1 MiB, so virtual == physical for the
 *    device's DMA), capped at 1920x1080x4 = 8 MiB.
 *  - Framebuffer mode (below) is the one case where the backend *is* the
 *    system display: it is only entered when the firmware provided no
 *    linear framebuffer at all (`-vga none -device virtio-gpu-pci`).
 *
 * Two modes share one control queue:
 *   1. demo output  - `vga2d [on|off|status]` (animated colour bands);
 *   2. framebuffer  - claimed automatically at boot when there is no GOP
 *                     framebuffer; fbcon / term / gui draw into it.
 */
#ifndef SUTUREOS_VIRTIO_GPU_H
#define SUTUREOS_VIRTIO_GPU_H

#include <stitch/types.h>

/* Largest scanout the static backing store can hold (1920*1080*4 = 8 MiB). */
#define VIRTIO_GPU_MAX_WIDTH   1920u
#define VIRTIO_GPU_MAX_HEIGHT  1080u

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

/* Periodic service called from the shell idle loop: in demo mode it advances
 * the animated frame; in framebuffer mode it pushes a deferred present.  A
 * no-op when the backend is not active. */
void virtio_gpu_service(void);

/* ===================== system framebuffer mode =====================
 * Used when the firmware gave the kernel no linear framebuffer at all
 * (`-vga none -device virtio-gpu-pci`).  kmain calls virtio_gpu_fb_init();
 * on success it publishes the returned address/geometry as limine_fb_*
 * (the buffer is identity-mapped .bss), so fbcon, the flanterm terminal,
 * the C desktop and the Rust shell draw into the virtio-gpu scanout with
 * no changes to their own code.
 *
 * Presentation is explicit and caller-paced: the renderers write pixels
 * normally, then present() / present_rect() run TRANSFER_TO_HOST_2D +
 * RESOURCE_FLUSH for the rectangle that changed.  Both are cheap no-ops
 * when framebuffer mode is not active, so call sites need no guard.
 */

/* Bring up the device (if needed), create the 2D resource at the
 * device-reported display size (clamped to 1920x1080), attach the static
 * backing store, point scanout 0 at it and clear it to black.
 * Returns 0 when the scanout now holds a pixel buffer the kernel may draw
 * into; -1 when there is no usable virtio-gpu.  Idempotent. */
int virtio_gpu_fb_init(void);

/* 1 when framebuffer mode owns scanout 0. */
int virtio_gpu_fb_active(void);

/* Geometry of the active scanout.  Address is a physical (and, because the
 * kernel runs identity-mapped, virtual) address; pitch is in bytes. */
uint64_t virtio_gpu_fb_addr(void);
uint32_t virtio_gpu_fb_width(void);
uint32_t virtio_gpu_fb_height(void);
uint32_t virtio_gpu_fb_pitch(void);
uint32_t virtio_gpu_fb_bpp(void);

/* Push the whole frame to the host.  Safe from any context: if another
 * thread/IRQ already holds the command lock the call just marks the frame
 * dirty instead of spinning. */
void virtio_gpu_fb_present(void);

/* Push one rectangle; coordinates are clamped to the scanout. */
void virtio_gpu_fb_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Mark the frame dirty without issuing commands (ISR-safe); the next
 * virtio_gpu_service() (shell idle loop / desktop loop) presents it. */
void virtio_gpu_fb_mark_dirty(void);

/* Present a dirty frame, if any.  Cheap no-op otherwise. */
void virtio_gpu_fb_service(void);

#endif /* SUTUREOS_VIRTIO_GPU_H */
