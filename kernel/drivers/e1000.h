/* SutureOS: Intel e1000 (8254x) NIC driver - ported from Cinux' well-tested
 * e1000 driver (Cinux/kernel/drivers/net/e1000.cpp, GPL-3.0, same project family).
 *
 * Why this driver exists: SutureOS only spoke RTL8139, and VMware's virtual NIC is
 * an e1000 - so networking was dead on the machine the OS is actually used on. The
 * port keeps Cinux' hard-won findings:
 *
 *  1. POLLED RX NEEDS A MMIO READ. QEMU's e1000 model only moves receive packets
 *     into the descriptor ring when the device-model main loop runs, and that loop
 *     only runs when the guest traps (MMIO access / interrupt / hlt). A pure
 *     memory poll therefore never sees a packet (symptom: GPRC=0, RDH=0 while the
 *     packet is on the wire). e1000_receive() reads the RDH register first, which
 *     forces the trap. (Cinux note: "2026-06-26-f5-m6-e1000-rx-timer-fix.md")
 *  2. RDT = N-1 sentinel, so tail never equals head.
 *  3. RCTL = EN | SECRC | BAM | UPE (strip CRC, accept broadcast + all unicast).
 *  4. TX descriptor needs EOP|IFCS|RS and the ring doorbell is a write to TDT.
 *
 * SutureOS is a flat identity-mapped kernel, so static buffers are already at their
 * physical addresses - no DMA pool is needed (that is the one structural change
 * from Cinux).
 */
#ifndef SUTUREOS_E1000_H
#define SUTUREOS_E1000_H

#include <stitch/types.h>

#define E1000_ERR_NONE        0
#define E1000_ERR_NOT_FOUND  -1
#define E1000_ERR_BAR        -2
#define E1000_ERR_RESET      -3
#define E1000_ERR_NO_BUFFERS -4

/* Probe the PCI bus, reset the card, read the MAC, arm RX/TX. 0 on success. */
int e1000_init(void);

/* Is an e1000 initialised and receiving? */
int e1000_present(void);

/* MAC address learned from the EEPROM. */
int e1000_get_mac(uint8_t mac_out[6]);

/* Physical address of the memory BAR (diagnostics). */
uint64_t e1000_bar0(void);

/* Number of RX descriptors / TX frames for diagnostics. */
uint64_t e1000_rx_frames(void);
uint64_t e1000_tx_frames(void);

/* Send one Ethernet frame (without FCS). Returns 0 or -1. */
int e1000_send_frame(const void *frame, uint32_t len);

/* Drain the RX ring, calling `fn` once per received frame. Returns the number of
 * frames delivered. */
int e1000_receive(void (*fn)(const uint8_t *frame, uint32_t len));

#endif /* SUTUREOS_E1000_H */
