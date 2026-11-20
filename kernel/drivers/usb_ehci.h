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
 *   - added the thin usb_ehci_* hooks at the bottom of this header: HID is a
 *     separate class driver (kernel/drivers/usb_hid.c) that reuses this
 *     driver's port reset, address allocator, control transfers and async qTD
 *     machinery instead of duplicating the transport.
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

/* ===================== hooks for the HID class driver =====================
 * kernel/drivers/usb_hid.c does the HID descriptor walk, the boot-report
 * decoding and the hot-plug policy, but owns no transport code: everything
 * below is a thin wrapper over the port/qTD machinery in usb_ehci.c. They are
 * only used by usb_hid.c - the storage path above never calls them.
 *
 * PORTSC bits the HID scanner needs (same values as the private EHCI_PORTSC_*
 * macros inside usb_ehci.c: CCS bit0, CSC bit1, PED bit2). */
#define USB_EHCI_PORTSC_CCS (1u << 0)
#define USB_EHCI_PORTSC_CSC (1u << 1)
#define USB_EHCI_PORTSC_PED (1u << 2)

/* 1 when the controller is up (BAR mapped, operational base known). */
int usb_ehci_ready(void);

/* Number of root ports (0 when the controller is not up). */
uint32_t usb_ehci_ports(void);

/* Raw PORTSC of a 1-based root port (0 when out of range). */
uint32_t usb_ehci_portsc(uint32_t port);

/* Acknowledge the RW1C change bits (CSC/PEDC/OCC) of a port. */
void usb_ehci_portsc_ack(uint32_t port);

/* Reset a root port (PR held ~50 ms, cleared explicitly, then wait for PED).
 * Returns 0 when the port came up enabled = a high-speed device answered. */
int usb_ehci_port_reset(uint32_t port);

/* Hand a port back to the companion controller (PORTSC.PO = 1). */
void usb_ehci_port_release(uint32_t port);

/* Next USB device address (1..8) from the shared allocator. */
int usb_ehci_alloc_address(void);

/* Control transfer to an arbitrary device address. Returns the number of bytes
 * received for an IN transfer (short packets measured via the qTD residual), 0
 * for OUT/no-data transfers, -1 on failure. */
int usb_ehci_ctl(uint8_t addr, uint16_t mps, const uint8_t *setup, uint32_t len,
                 int in, const void *out);

/* Buffer usb_ehci_ctl() fills for IN transfers (16 KiB). */
const uint8_t *usb_ehci_in_data(void);

/* One interrupt-IN transfer. Returns bytes received (>0), 0 for a zero-length
 * packet, -2 when the device NAKed (no report pending, not an error) and -1 on
 * a real transfer error. `ms` bounds the poll. */
int usb_ehci_intr_in(uint8_t addr, uint16_t mps, uint8_t ep, uint32_t *toggle,
                     void *buf, uint32_t len, uint32_t ms);

/* Root port the mass-storage device was enumerated on (0 when none). The HID
 * scanner must never reset that port. */
int usb_ehci_storage_port(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_USB_EHCI_H */
