/* SutureOS module: Realtek RTL8139 fast-ethernet NIC driver
 * Ported from: SpiritFoxOS (C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\
 *              kernel\src\drivers\net\rtl8139.c), GPL-3.0 - same license as
 *              this project. Register map cross-checked against the RTL8139C(L)
 *              datasheet and the register usage in Cinux / CoolPotOS.
 * Changes: C++/own-libc dependencies dropped (no kmalloc, no hal_* wrappers,
 *          no apic_enable_irq); PIO access goes straight through <stitch/io.h>;
 *          DMA buffers are static .bss objects instead of page allocations so
 *          the driver never touches the kernel heap; the chip is driven in
 *          POLLING mode (IMR = 0, ISR is only read) because SutureOS has no
 *          working shared-IRQ path for the RTL8139 line; PCI device discovery
 *          is a self-contained config-space reader (bus 0..255, dev 0..31,
 *          fn 0..7) since the kernel has no generic PCI enumerator.
 *
 * The driver hands whole ethernet frames (14-byte header + payload, no FCS)
 * to a callback while the frame is still in the RX ring, so the stack can
 * parse it in place without a copy of its own.
 */
#ifndef STITCH_RTL8139_H
#define STITCH_RTL8139_H

#include <stitch/types.h>

/* Diagnostic error codes returned by rtl8139_init() (0 == success) */
#define RTL8139_ERR_NONE         0
#define RTL8139_ERR_NOT_FOUND   -1   /* no 10EC:8139 on the PCI bus            */
#define RTL8139_ERR_BAR          2   /* BAR0 is not an I/O-space BAR           */
#define RTL8139_ERR_RESET        3   /* software reset never completed         */
#define RTL8139_ERR_IO_BASE      4   /* I/O base outside the 16-bit port space */
#define RTL8139_ERR_NO_BUFFERS   5   /* DMA buffers unusable                   */
#define RTL8139_ERR_CMD          6   /* Rx/Tx did not come up in ChipCmd       */

/* Bring up the NIC found on the PCI bus (idempotent: a second call is a no-op
 * that returns 0). Fills the MAC and the PCI IRQ line, programs the 8 KiB RX
 * ring + 4 TX descriptors and leaves the chip in polling mode (interrupt mask
 * cleared). Returns 0 or one of RTL8139_ERR_*. */
int rtl8139_init(void);

/* 1 once rtl8139_init() succeeded. */
int rtl8139_present(void);

/* Copy the six IDR0..5 bytes read during bring-up. Returns 0, or -1 when the
 * driver is not initialised. */
int rtl8139_get_mac(uint8_t mac_out[6]);

/* PCI IRQ line recorded from config space offset 0x3F (0xFF -> forced to 11,
 * matching the QEMU i440fx slot assignment). */
uint8_t rtl8139_irq(void);

/* 16-bit I/O BAR decoded from config space (BAR0). */
uint16_t rtl8139_io_base(void);

/* Transmit one whole ethernet frame (14 <= len <= 1514, no FCS). Copies into
 * one of four descriptor buffers, then polls the descriptor's TOK bit. Returns
 * 0 on success, -1 when the driver is down or len is out of range, -2 when all
 * four descriptors stayed busy, -3 when the chip aborted the transfer. */
int rtl8139_send_frame(const void *frame, uint32_t len);

/* Drain every complete frame currently in the RX ring and hand each one to
 * `fn` (called with the 14-byte-header frame and its length). Non-blocking:
 * returns as soon as BUFE (or a short/incoherent ring tail) is seen. Returns
 * the number of frames delivered. */
int rtl8139_receive(void (*fn)(const uint8_t *frame, uint32_t len));

/* Last raw IntrStatus (0x3E) sample taken by rtl8139_receive(). */
uint16_t rtl8139_last_isr(void);

/* Number of frames dropped on RX (bad header, incoherent length, oversized,
 * or a ring wrap that could not be linearised). */
uint32_t rtl8139_rx_drops(void);

/* Frame counters (incremented by the driver itself). */
uint64_t rtl8139_tx_frames(void);
uint64_t rtl8139_rx_frames(void);

/* Boot-time self test (st_run("rtl8139", ...)): PCI presence + register probes.
 * Records a PASS only when a device answering the RTL8139 ID was found; the
 * test is reported as a pass with a "not present" note otherwise so a NIC-less
 * machine does not turn the boot log red. */
void st_rtl8139_test(void);

#endif /* STITCH_RTL8139_H */
