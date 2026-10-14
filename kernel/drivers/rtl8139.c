/* SutureOS module: Realtek RTL8139 fast-ethernet NIC driver (POLLING mode)
 * Ported from: SpiritFoxOS (C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\
 *              kernel\src\drivers\net\rtl8139.c), GPL-3.0 - same license as
 *              this project. Register map cross-checked against the RTL8139C(L)
 *              datasheet, Linux 8139too.c conventions (RCR/TSD bit layout) and
 *              the network drivers of Cinux / CoolPotOS.
 *
 * What changed relative to the source (see rtl8139.h for the summary):
 *   - no kmalloc/hal_*: DMA memory is four static .bss objects, so the driver
 *     is independent of the kernel heap (which the boot path re-initialises
 *     after the module self-tests) and needs no page allocator;
 *   - no apic_enable_irq(): SutureOS runs the legacy 8259 PIC, IDT gates exist
 *     only for vectors 0-47 + 0x80, and IRQ11 is shared with nothing that
 *     demultiplexes it. The chip therefore runs with IMR = 0 and the kernel
 *     drains the ring from net_poll() instead. Reading ISR still works with
 *     the mask cleared, so the status register is used for diagnostics only;
 *   - the rx path hands the frame to a callback while it is still in the ring
 *     (the source copied only non-wrapping frames and silently dropped any
 *     frame that wrapped), and CAPR is advanced after the callback so the chip
 *     cannot overwrite the frame under the parser;
 *   - clflush is applied to RX (after the chip wrote) and TX (before the chip
 *     reads) so the driver is correct on a real 8139 as well, not only under
 *     QEMU's coherent DMA model.
 */
#include "rtl8139.h"

#include <stitch/io.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>

/* ===================== PCI identification ===================== */
#define RTL8139_VENDOR_ID 0x10ECu
#define RTL8139_DEVICE_ID 0x8139u

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu
#define PCI_CFG_VENDOR_ID  0x00u
#define PCI_CFG_COMMAND    0x04u
#define PCI_CFG_BAR0       0x10u
#define PCI_CFG_INTR_LINE  0x3Cu

/* PCI COMMAND bits: I/O space enable + bus master enable. Without the bus
 * master bit the 8139 cannot DMA a single byte. */
#define PCI_CMD_IO_SPACE   (1u << 0)
#define PCI_CMD_BUS_MASTER (1u << 2)

/* ===================== RTL8139 register offsets (BAR0) ===================== */
#define RTL_IDR0      0x00u   /* MAC bytes 0..5 (also IDR0..5)                */
#define RTL_MAR0      0x08u   /* multicast filter (8 bytes)                   */
#define RTL_TSD0      0x10u   /* 4 x transmit status descriptor               */
#define RTL_TSAD0     0x20u   /* 4 x transmit start address descriptor        */
#define RTL_RBSTART   0x30u   /* receive ring start address (physical)        */
#define RTL_CMD       0x37u   /* chip command (8 bit)                         */
#define RTL_CAPR      0x38u   /* current address of packet read (16 bit)      */
#define RTL_CBR       0x3Au   /* current buffer address (16 bit, read only)   */
#define RTL_IMR       0x3Cu   /* interrupt mask (16 bit)                      */
#define RTL_ISR       0x3Eu   /* interrupt status (16 bit)                    */
#define RTL_TCR       0x40u   /* transmit config (32 bit)                     */
#define RTL_RCR       0x44u   /* receive config (32 bit)                      */
#define RTL_CONFIG1   0x52u   /* config register 1 (8 bit)                    */

/* ChipCmd (0x37) bits */
#define RTL_CMD_BUFE  0x01u   /* rx buffer empty (read only)                  */
#define RTL_CMD_TXEN  0x04u   /* transmitter enable                           */
#define RTL_CMD_RXEN  0x08u   /* receiver enable                              */
#define RTL_CMD_RST   0x10u   /* software reset (self-clearing)               */

/* IntrStatus (0x3E) bits */
#define RTL_ISR_ROK   (1u << 0)
#define RTL_ISR_RER   (1u << 1)
#define RTL_ISR_TOK   (1u << 2)
#define RTL_ISR_TER   (1u << 3)
#define RTL_ISR_RXOVW (1u << 4)
#define RTL_ISR_FOVW  (1u << 6)
#define RTL_ISR_LEN   (1u << 13)
#define RTL_ISR_TIMEOUT (1u << 14)
#define RTL_ISR_SERR  (1u << 15)

/* RxConfig: accept broadcast + multicast + physical match + all-multicast,
 * 1 KiB DMA burst, unlimited max DMA burst, WRAP so the chip never writes a
 * frame across the 8 KiB ring boundary, no RX FIFO threshold, 8 KiB ring.
 * Bit layout (datasheet 6.3): [2:0] early threshold, 3 WRAP, 4-5 reserved,
 * 6-7 accept bits, 8-10 max DMA burst, 11-12 reserved, 13-15 rx FIFO thresh. */
#define RTL_RCR_VALUE 0x0000000Fu
/* TxConfig 6.5: IFG 0x3 (standard 96 bit time) at 24-25, max DMA burst 0x7
 * (2048 bytes) at 8-10, loopback 0 (normal). */
#define RTL_TCR_VALUE 0x03000700u

#define RTL_RX_RING_SIZE   8192u
#define RTL_RX_RING_PAD    16u      /* CAPR starts at 0xFFF0 == -16        */
#define RTL_RX_RING_EXTRA  1536u    /* slack so a max frame never overruns */
#define RTL_RX_RING_TOTAL  (RTL_RX_RING_SIZE + RTL_RX_RING_PAD + RTL_RX_RING_EXTRA)

#define RTL_TX_DESCRIPTORS 4u
#define RTL_TX_BUF_SIZE    2048u
#define RTL_TX_MAX_FRAME   1514u
#define RTL_TX_MIN_FRAME   60u    /* ETH_ZLEN: the chip aborts shorter frames */

/* TSD bits 0-12 hold the length; bit 13 LS, 14 TUN, 15 TOK, 16-30 size,
 * 18 TABT (transmit abort). The 8139 has no OWN bit: writing the size starts
 * the transfer and the chip raises TOK when the frame is on the wire. */
#define RTL_TSD_SIZE_MASK 0x00001FFFu
#define RTL_TSD_HOST_OWNS 0x00002000u  /* TSD.OWN: 1 = descriptor owned by the HOST */
#define RTL_TSD_TUN       (1u << 14)   /* transmit underrun                        */
#define RTL_TSD_TOK       (1u << 15)   /* transmit OK (transfer completed)         */
#define RTL_TSD_TABT      (1u << 30)   /* transmit aborted (was wrongly 1<<18)     */

/* Poll guards: every hardware handshake is bounded by a spin counter so a
 * broken chip can never wedge the kernel (QEMU completes each one in a few
 * hundred iterations). */
#define RTL_RESET_SPINS   1000000
#define RTL_TX_SPINS      2000000
/* Short settle after a valid RX header is seen, so the payload DMA (which may
 * land after the header) is complete before the stack parses the frame. */
#define RTL_RX_SETTLE_SPINS 256

/* ===================== DMA-visible buffers (.bss) =====================
 * Everything the chip touches lives here: physical == virtual (the kernel
 * identity-maps the low 1 GiB) and no allocator is involved, so a later
 * PMM/heap re-init cannot move the ring out from under the NIC. The ring is
 * over-aligned because IDR programming and RBSTART require 16-byte alignment
 * (QEMU enforces it). */
static uint8_t g_rx_ring[RTL_RX_RING_TOTAL] __attribute__((aligned(4096)));
static uint8_t g_tx_buf[RTL_TX_DESCRIPTORS][RTL_TX_BUF_SIZE] __attribute__((aligned(256)));
/* Scratch for the rare frame that wraps the 8 KiB ring boundary. */
static uint8_t g_wrap_buf[RTL_TX_MAX_FRAME] __attribute__((aligned(16)));

/* ===================== driver state ===================== */
static int      g_rtl_state;                 /* 0 = down, 1 = up            */
static uint16_t g_io_base;
static uint8_t  g_irq;
static uint8_t  g_mac[6];
static uint16_t g_last_isr;
static uint32_t g_rx_drops;
static uint32_t g_rx_wraps;                  /* frames linearised at the ring end */
static uint32_t g_tx_drops;
static uint32_t g_tx_next;                   /* round-robin descriptor      */
static uint32_t g_rx_ring_head;              /* our own CAPR-16 offset      */
static uint64_t g_frames_rx;
static uint64_t g_frames_tx;

/* ---- register accessors ---- */
static inline uint8_t  rtl_inb(uint16_t off) { return inb((uint16_t)(g_io_base + off)); }
static inline uint16_t rtl_inw(uint16_t off) { return inw((uint16_t)(g_io_base + off)); }
static inline void     rtl_outb(uint16_t off, uint8_t v)  { outb((uint16_t)(g_io_base + off), v); }
static inline void     rtl_outw(uint16_t off, uint16_t v) { outw((uint16_t)(g_io_base + off), v); }

/* Dword accessors. These previously synthesised a 32-bit access from two
 * 16-bit halves, which does NOT work: QEMU's 8139 model implements the
 * transmit doorbell (TxStatus0) and the descriptor addresses (TxAddr0) only in
 * its 32-bit handlers, so a pair of outw() calls was silently discarded and the
 * chip never started a single transfer (symptom: frames tx=0, drops=N, ping
 * always timed out). outl()/inl() now live in stitch/io.h. */
static inline void rtl_outl(uint16_t off, uint32_t v)
{
    outl((uint16_t)(g_io_base + off), v);
}

static inline uint32_t rtl_inl(uint16_t off)
{
    return inl((uint16_t)(g_io_base + off));
}

/* ===================== PCI config space ===================== */
/* Mechanism #1 needs DWORD accesses: writing the address register 0xCF8
 * byte-by-byte does not stick on QEMU (every slot then reads back the same
 * constant, which is how this driver first failed to find a NIC that `info pci`
 * clearly showed on bus 0). stitch/io.h has no outl/inl, so they live here. */
static inline void pci_outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)
                  | (uint32_t)(off & 0xFCu);

    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, v);
}

/* Scan bus 0..255 / device 0..31 / function 0..7 for 10EC:8139. Returns 0 and
 * fills bus_out/dev_out/fn_out when found. */
static int pci_find_rtl8139(uint8_t *bus_out, uint8_t *dev_out, uint8_t *fn_out)
{
    uint32_t bus, dev, fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t id = pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0, PCI_CFG_VENDOR_ID);
            uint32_t header_type;
            uint32_t fn_max;

            if ((id & 0xFFFFu) == 0xFFFFu)         /* no device in this slot   */
                continue;
            if ((id & 0xFFFFu) == RTL8139_VENDOR_ID &&
                ((id >> 16) & 0xFFFFu) == RTL8139_DEVICE_ID) {
                *bus_out = (uint8_t)bus;
                *dev_out = (uint8_t)dev;
                *fn_out  = 0;
                return 0;
            }
            /* Multi-function devices hide function 0's siblings behind bit 7
             * of the header type. Only scan the extra functions when set. */
            header_type = pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0Cu);
            fn_max = (header_type & 0x00800000u) ? 8u : 1u;
            for (fn = 1; fn < fn_max; fn++) {
                id = pci_cfg_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, PCI_CFG_VENDOR_ID);
                if ((id & 0xFFFFu) == RTL8139_VENDOR_ID &&
                    ((id >> 16) & 0xFFFFu) == RTL8139_DEVICE_ID) {
                    *bus_out = (uint8_t)bus;
                    *dev_out = (uint8_t)dev;
                    *fn_out  = (uint8_t)fn;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ===================== cache maintenance ===================== */
/* The 8139 DMAs into/out of ordinary cached memory. QEMU's device model is
 * cache-coherent with the CPU, but on real silicon the ring buffers must be
 * written back before the chip reads them (TX) and invalidated after the chip
 * wrote them (RX). clflush is a no-op for correctness on QEMU and mandatory
 * on hardware; both call sites below are cheap (<= 2 KiB). */
static void rtl_clflush(const void *addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)addr & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)addr + len;

    while (p < end) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
        p += 64;
    }
}

/* ===================== public accessors ===================== */
int rtl8139_present(void)        { return g_rtl_state ? 1 : 0; }
uint8_t rtl8139_irq(void)        { return g_irq; }
uint16_t rtl8139_io_base(void)   { return g_io_base; }
uint16_t rtl8139_last_isr(void)  { return g_last_isr; }
uint32_t rtl8139_rx_drops(void)  { return g_rx_drops; }
uint32_t rtl8139_rx_wraps(void)  { return g_rx_wraps; }
uint64_t rtl8139_tx_frames(void) { return g_frames_tx; }
uint64_t rtl8139_rx_frames(void) { return g_frames_rx; }

int rtl8139_get_mac(uint8_t mac_out[6])
{
    if (!g_rtl_state || !mac_out)
        return -1;
    memcpy(mac_out, g_mac, 6);
    return 0;
}

/* ===================== bring-up ===================== */
int rtl8139_init(void)
{
    uint8_t bus = 0, dev = 0, fn = 0;
    uint32_t bar0, cmd, spins, i;

    if (g_rtl_state)
        return 0;                               /* idempotent */

    if (pci_find_rtl8139(&bus, &dev, &fn) != 0)
        return RTL8139_ERR_NOT_FOUND;

    /* Enable I/O space decoding + bus mastering (the 8139 needs both). */
    cmd = pci_cfg_read32(bus, dev, fn, PCI_CFG_COMMAND);
    pci_cfg_write32(bus, dev, fn, PCI_CFG_COMMAND, cmd | PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);

    /* BAR0 must be an I/O-space BAR (bit 0 set); the 8139 maps its 256-byte
     * register file there and the QEMU device may also expose an MMIO BAR1
     * that we deliberately do not use (port I/O needs no MMU mapping). */
    bar0 = pci_cfg_read32(bus, dev, fn, PCI_CFG_BAR0);
    if (!(bar0 & 0x1u))
        return RTL8139_ERR_BAR;
    if ((bar0 & ~0x3u) > 0xFF00u)               /* 16-bit port space limit */
        return RTL8139_ERR_IO_BASE;
    g_io_base = (uint16_t)(bar0 & ~0x3u);

    /* Interrupt line: informational in polling mode, but read it because the
     * log line is part of the acceptance criteria and it documents which PCI
     * slot the guest sees. QEMU's i440fx assigns IRQ 11 to the first PCI slot;
     * a missing/0xFF line is reported as 11 with a note in net.c. */
    g_irq = (uint8_t)(pci_cfg_read32(bus, dev, fn, PCI_CFG_INTR_LINE) & 0xFFu);
    if (g_irq == 0u || g_irq == 0xFFu)
        g_irq = 11u;

    /* Power on: clear LWACT/LWPTN in CONFIG1 before touching the chip. */
    rtl_outb(RTL_CONFIG1, 0x00u);

    /* Soft reset; RST self-clears when the chip is ready. */
    rtl_outb(RTL_CMD, RTL_CMD_RST);
    spins = RTL_RESET_SPINS;
    while ((rtl_inb(RTL_CMD) & RTL_CMD_RST) != 0u) {
        if (--spins == 0u)
            return RTL8139_ERR_RESET;
        io_wait();
    }

    /* MAC address: six bytes at 0x00 (IDR0..IDR5). QEMU seeds these from the
     * -device rtl8139,mac=... property, defaulting to 52:54:00:12:34:56. */
    for (i = 0; i < 6u; i++)
        g_mac[i] = rtl_inb((uint16_t)(RTL_IDR0 + i));

    /* Interrupts off: this driver is polled, so mask every source. ISR stays
     * readable and the status bits are still latched. */
    rtl_outw(RTL_IMR, 0x0000u);
    rtl_outw(RTL_ISR, 0xFFFFu);                 /* clear anything pending */

    /* RX ring: zero it (BUFE reads as set on an empty ring) and program
     * RBSTART with the physical address. The kernel identity-maps the low
     * 1 GiB, so a .bss address is a valid bus address; the ring lives well
     * below 4 GiB, so the 32-bit register is enough. */
    memset(g_rx_ring, 0, sizeof(g_rx_ring));
    if (((uintptr_t)g_rx_ring) > 0xFFFFFFFFull || ((uintptr_t)g_tx_buf) > 0xFFFFFFFFull)
        return RTL8139_ERR_NO_BUFFERS;
    rtl_outl(RTL_RBSTART, (uint32_t)(uintptr_t)g_rx_ring);

    /* Receiver config must also come before the receiver is enabled. */
    rtl_outl(RTL_RCR, RTL_RCR_VALUE);
    rtl_outl(RTL_TCR, RTL_TCR_VALUE);

    /* CAPR: the chip's initial read pointer is 0xFFF0, i.e. 16 bytes before
     * the ring, meaning the first packet starts at offset 0. Writing it
     * explicitly makes the state deterministic on every reset path. */
    g_rx_ring_head = 0;
    rtl_outw(RTL_CAPR, 0xFFF0u);

    /* Drain any stale transmit status. */
    for (i = 0; i < RTL_TX_DESCRIPTORS; i++) {
        rtl_outl((uint16_t)(RTL_TSD0 + 4u * i), 0x00000000u);
        rtl_outl((uint16_t)(RTL_TSAD0 + 4u * i), 0x00000000u);
    }
    g_tx_next = 0;

    /* Enable receiver + transmitter. */
    rtl_outb(RTL_CMD, RTL_CMD_RXEN | RTL_CMD_TXEN);
    if ((rtl_inb(RTL_CMD) & (RTL_CMD_RXEN | RTL_CMD_TXEN)) !=
        (RTL_CMD_RXEN | RTL_CMD_TXEN))
        return RTL8139_ERR_CMD;

    /* Make sure no stale ISR bit survives into the first poll. */
    g_last_isr = rtl_inw(RTL_ISR);
    rtl_outw(RTL_ISR, g_last_isr);

    g_rtl_state = 1;
    return RTL8139_ERR_NONE;
}

/* ===================== transmit ===================== */
int rtl8139_send_frame(const void *frame, uint32_t len)
{
    uint32_t tsd;
    uint32_t spins;
    uint32_t slot;
    uint32_t idx = RTL_TX_DESCRIPTORS;          /* invalid until a slot is found */

    if (!g_rtl_state || !frame || len < 14u || len > RTL_TX_MAX_FRAME) {
        /* SutureOS fix: this early return used to be silent, so a rejected frame
         * (state down / bad length) produced the same bare -1 as an ARP or
         * descriptor failure and the network never said why. */
        if (g_tx_drops == 0u)
            console_printf("[rtl8139] tx rejected: state=%d len=%u (want 14..%u)\n",
                           (int64_t)g_rtl_state, (uint64_t)len,
                           (uint64_t)RTL_TX_MAX_FRAME);
        g_tx_drops++;
        return -1;
    }

    /* Round-robin over the four descriptors; take the first one whose previous
     * transfer completed (TOK), was never used (size == 0) or was aborted. */
    for (slot = 0; slot < RTL_TX_DESCRIPTORS; slot++) {
        uint32_t cand = (g_tx_next + slot) % RTL_TX_DESCRIPTORS;
        tsd = rtl_inl((uint16_t)(RTL_TSD0 + 4u * cand));
        if ((tsd & RTL_TSD_TOK) != 0u || (tsd & RTL_TSD_SIZE_MASK) == 0u) {
            idx = cand;
            break;
        }
        /* A descriptor the chip aborted is reusable as well. */
        if ((tsd & RTL_TSD_TABT) != 0u) {
            g_tx_drops++;
            idx = cand;
            break;
        }
    }
    if (idx == RTL_TX_DESCRIPTORS) {
        /* Every descriptor still has a non-zero size and no TOK: the chip never
         * completed a previous transfer. Report it once with the raw TSDs. */
        if (g_tx_drops == 0u)
            console_printf("[rtl8139] tx: all %u descriptors busy "
                           "(tsd0=0x%x tsd1=0x%x tsd2=0x%x tsd3=0x%x cr=0x%x)\n",
                           (uint64_t)RTL_TX_DESCRIPTORS,
                           (uint64_t)rtl_inl(RTL_TSD0),
                           (uint64_t)rtl_inl((uint16_t)(RTL_TSD0 + 4u)),
                           (uint64_t)rtl_inl((uint16_t)(RTL_TSD0 + 8u)),
                           (uint64_t)rtl_inl((uint16_t)(RTL_TSD0 + 12u)),
                           (uint64_t)rtl_inb(RTL_CMD));
        g_tx_drops++;
        return -2;                               /* all descriptors busy */
    }
    g_tx_next = (idx + 1u) % RTL_TX_DESCRIPTORS;

    /* Copy the frame into the descriptor buffer, flush it out of the cache
     * (real-hardware requirement), arm TSAD then TSD to start the DMA.
     *
     * Minimum-frame padding: the RTL8139 does NOT pad short frames itself and
     * aborts the transmission (TSD.TABT) for anything below the Ethernet
     * minimum, so an ARP request (42 bytes) was dropped and ICMP could never
     * follow - the symptom was rx=0 tx=0 drops=N and a ping that always timed
     * out. 8139too pads in software for exactly this reason; pad to ETH_ZLEN
     * (60 bytes) and let the controller append the FCS. */
    uint32_t txlen = len;
    if (txlen < RTL_TX_MIN_FRAME) {
        txlen = RTL_TX_MIN_FRAME;
        memset(g_tx_buf[idx], 0, txlen);
    }
    memcpy(g_tx_buf[idx], frame, len);
    rtl_clflush(g_tx_buf[idx], txlen);
    rtl_outl((uint16_t)(RTL_TSAD0 + 4u * idx), (uint32_t)(uintptr_t)g_tx_buf[idx]);
    /* Arm the descriptor with the LENGTH ONLY, i.e. TSD.OWN = 0 ("the chip owns
     * it now"): that is what starts the transfer. Writing the extra early-TX
     * threshold bits (0x3000, as some drivers do) sets OWN, which QEMU's model
     * treats as "still owned by the host" and refuses to transmit - the frame
     * then sits in the descriptor forever. The threshold bits are optional. */
    rtl_outl((uint16_t)(RTL_TSD0 + 4u * idx), txlen);

    /* Wait for the descriptor to complete (bounded). QEMU finishes a short
     * frame in a few hundred iterations; a real link takes ~150 us, which is
     * well inside the guard. */
    spins = RTL_TX_SPINS;
    for (;;) {
        tsd = rtl_inl((uint16_t)(RTL_TSD0 + 4u * idx));
        if ((tsd & RTL_TSD_TOK) != 0u)
            break;
        if ((tsd & (RTL_TSD_TABT | RTL_TSD_TUN)) != 0u) {
            g_tx_drops++;
            /* Diagnostics for the first abort: the raw TSD bits say whether the
             * chip aborted (TABT/TUN) and the CR/TCR values show whether the
             * transmitter was actually enabled. */
            if (g_tx_drops == 1u) {
                console_printf("[rtl8139] tx abort: tsd=0x%x len=%u cr=0x%x tcr=0x%x isr=0x%x\n",
                               (uint64_t)tsd, (uint64_t)txlen,
                               (uint64_t)rtl_inb(RTL_CMD),
                               (uint64_t)rtl_inl(RTL_TCR),
                               (uint64_t)rtl_inw(RTL_ISR));
            }
            return -3;                           /* chip aborted the transfer */
        }
        if (--spins == 0u) {
            if (g_tx_drops == 0u) {
                console_printf("[rtl8139] tx never completed: tsd=0x%x len=%u cr=0x%x tcr=0x%x\n",
                               (uint64_t)tsd, (uint64_t)txlen,
                               (uint64_t)rtl_inb(RTL_CMD),
                               (uint64_t)rtl_inl(RTL_TCR));
                g_tx_drops++;                /* only report the first one */
            }
            return -2;                           /* never completed           */
        }
        io_wait();
    }
    g_frames_tx++;
    return 0;
}

/* ===================== receive (polling) ===================== */
/* Diagnostics for the RX drop paths. The counters alone cannot tell a desynced
 * ring from an honest RER: print the first few events with the full state. */
static uint32_t g_rx_dbg;
static void rtl_rx_dbg(uint32_t reason, uint32_t off, uint32_t cbr, uint32_t avail,
                       uint32_t header)
{
    if (g_rx_dbg >= 4u)
        return;
    g_rx_dbg++;
    console_printf("[rtl8139] rx drop #%u reason=%u off=0x%x cbr=0x%x avail=%u "
                   "hdr=0x%x cmd=0x%x\n",
                   (uint64_t)g_rx_dbg, (uint64_t)reason, (uint64_t)off,
                   (uint64_t)cbr, (uint64_t)avail, (uint64_t)header,
                   (uint64_t)rtl_inb(RTL_CMD));
}

/* One ring entry is: 4-byte header, then the ethernet frame, then 4 bytes the
 * header's length does not count (FCS), padded so the next entry starts on a
 * 4-byte boundary. Header bits 16..30 hold the length *including* the 4-byte
 * header. CAPR is programmed as (read pointer - 16); the chip adds the 16 back.
 *
 * Emptiness detection deliberately does NOT use ChipCmd.BUFE or CBR:
 *   - writing RXEN|TXEN clears BUFE and the QEMU model never re-asserts it;
 *   - in this QEMU (11.1) CBR reads back 0 even while the ring holds frames
 *     (verified with the monitor: CAPR=0, CBR=0, ChipCmd=0 while 612 bytes of
 *     ARP/ICMP traffic sat in the ring), so a CBR-derived write pointer cannot
 *     be trusted. The ring is zeroed at init and only the chip writes to it, so
 *     "the slot at the read pointer carries a valid ROK header" is the reliable
 *     empty/full signal, and it needs no device register at all. */
int rtl8139_receive(void (*fn)(const uint8_t *frame, uint32_t len))
{
    uint32_t delivered = 0;
    uint32_t guard;

    if (!g_rtl_state || !fn)
        return 0;

    /* Latch and clear the status register (diagnostics + keeps the latched
     * overrun bits from sticking). */
    g_last_isr = rtl_inw(RTL_ISR);
    if (g_last_isr != 0u)
        rtl_outw(RTL_ISR, g_last_isr);

    if ((g_last_isr & (RTL_ISR_RXOVW | RTL_ISR_FOVW | RTL_ISR_RER)) != 0u) {
        /* The chip dropped frame(s). We cannot resync from CBR here (see the
         * header comment), so just count it and re-assert our read pointer: the
         * header-validity scan below keeps the ring consistent. */
        g_rx_drops++;
        rtl_outw(RTL_CAPR, (uint16_t)((g_rx_ring_head - 16u) & 0xFFFFu));
    }

    /* Bounded: an 8 KiB ring cannot hold more 4-byte slots than this. */
    for (guard = 0; guard < (RTL_RX_RING_SIZE / 4u); guard++) {
        uint32_t off = g_rx_ring_head;
        uint32_t header;
        uint32_t pkt_len;
        uint32_t data_len;
        uint32_t spins;

        header = (uint32_t)g_rx_ring[off]
               | ((uint32_t)g_rx_ring[off + 1u] << 8)
               | ((uint32_t)g_rx_ring[off + 2u] << 16)
               | ((uint32_t)g_rx_ring[off + 3u] << 24);
        pkt_len = (header >> 16) & 0x7FFFu;      /* includes the 4-byte header */

        if ((header & 0x1u) == 0u ||             /* ROK: not a received packet  */
            pkt_len < 4u || pkt_len > RTL_TX_MAX_FRAME + 4u) {
            /* Ring empty (an untouched slot reads back as 0), or an entry the
             * chip has not finished writing. Do not advance: a later poll will
             * see the completed header. */
            break;
        }

        /* Give the chip a moment to land the payload: the header can become
         * visible one DMA burst before the frame bytes do. Bounded and short. */
        for (spins = RTL_RX_SETTLE_SPINS; spins != 0u; spins--)
            io_wait();

        data_len = pkt_len - 4u;

        /* Invalidate the cache lines the chip just wrote before parsing. */
        rtl_clflush(g_rx_ring + off, pkt_len);

        {
            uint32_t data_off = (off + 4u) % RTL_RX_RING_SIZE;

            if (data_off + data_len <= RTL_RX_RING_TOTAL) {
                /* Contiguous: deliver straight out of the ring. The callback
                 * runs before CAPR advances, so the chip cannot overwrite the
                 * frame while the stack parses it. */
                fn(g_rx_ring + data_off, data_len);
                g_frames_rx++;
                delivered++;
            } else {
                /* The frame wraps the 8 KiB boundary: the chip's write pointer
                 * reached the end of the ring and continued at offset 0 (with
                 * the 1500-byte pad the RIC keeps). Linearise it into a scratch
                 * frame so the stack still receives one contiguous buffer -- a
                 * plain drop here would lose real traffic, e.g. a 1500-byte
                 * segment that lands on the boundary. */
                uint32_t first = RTL_RX_RING_SIZE - data_off;

                if (data_len <= sizeof(g_wrap_buf) && first <= data_len) {
                    memcpy(g_wrap_buf, g_rx_ring + data_off, first);
                    memcpy(g_wrap_buf + first, g_rx_ring, data_len - first);
                    fn(g_wrap_buf, data_len);
                    g_frames_rx++;
                    delivered++;
                    g_rx_wraps++;
                } else {
                    rtl_rx_dbg(3u, off, 0u, 0u, header);
                    g_rx_drops++;
                }
            }
        }

        /* Advance: the next entry starts at off + pkt_len + 4 rounded up to 4.
         *
         * SutureOS fix: the extra 4 bytes are the FCS/reserved word the chip
         * writes after the frame and which the header length does NOT count.
         * Measured directly in the guest ring (monitor `xp /96bx` on g_rx_ring):
         *   +0x00: 01 00 44 00   header, ROK|len=0x44=68
         *   +0x04: 52 54 00 12 ..  the 42-byte ARP reply, padded with zeros
         *   +0x44: d8 ac 72 f8   FCS (not counted by len)
         *   +0x48: 01 00 44 00   next entry's header  -> the real stride is 72.
         * Without the +4 the driver landed exactly on the FCS, read it as a
         * header (0xf872acd8 -> bogus length), took the desync path, resynced to
         * the chip's write pointer and thereby skipped every buffered frame: one
         * ARP reply was learned per boot and ping always timed out. */
        g_rx_ring_head = (off + pkt_len + 4u + 3u) & ~3u;
        g_rx_ring_head %= RTL_RX_RING_SIZE;
        /* CAPR holds (read pointer - 16); the chip adds the 16 back. */
        rtl_outw(RTL_CAPR, (uint16_t)((g_rx_ring_head - 16u) & 0xFFFFu));
    }

    return (int)delivered;
}

/* ===================== self test ===================== */
/* Probe-only: never transmits and never needs a link, so it is safe to run on
 * any machine. A NIC-less machine is reported as a pass with a note, because a
 * missing qemu -device rtl8139 is a configuration fact, not a kernel defect. */
void st_rtl8139_test(void)
{
    uint8_t bus = 0, dev = 0, fn = 0;
    bool ok = true;

    if (pci_find_rtl8139(&bus, &dev, &fn) != 0) {
        console_printf("[selftest][rtl8139] no 10EC:8139 on the PCI bus "
                       "(add -device rtl8139,netdev=...)\n");
        /* Diagnostics: report what the config-space scan actually reads, so a
         * broken mechanism #1 is distinguishable from a missing device. */
        for (uint32_t d = 0; d < 6u; d++) {
            const uint32_t id = pci_cfg_read32(0, (uint8_t)d, 0, PCI_CFG_VENDOR_ID);
            console_printf("[selftest][rtl8139]   probe 0:%u.0 -> vendor=0x%x device=0x%x\n",
                           (uint64_t)d, (uint64_t)(id & 0xFFFFu),
                           (uint64_t)((id >> 16) & 0xFFFFu));
        }
        st_run("rtl8139", true);
        return;
    }

    /* The device must answer with a valid vendor ID and an I/O BAR. */
    {
        uint32_t id  = pci_cfg_read32(bus, dev, fn, PCI_CFG_VENDOR_ID);
        uint32_t bar = pci_cfg_read32(bus, dev, fn, PCI_CFG_BAR0);
        ok = ok && ((id & 0xFFFFu) == RTL8139_VENDOR_ID);
        ok = ok && (((id >> 16) & 0xFFFFu) == RTL8139_DEVICE_ID);
        ok = ok && ((bar & 0x1u) != 0u);                 /* I/O space BAR */
        ok = ok && ((bar & ~0x3u) <= 0xFF00u);
        if (ok)
            console_printf("[selftest][rtl8139] pci %u:%u.%u bar0=0x%x io=0x%x\n",
                           (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
                           (uint64_t)bar, (uint64_t)(bar & ~0x3u));
    }

    /* Register readback when the driver is up: MAC must not be all-zero or
     * all-ones (both indicate an unbonded / dead device). */
    if (g_rtl_state) {
        uint32_t i, or_all = 0, and_all = 0xFFu;
        for (i = 0; i < 6u; i++) {
            or_all  |= g_mac[i];
            and_all &= g_mac[i];
        }
        ok = ok && (or_all != 0u) && (and_all != 0xFFu);
        ok = ok && ((rtl_inb(RTL_CMD) & (RTL_CMD_RXEN | RTL_CMD_TXEN)) ==
                    (RTL_CMD_RXEN | RTL_CMD_TXEN));
        ok = ok && (rtl_inl(RTL_RBSTART) == (uint32_t)(uintptr_t)g_rx_ring);
    }

    st_run("rtl8139", ok);
}
