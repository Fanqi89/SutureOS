/* SutureOS module: USB 2.0 (EHCI) host controller + USB mass storage (BOT/SCSI)
 *
 * Source: HanOS kernel/device/usb/ehci.c (MIT) - EHCI controller bring-up,
 *         port reset, async QH/qTD chain, port enumeration, Bulk-Only
 *         Transport + READ(10)/WRITE(10)/READ CAPACITY;
 *         Bulk-Only Transport CBW/CSW field layout and SCSI CDB packing were
 *         cross-checked against HBOS src/usb_msc.c (GPL-3.0).
 * Original licenses: MIT (HanOS, HanOS is MIT per docs/licensing-audit.md),
 *                    GPL-3.0 (HBOS). SutureOS itself is GPL-3.0.
 * Changes (SutureOS port):
 *   - PCI: SutureOS mechanism #1 scan for class 0x0C / sub 0x03 / prog-if 0x20
 *     instead of HanOS' pci_devices vector; BAR0 is mapped with
 *     mmu_map_mmio_identity() (the EHCI BAR can sit above 1 GiB);
 *   - timing: the 8254 PIT counter (mb_pit.h) instead of HanOS' HPET, so the
 *     delays work with interrupts masked;
 *   - DMA: one static .bss arena (identity mapped, like ahci.c/nvme.c) instead
 *     of HanOS' pmm_get() pages, plus clflush around DMA on real hardware;
 *   - added SCSI TEST UNIT READY / INQUIRY / SYNCHRONIZE CACHE and a
 *     blk_device (kernel/drivers/block.h) registration on drive 16;
 *   - only high-speed devices are enumerated: a full/low-speed device is handed
 *     back to the companion controller (PORTSC.PO=1), see the limitation note
 *     in usb_ehci.c.
 */
#ifndef STITCH_USB_EHCI_H
#define STITCH_USB_EHCI_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scan PCI for an EHCI controller, bring it up, enumerate the first
 * high-speed mass-storage device and register it as a block device
 * (driver number BLK_DRIVE_USB = 16).
 *
 * Returns 0 when a storage device was registered, -1 otherwise.
 * Logs exactly one "[usb] ..." line when nothing is attached, so a machine
 * without USB keeps the boot log it had before this driver existed. */
int usb_probe(void);

/* 1 = a USB mass-storage block device is registered. */
int usb_present(void);

/* Drive number of the registered device (BLK_DRIVE_USB), -1 when absent. */
int usb_drive(void);

/* Capacity in 512-byte blocks, 0 when absent. */
uint64_t usb_blocks(void);

/* Block size in bytes, 0 when absent. */
uint32_t usb_block_size(void);

/* Boot self-test: raw block read (signature check) + VFS read of USB.TXT +
 * a write/read-back/restore round trip on the last block.
 * Prints "[usb] selftest PASS (read=1 cat=1)" on success.
 * Does nothing when no USB storage is present. */
void usb_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_USB_EHCI_H */
