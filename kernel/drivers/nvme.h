/* SutureOS module: NVMe block driver - public interface
 *
 * SutureOS is GPL-3.0. This driver is a fresh freestanding C11 adaptation; the
 * controller register map, queue layout and command layout were cross-checked
 * against the following upstream NVMe drivers (read-only references, never
 * modified):
 *   1. CoolPotOS  module/nvme/nvme.c           (GPL-3.0) - full admin/IO queue
 *      bring-up, PRP handling, CQE status decoding.
 *   2. VimtuOS    kernel/nvme64.cpp            (GPL-3.0) - PCI 01/08/02 scan,
 *      CAP/DSTRD doorbell stride, phase-bit polling, AQA/ASQ/ACQ + CC layout.
 *   3. Cinux      kernel/drivers/nvme/nvme.cpp (MIT)     - BAR0 handling
 *      (including the firmware-leaves-BAR0-unassigned case), CC field shifts
 *      (IOSQES=16/IOCQES=20), Create CQ/SQ CDW10/CDW11 layout.
 * Paths are relative to C:\Users\fanqi\Desktop\自研操作系统\.
 *
 * Scope of this driver: one controller, one admin queue pair, one I/O queue
 * pair, NSID 1 only, polled completion (no MSI-X, no interrupts).
 */
#ifndef STITCH_NVME_H
#define STITCH_NVME_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* blk_nvme_init()/blk_nvme_probe() are declared in block.h as well (the block
 * layer is the primary consumer). They are repeated here so a caller that only
 * wants the NVMe driver can include this header on its own. */

/* Bring the controller up (reset, admin + I/O queue pairs, Identify Controller,
 * Identify Namespace NSID 1) and register the namespace as blk_device "nvme0"
 * on drive 6 with type BLK_TYPE_NVME. Idempotent: the second call is a no-op.
 * Returns 0 on success, -1 if there is no controller / no usable namespace. */
int blk_nvme_init(void);

/* Detection only: runs exactly the same bring-up but never registers anything.
 * Idempotent. Returns the number of namespaces found (0 = none). */
int blk_nvme_probe(void);

/* Read-only introspection (all report 0 until a successful probe/init):
 *   blk_nvme_present()    1 once the controller is enabled and NSID 1 parsed
 *   blk_nvme_namespace_count()
 *   blk_nvme_mmio_base()  BAR0 physical == virtual address we mapped
 *   blk_nvme_version()    NVMe VS register
 *   blk_nvme_max_queue_entries()  CAP.MQES (0-based maximum entries)          */
int      blk_nvme_present(void);
int      blk_nvme_namespace_count(void);
uint64_t blk_nvme_mmio_base(void);
uint32_t blk_nvme_version(void);
uint32_t blk_nvme_max_queue_entries(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_NVME_H */
