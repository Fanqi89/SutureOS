/* SutureOS module: VirtIO-blk driver (legacy / virtio 0.9.5 transitional PCI)
 * Ported from (read-only references, never modified):
 *   - C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\drivers\virtio\virtio.cpp
 *       (GPL-3.0) status handshake, feature negotiation, 64-bit device config
 *       reads and the "queue register programming" shape.
 *   - C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\drivers\virtio\virtio_blk.cpp
 *       (GPL-3.0) the 3-descriptor request chain (header / data / status),
 *       VIRTIO_BLK_T_{IN,OUT,FLUSH} and the status-byte completion check.
 *   - C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\mini\driver\pci.hpp
 *       (MIT) PCI config-space mechanism #1 dword access.
 *   - C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\kernel\virtio_gpu64.cpp
 *       (GPL-3.0) split-virtqueue desc/avail/used layout, bounded used-ring
 *       polling and the "never accept a feature you cannot implement" rule.
 * SutureOS itself is GPL-3.0; this C adaptation keeps those licenses' terms.
 *
 * WHAT THIS IS
 *   A real, freestanding C11 driver for the LEGACY (virtio 0.9.5) virtio-blk
 *   PCI transport: PCI BAR0 is an I/O-port register window, one virtqueue is
 *   programmed by writing its page frame number (PFN), requests are submitted
 *   through a split virtqueue and completed by polling the used ring.  QEMU's
 *   `-device virtio-blk-pci` is a transitional device and exposes exactly this
 *   legacy interface as long as the driver only touches BAR0 (the modern
 *   capability list / BAR4 window is deliberately never used).
 *
 * LEGACY REGISTER WINDOW (BAR0, byte offsets; <stitch/io.h> in/out only)
 *   0x00 r32  host features          (read BEFORE writing guest features)
 *   0x04 w32  guest features         (we accept only what we implement)
 *   0x08 w32  queue address          (PFN = vq_phys >> 12, page aligned)
 *   0x0C r16  queue size             (read-only in legacy, power of two)
 *   0x0E w16  queue select           (0 = the single blk request queue)
 *   0x10 w16  queue notify           (write the queue index to kick it)
 *   0x12 r/w8 device status          (ACK 1 | DRIVER 2 | DRIVER_OK 4 |
 *                                     FEATURES_OK 8 | FAILED 128)
 *   0x13 r8   ISR                    (read-to-clear; we only ack with it)
 *   0x14 r64 device config           (offset 0: capacity in 512-byte sectors,
 *                                     little-endian, read as two dwords)
 *
 * LEGACY VIRTQUEUE LAYOUT (all inside one contiguous, 4096-aligned buffer)
 *   desc[queue_size]     16 bytes each      @ +0
 *   avail                16-bit flags, 16-bit idx, 16-bit ring[queue_size]
 *                                            @ +16*queue_size
 *   used                 16-bit flags, 16-bit idx, {u32 id,u32 len}[] entries
 *                                            @ ALIGN(avail_end, 4096)
 *   The used ring offset follows QEMU's vring_align() computation
 *   (ALIGN(avail_off + 4 + 2*n, 4096)).  vring_init() in Linux additionally
 *   reserves the 2-byte avail `used_event` field, but for a power-of-two queue
 *   size <= 256 both formulas land on the same page boundary (the alignment
 *   step absorbs the 2 bytes), so no ambiguity is left to chance here.
 *
 * I/O MODEL
 *   Polling, single outstanding request, serialised by an internal spinlock
 *   (SutureOS is SMP).  Every request is a 3-descriptor chain:
 *     desc0 -> 16-byte header {type, reserved, sector} (device reads)
 *     desc1 -> one bounce page, <= 4096 bytes (direction depends on type)
 *     desc2 -> 1 status byte (device writes)
 *   Transfers are chunked to at most 8 sectors (4096 bytes) per request so a
 *   data descriptor never crosses one page; there is no descriptor for the
 *   caller's buffer, so no caller memory is ever handed to the device.
 */
#ifndef STITCH_VIRTIO_BLK_H
#define STITCH_VIRTIO_BLK_H

#include <stitch/types.h>

/* ---- PCI identity (legacy id first, modern transitional accepted as fallback) */
#define VIRTIO_BLK_PCI_VENDOR      0x1AF4u
#define VIRTIO_BLK_PCI_DEV_LEGACY  0x1001u   /* 0x1000 + (virtio blk id 2 - 1)  */
#define VIRTIO_BLK_PCI_DEV_MODERN  0x1042u   /* 0x1040 + virtio blk id 2       */

/* ---- drive number --------------------------------------------------------
 * SutureOS block drive map (see block.h): ata0 = 0, ata1 = 1, AHCI/SATA = 4..7,
 * NVMe = 8, ram0 = 9 (BLK_DRIVE_RAM). virtio-blk claims 10 ("vda").
 * If another driver also registers the same drive number, the block layer's
 * blk_find_by_drive() returns whichever device was registered last, so the wiring
 * must keep these ranges disjoint (block.c cannot detect the collision itself). */
#define VIRTIO_BLK_DRIVE           10

#define VIRTIO_BLK_SECTOR_SIZE     512u
#define VIRTIO_BLK_QUEUE_MAX       256u      /* largest queue we can lay out      */
#define VIRTIO_BLK_MAX_SECTORS     (1ULL << 32) /* capacity cap (2 TiB)           */

/* ---- entry points (also declared in kernel/drivers/block.h) -------------- */

/* Probe + configure the device, then register it as struct blk_device "vda".
 * Idempotent: returns 0 immediately when already up.
 * Returns 0 on success; -1 when no virtio-blk is on the PCI bus (after logging
 * exactly `[virtio-blk] no virtio-blk device on PCI`) or when bring-up fails. */
int blk_virtio_init(void);

/* Detection only, no side effects and no registration.  Returns 1 when a
 * 1AF4:1001 (or 1AF4:1042 transitional) device is present, 0 otherwise.
 * Idempotent and safe to call before blk_virtio_init(). */
int blk_virtio_probe(void);

/* ---- diagnostics (1 = driver up and serving requests) -------------------- */
int      blk_virtio_present(void);
uint16_t blk_virtio_io_base(void);          /* decoded BAR0 I/O base            */
uint16_t blk_virtio_queue_size(void);       /* queue size read from 0x0C        */
uint32_t blk_virtio_features(void);         /* host features read from 0x00     */
uint32_t blk_virtio_guest_features(void);   /* bits we actually accepted        */
uint64_t blk_virtio_capacity_sectors(void); /* device config offset 0           */
uint64_t blk_virtio_requests(void);         /* completed requests (status == OK)*/
uint64_t blk_virtio_errors(void);           /* failed / unsupported requests    */

#endif /* STITCH_VIRTIO_BLK_H */
