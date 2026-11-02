/* SutureOS module: USB 2.0 (EHCI) host controller + USB mass storage (BOT/SCSI)
 *
 * Source: HanOS kernel/device/usb/ehci.c (MIT) - controller bring-up, port
 *         reset, async QH/qTD chain, port enumeration and the Bulk-Only
 *         Transport read/write path. CBW/CSW layout and SCSI CDB packing were
 *         cross-checked against HBOS src/usb_msc.c (GPL-3.0).
 * Original licenses: MIT (HanOS), GPL-3.0 (HBOS). SutureOS is GPL-3.0.
 * Changes (SutureOS port, all of them marked "SutureOS" where they are subtle):
 *   - PCI discovery uses SutureOS' mechanism #1 accessors (same as ahci.c)
 *     scanning class 0x0C / sub-class 0x03 / prog-if 0x20 instead of HanOS'
 *     pci_devices vector;
 *   - BAR0 is mapped with mmu_map_mmio_identity(): an EHCI BAR is commonly
 *     above 1 GiB, and dereferencing an unmapped MMIO address is a #PF, not a
 *     clean failure;
 *   - delays come from the 8254 PIT counter (mb_pit.h) instead of HPET, so they
 *     also work with interrupts masked (kmain probes block devices with IF
 *     state that this driver must not depend on);
 *   - DMA uses one static, page-aligned .bss arena (identity mapped, exactly
 *     like ahci.c/nvme.c) instead of pmm_get() pages, with clflush around the
 *     DMA on real hardware;
 *   - added SCSI TEST UNIT READY, INQUIRY, SYNCHRONIZE CACHE, and the
 *     blk_device registration (kernel/drivers/block.h, drive 16);
 *   - logging is SutureOS console_printf with the "[usb]" prefix.
 *
 * Two things about the EHCI programming model that are easy to get wrong and
 * are the reason this file looks the way it does:
 *
 *  1. Port reset must NOT wait for the controller to clear PORTSC.PR.
 *     QEMU's EHCI model keeps PR asserted until software clears it, so polling
 *     for PR==0 spins forever with no PANIC and looks like a hang somewhere
 *     else. The sequence that works (and matches Linux/SeaBIOS) is:
 *     write PR=1, hold ~50 ms, write PR=0, then poll PORTSC.PED.
 *  2. The qTD chain is published in the QH overlay's *Next qTD Pointer*
 *     (offset 0x10), never in Current qTD Pointer (0x08) - that field is owned
 *     by the hardware. Publishing into the wrong field makes the controller
 *     accept the QH but execute zero qTDs ("timeout, token=00000000").
 *
 * Limitation (reported, not worked around): only high-speed devices are
 * enumerated. A full/low-speed device that happens to be routed to an EHCI
 * root port needs split transactions through the companion's transaction
 * translator; instead this driver hands the port back to the companion
 * controller (PORTSC.PO=1) and logs it. QEMU's usb-storage on `usb-ehci` is
 * high speed, which is what the acceptance test uses.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

#include "mmu.h"        /* mmu_map_mmio_identity()                    */
#include "mb_pit.h"     /* 8254 counter: wall clock without interrupts */
#include "block.h"      /* blk_device / blk_register                  */
#include "usb_ehci.h"

/* ===================== PCI config space (mechanism #1) =====================
 * Same accessor pair as ahci.c/rtl8139.c: port 0xCF8 must be written as a
 * DWORD, byte-wise writes are silently ignored on QEMU. */
#define USB_PCI_CFG_ADDR 0xCF8u
#define USB_PCI_CFG_DATA 0xCFCu

#define USB_PCI_CFG_VENDOR_ID 0x00u
#define USB_PCI_CFG_COMMAND   0x04u
#define USB_PCI_CFG_REV_CLASS 0x08u
#define USB_PCI_CFG_HEADER    0x0Cu
#define USB_PCI_CFG_BAR0      0x10u

#define USB_PCI_CMD_IO_SPACE   (1u << 0)
#define USB_PCI_CMD_MEM_SPACE  (1u << 1)
#define USB_PCI_CMD_BUS_MASTER (1u << 2)

static uint32_t usb_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(USB_PCI_CFG_ADDR, addr);
    return inl(USB_PCI_CFG_DATA);
}

static void usb_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                                uint32_t v)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(USB_PCI_CFG_ADDR, addr);
    outl(USB_PCI_CFG_DATA, v);
}

/* ===================== EHCI register map ===================== */
/* Capability registers, relative to the MMIO base. */
#define EHCI_CAP_CAPLENGTH  0x00u   /* byte  : capability register length   */
#define EHCI_CAP_HCIVERSION 0x02u
#define EHCI_CAP_HCSPARAMS  0x04u   /* low 4 bits: N_PORTS                  */
#define EHCI_CAP_HCCPARAMS  0x08u   /* bit 0: 64-bit addressing capable     */

/* Operational registers, relative to CAPLENGTH. */
#define EHCI_OP_USBCMD           0x00u
#define EHCI_OP_USBSTS           0x04u
#define EHCI_OP_USBINTR          0x08u
#define EHCI_OP_FRINDEX          0x0Cu
#define EHCI_OP_CTRLDSSEGMENT    0x10u
#define EHCI_OP_PERIODICLISTBASE 0x14u
#define EHCI_OP_ASYNCLISTADDR    0x18u
#define EHCI_OP_CONFIGFLAG       0x40u
#define EHCI_OP_PORTSC(n)        (0x44u + ((uint32_t)(n) - 1u) * 4u)

#define EHCI_USBCMD_RS      (1u << 0)   /* Run/Stop                     */
#define EHCI_USBCMD_HCRESET (1u << 1)
#define EHCI_USBCMD_PSE     (1u << 4)   /* Periodic Schedule Enable     */
#define EHCI_USBCMD_ASE     (1u << 5)   /* Asynchronous Schedule Enable */

#define EHCI_USBSTS_HCHALTED    (1u << 12)
#define EHCI_USBSTS_RECLAMATION (1u << 13)
#define EHCI_USBSTS_ASYNC       (1u << 15)

/* PORTSC bits. PORTSC.PED is R/W: writing a whole word back with the port
 * enabled bit cleared disables the port, so every write here keeps the bits it
 * read (this is the "PED got cleared" trap). */
#define EHCI_PORTSC_CCS   (1u << 0)     /* Current Connect Status (ro)   */
#define EHCI_PORTSC_CSC   (1u << 1)     /* Connect Status Change  (W1C)  */
#define EHCI_PORTSC_PED   (1u << 2)     /* Port Enabled/Disabled         */
#define EHCI_PORTSC_PEDC  (1u << 3)     /* Port Enable Change    (W1C)   */
#define EHCI_PORTSC_OCC   (1u << 5)     /* Over-current Change   (W1C)   */
#define EHCI_PORTSC_PR    (1u << 8)     /* Port Reset                    */
#define EHCI_PORTSC_PP    (1u << 12)    /* Port Power                    */
#define EHCI_PORTSC_PO    (1u << 13)    /* Port Owner (1 = companion)    */

/* Change bits that must be cleared by writing 1. */
#define EHCI_PORTSC_RWC (EHCI_PORTSC_CSC | EHCI_PORTSC_PEDC | EHCI_PORTSC_OCC)

/* PORTSC bits 27:26 = Port Speed: 0=full, 1=low, 2=high. */
#define EHCI_PORTSC_SPEED_SHIFT 26
#define EHCI_SPEED_FULL 0u
#define EHCI_SPEED_LOW  1u
#define EHCI_SPEED_HIGH 2u

/* ===================== queue element transfer descriptor =====================
 * 32-byte base structure, padded to the 64-byte 64-bit-capable layout
 * (EHCI appendix B): the five high dword buffer pointers live at +0x20.
 * The controller reads/writes only the fields its HCCPARAMS advertises, so the
 * superset layout is safe either way; it must only stay 32-byte aligned. */
typedef struct {
    uint32_t next;          /* 0x00 next qTD (bit0 = terminate)         */
    uint32_t alt_next;      /* 0x04                                     */
    uint32_t token;         /* 0x08 status/PID/bytes/toggle             */
    uint32_t buf[5];        /* 0x0C five buffer page pointers           */
    uint32_t buf_hi[5];     /* 0x20 their high dwords (64-bit capable)  */
    uint32_t pad[3];        /* 0x34 pad to 64 bytes                     */
} __attribute__((aligned(32))) usb_qtd_t;

/* ===================== queue head =====================
 * 0x00 horiz, 0x04 epchar, 0x08 epcap, then the transfer overlay:
 * 0x0C cur_qtd (hardware owned), 0x10 next_qtd (where the chain goes),
 * 0x14 alt_qtd, 0x18 token, 0x1C.. buffers. Padded to 96 bytes so a following
 * QH stays 32-byte aligned. */
typedef struct {
    uint32_t horiz;         /* 0x00                                     */
    uint32_t epchar;        /* 0x04                                     */
    uint32_t epcap;         /* 0x08                                     */
    uint32_t cur_qtd;       /* 0x0C overlay: current qTD (hardware)     */
    uint32_t next_qtd;      /* 0x10 overlay: next qTD (we publish here) */
    uint32_t alt_qtd;       /* 0x14                                     */
    uint32_t token;         /* 0x18                                     */
    uint32_t buf[5];        /* 0x1C                                     */
    uint32_t buf_hi[5];     /* 0x30                                     */
    uint32_t pad[7];        /* pad to 96 bytes                          */
} __attribute__((aligned(32))) usb_qh_t;

#define USB_QTD_ACTIVE   (1u << 7)
#define USB_QTD_HALTED   (1u << 6)
#define USB_QTD_DBE      (1u << 5)      /* data buffer error            */
#define USB_QTD_BABBLE   (1u << 4)
#define USB_QTD_XACTERR  (1u << 3)
#define USB_QTD_MISSED   (1u << 2)
#define USB_QTD_STALL    (1u << 0)
#define USB_QTD_ERR_MASK (USB_QTD_HALTED | USB_QTD_DBE | USB_QTD_BABBLE | \
                          USB_QTD_XACTERR | USB_QTD_MISSED | USB_QTD_STALL)

#define USB_PID_OUT   0u
#define USB_PID_IN    1u
#define USB_PID_SETUP 2u

#define USB_QTD_IOC             (1u << 15)
#define USB_QTD_BYTES_SHIFT     16
#define USB_QTD_DT              (1u << 31)      /* data toggle              */
#define USB_QTD_CERR_3          (3u << 10)      /* 3 retries on error       */
#define USB_QTD_TOKEN(pid, len) (((uint32_t)(pid) << 8) | USB_QTD_CERR_3 | \
                                 USB_QTD_IOC | USB_QTD_ACTIVE |          \
                                 ((uint32_t)(len) << USB_QTD_BYTES_SHIFT))

#define USB_QH_TERMINATE 0x1u

/* ===================== transfer sizes / DMA arena =====================
 * One BOT data stage is one EHCI bulk transfer. A qTD can describe at most
 * five 4 KiB pages, so a page-aligned 16 KiB buffer always fits in four
 * pointers; blk_read()/blk_write() chunk larger requests into 32-sector
 * pieces (16 KiB) instead of growing the arena. */
#define USB_SECTOR_SIZE      512u
#define USB_MAX_XFER_SECTORS 32u
#define USB_DATA_BYTES       (USB_MAX_XFER_SECTORS * USB_SECTOR_SIZE)   /* 16 KiB */
#define USB_QTD_COUNT        16u

/* Static DMA arena: the kernel identity maps low physical memory and the PMM
 * starts after _bss_end, so the address of these arrays IS the physical
 * address used for DMA (same argument as ahci.c). */
static uint32_t    g_framelist[1024] __attribute__((aligned(4096)));
static usb_qh_t    g_qh[2]           __attribute__((aligned(4096)));
static usb_qtd_t   g_qtd[USB_QTD_COUNT] __attribute__((aligned(4096)));
static uint8_t     g_data[USB_DATA_BYTES] __attribute__((aligned(4096)));
static uint8_t     g_small[128]      __attribute__((aligned(64)));
static uint8_t     g_setup[64]       __attribute__((aligned(64)));
static uint8_t     g_cbw[64]         __attribute__((aligned(64)));
static uint8_t     g_csw[64]         __attribute__((aligned(64)));
static uint8_t     g_save[USB_SECTOR_SIZE] __attribute__((aligned(64)));
static uint8_t     g_pat[USB_SECTOR_SIZE]  __attribute__((aligned(64)));

/* ===================== driver state ===================== */
static uint64_t g_mmio;             /* BAR0, identity mapped              */
static uint32_t g_op;               /* operational register base          */
static uint32_t g_nports;
static int      g_probe_done;
static int      g_have_device;      /* a mass-storage device is ready     */
static int      g_drive = -1;
static uint64_t g_blocks;
static uint32_t g_block_size = USB_SECTOR_SIZE;

static uint8_t  g_dev_addr;
static uint8_t  g_dev_speed = EHCI_SPEED_HIGH;
static uint8_t  g_dev_port;
static uint8_t  g_bulk_in_ep, g_bulk_out_ep;
static uint16_t g_bulk_in_mps = 512, g_bulk_out_mps = 512;
static uint32_t g_in_toggle, g_out_toggle;
static uint32_t g_tag;
static uint32_t g_xfer_errors;
static char     g_inq_vendor[9];
static char     g_inq_product[17];

static struct blk_device g_blk;
static const struct blk_ops g_usb_ops;

/* ===================== helpers ===================== */
static inline uint32_t usb_mmio_rd(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(g_mmio + (uint64_t)off);
}

static inline void usb_mmio_wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(g_mmio + (uint64_t)off) = v;
    __asm__ volatile("" ::: "memory");
}

static inline uint32_t usb_port_rd(uint32_t port)
{
    /* PORTSC lives in the operational register block: base (CAPLENGTH) plus
     * 0x44. Forgetting g_op here reads unimplemented space that reads as zero,
     * which looks exactly like "no device attached". */
    return usb_mmio_rd(g_op + EHCI_OP_PORTSC(port));
}

static inline void usb_port_wr(uint32_t port, uint32_t v)
{
    usb_mmio_wr(g_op + EHCI_OP_PORTSC(port), v);
}

static inline uint64_t usb_ptr_phys(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

/* Write-back cache maintenance around DMA. QEMU is coherent and does not need
 * it; real hardware does, and a stale line is one of the classic "the device
 * wrote it but the CPU reads zeros" bugs. */
static void usb_clflush(const void *addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)addr & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)addr + len;

    while (p < end) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
        p += 64;
    }
}

/* Millisecond delay from the 8254 counter.
 * Mode 3 makes the raw count arithmetic error prone (the count does not
 * decrement by exactly one per input clock for every divisor), so - exactly
 * like ac97.c - the loop counts *reloads*: whenever the latched value jumps up
 * one full PIT period (1000/hz ms) has elapsed. The counter is sampled far
 * more often than once per period, so no reload can be missed. The spin
 * backstop guarantees this can never hang the boot. */
#define USB_DELAY_SPIN_BACKSTOP 20000000u

static void usb_delay_ms(uint32_t ms)
{
    uint32_t hz = mbos_pit_hz();
    uint32_t period_ms;
    uint32_t reloads = 0;
    uint32_t spins = 0;
    uint16_t last;

    if (ms == 0)
        return;
    if (hz == 0)
        hz = 100;
    period_ms = 1000u / hz;
    if (period_ms == 0)
        period_ms = 1;
    last = mbos_pit_counter();

    while (reloads * period_ms < ms) {
        uint16_t now = mbos_pit_counter();
        if (now > last)
            reloads++;
        last = now;
        if (++spins > USB_DELAY_SPIN_BACKSTOP)
            break;
        __asm__ volatile("pause");
    }
}

/* ===================== async schedule =====================
 * One queue head is re-armed for every transfer, so two transfers never
 * overlap. The async schedule is stopped while software rewrites the QH
 * (otherwise the controller may be mid-fetch and the overlay is torn). */
static void usb_qh_arm(usb_qh_t *qh, uint8_t addr, uint8_t ep, uint8_t speed,
                       uint16_t mps, uint32_t qtd_index)
{
    uint32_t i;

    /* Stop async scheduling: RS|PSE, no ASE. */
    usb_mmio_wr(g_op + EHCI_OP_USBCMD, EHCI_USBCMD_RS | EHCI_USBCMD_PSE);
    for (i = 0; i < 100u; i++) {
        if ((usb_mmio_rd(g_op + EHCI_OP_USBSTS) & EHCI_USBSTS_ASYNC) == 0u)
            break;
        usb_delay_ms(1);
    }
    /* Clear latched status errors (RW1C): a host system error halts the HC. */
    usb_mmio_wr(g_op + EHCI_OP_USBSTS, 0x3Fu);

    qh->horiz = (uint32_t)usb_ptr_phys(qh) | USB_QH_TERMINATE;   /* head, self loop */
    qh->epchar = (uint32_t)addr
               | ((uint32_t)ep << 8)
               | ((uint32_t)speed << 12)
               | (1u << 14)                       /* DTC: use qTD toggle  */
               | (1u << 15)                       /* H: reclamation head  */
               | ((uint32_t)mps << 16)
               | (ep == 0u ? (1u << 27) : 0u)     /* control endpoint     */
               | (7u << 28);                      /* NAK reload           */
    qh->epcap = 1u << 30;                         /* multiplier 1         */
    qh->cur_qtd = USB_QH_TERMINATE;
    /* SutureOS: the chain MUST be published at the overlay's Next qTD
     * pointer (0x10). Writing cur_qtd instead makes the controller fetch
     * nothing at all. */
    qh->next_qtd = (uint32_t)usb_ptr_phys(&g_qtd[qtd_index]);
    qh->alt_qtd = USB_QH_TERMINATE;
    qh->token = 0;

    usb_clflush(qh, sizeof(*qh));
    __asm__ volatile("mfence" ::: "memory");
    usb_mmio_wr(g_op + EHCI_OP_ASYNCLISTADDR, (uint32_t)usb_ptr_phys(qh));
    usb_mmio_wr(g_op + EHCI_OP_USBCMD,
                EHCI_USBCMD_RS | EHCI_USBCMD_PSE | EHCI_USBCMD_ASE);
}

/* Wait until every qTD in the chain left the active state. */
static int usb_wait_qtds(usb_qtd_t *first, uint32_t count, uint32_t ms)
{
    uint32_t elapsed;

    for (elapsed = 0; elapsed <= ms; elapsed++) {
        uint32_t i;
        int busy = 0;

        for (i = 0; i < count; i++) {
            if (first[i].token & USB_QTD_ACTIVE) {
                busy = 1;
                break;
            }
        }
        if (!busy)
            return 0;
        usb_delay_ms(1);
    }
    return -1;
}

static int usb_check_qtds(uint32_t count)
{
    uint32_t i;
    int ok = 1;

    for (i = 0; i < count; i++) {
        if (g_qtd[i].token & USB_QTD_ERR_MASK)
            ok = 0;
    }
    return ok ? 0 : 1;
}

/* Fill the five page pointers of a qTD. The controller walks the transfer page
 * by page, so a buffer that straddles a page boundary needs every page
 * pointer set (a 512-byte read that happens to cross 4 KiB is exactly the case
 * where a single pointer silently corrupts the tail). */
static void usb_qtd_buffers(usb_qtd_t *q, uint64_t addr, uint32_t len)
{
    uint32_t pages;
    uint32_t i;

    for (i = 0; i < 5u; i++) {
        q->buf[i] = 0;
        q->buf_hi[i] = 0;
    }
    if (len == 0)
        return;

    pages = (uint32_t)(((addr & 0xFFFu) + (uint64_t)len + 0xFFFu) >> 12);
    if (pages == 0)
        pages = 1;
    if (pages > 5u) {
        pages = 5u;         /* caller chunks the transfer; never reached */
    }
    q->buf[0] = (uint32_t)addr;
    for (i = 1; i < pages; i++)
        q->buf[i] = (uint32_t)((addr & ~0xFFFULL) + (uint64_t)i * 0x1000ULL);
}

/* ===================== control transfer =====================
 * SETUP -> optional DATA -> STATUS through one QH, each stage a qTD. */
static int usb_control(uint8_t addr, uint8_t speed, uint16_t mps,
                       const uint8_t *setup, uint32_t len, int in,
                       const void *out)
{
    uint32_t n = 0;

    memcpy(g_setup, setup, 8);
    if (len > 0 && !in && out != NULL)
        memcpy(g_data, out, len);

    memset(g_qtd, 0, sizeof(g_qtd));

    /* SETUP stage: 8 bytes, DATA0 (toggle bit clear). */
    g_qtd[0].next = (uint32_t)usb_ptr_phys(&g_qtd[1]);
    g_qtd[0].alt_next = USB_QH_TERMINATE;
    g_qtd[0].token = USB_QTD_TOKEN(USB_PID_SETUP, 8);
    g_qtd[0].buf[0] = (uint32_t)usb_ptr_phys(g_setup);
    n = 1;

    if (len > 0) {
        /* DATA stage: DATA1. */
        g_qtd[1].next = (uint32_t)usb_ptr_phys(&g_qtd[2]);
        g_qtd[1].alt_next = USB_QH_TERMINATE;
        g_qtd[1].token = USB_QTD_TOKEN(in ? USB_PID_IN : USB_PID_OUT, len)
                       | USB_QTD_DT;
        usb_qtd_buffers(&g_qtd[1], usb_ptr_phys(g_data), len);
        n = 2;
    }

    /* STATUS stage: opposite direction, zero length, still needs a valid
     * buffer pointer (some controllers fault on a null pointer). */
    g_qtd[n].next = USB_QH_TERMINATE;
    g_qtd[n].alt_next = USB_QH_TERMINATE;
    g_qtd[n].token = USB_QTD_TOKEN(in ? USB_PID_OUT : USB_PID_IN, 0)
                   | USB_QTD_DT;
    g_qtd[n].buf[0] = (uint32_t)usb_ptr_phys(g_data);
    n++;

    usb_clflush(g_setup, 8);
    usb_clflush(g_data, len > 0 ? len : 1u);
    usb_clflush(g_qtd, n * sizeof(usb_qtd_t));

    usb_qh_arm(&g_qh[0], addr, 0, speed, mps, 0);

    if (usb_wait_qtds(g_qtd, n, 500u) != 0 || usb_check_qtds(n) != 0) {
        console_printf("[usb] control req=%x addr=%u len=%u %s FAILED "
                       "(tok=%x sts=%x)\n",
                       (uint64_t)setup[1], (uint64_t)addr, (uint64_t)len,
                       in ? "in" : "out",
                       (uint64_t)g_qtd[0].token,
                       (uint64_t)usb_mmio_rd(g_op + EHCI_OP_USBSTS));
        g_xfer_errors++;
        return -1;
    }
    if (in && len > 0)
        usb_clflush(g_data, len);
    return 0;
}

static int usb_get_descriptor(uint8_t addr, uint8_t speed, uint16_t mps,
                              uint8_t type, uint8_t index, uint16_t len)
{
    uint8_t setup[8];

    setup[0] = 0x80;        /* device-to-host, standard, device        */
    setup[1] = 6;           /* GET_DESCRIPTOR                          */
    setup[2] = 0;
    setup[3] = type;
    setup[4] = index;
    setup[5] = 0;
    setup[6] = (uint8_t)len;
    setup[7] = (uint8_t)(len >> 8);
    return usb_control(addr, speed, mps, setup, len, 1, NULL);
}

static int usb_set_address(uint8_t speed, uint16_t mps, uint8_t addr)
{
    uint8_t setup[8] = { 0x00, 5, (uint8_t)addr, 0, 0, 0, 0, 0 };
    return usb_control(0, speed, mps, setup, 0, 0, NULL);
}

static int usb_set_config(uint8_t addr, uint8_t speed, uint16_t mps, uint8_t value)
{
    uint8_t setup[8] = { 0x00, 9, (uint8_t)value, 0, 0, 0, 0, 0 };
    return usb_control(addr, speed, mps, setup, 0, 0, NULL);
}

/* ===================== bulk transfer =====================
 * All DMA goes through a page-aligned .bss bounce buffer, and `bounce` is
 * explicit: the CBW/CSW stages use the small one while the data stage uses the
 * 16 KiB one. That separation matters - a single shared bounce buffer makes the
 * (later) CSW read overwrite the data the caller is about to parse, which shows
 * up as "the file content starts with the 13-byte CSW". */
static int usb_bulk(uint8_t ep, uint16_t mps, int in, uint32_t *toggle,
                    void *buf, uint32_t len, uint8_t *bounce)
{
    if (len == 0 || len > USB_DATA_BYTES) {
        g_xfer_errors++;
        return -1;
    }
    if (!in)
        memcpy(bounce, buf, len);

    memset(&g_qtd[0], 0, sizeof(usb_qtd_t));
    g_qtd[0].next = USB_QH_TERMINATE;
    g_qtd[0].alt_next = USB_QH_TERMINATE;
    g_qtd[0].token = USB_QTD_TOKEN(in ? USB_PID_IN : USB_PID_OUT, len)
                   | (*toggle & USB_QTD_DT);
    usb_qtd_buffers(&g_qtd[0], usb_ptr_phys(bounce), len);

    usb_clflush(bounce, len);
    usb_clflush(&g_qtd[0], sizeof(g_qtd[0]));

    usb_qh_arm(&g_qh[0], g_dev_addr, ep, g_dev_speed, mps, 0);

    if (usb_wait_qtds(g_qtd, 1, 2000u) != 0 || usb_check_qtds(1) != 0) {
        console_printf("[usb] bulk ep=%x %s len=%u FAILED (tok=%x)\n",
                       (uint64_t)ep, in ? "in" : "out", (uint64_t)len,
                       (uint64_t)g_qtd[0].token);
        g_xfer_errors++;
        return -1;
    }

    *toggle ^= USB_QTD_DT;
    if (in) {
        usb_clflush(bounce, len);
        memcpy(buf, bounce, len);
    }
    return 0;
}

/* ===================== Bulk-Only Transport =====================
 * CBW (31 bytes out) -> optional data stage -> CSW (13 bytes in).
 * CBW signature 0x43425355 "USBC", CSW signature 0x53425355 "USBS". */
#define USB_BOT_CBW_SIG 0x43425355u
#define USB_BOT_CSW_SIG 0x53425355u

static int usb_bot(const uint8_t *cb, uint8_t cb_len, void *data,
                   uint32_t data_len, int in)
{
    uint32_t tag = ++g_tag;

    if (cb_len == 0 || cb_len > 16)
        return -1;
    if (data_len > USB_DATA_BYTES)
        return -1;

    memset(g_cbw, 0, 31);
    g_cbw[0] = (uint8_t)(USB_BOT_CBW_SIG & 0xFFu);
    g_cbw[1] = (uint8_t)((USB_BOT_CBW_SIG >> 8) & 0xFFu);
    g_cbw[2] = (uint8_t)((USB_BOT_CBW_SIG >> 16) & 0xFFu);
    g_cbw[3] = (uint8_t)((USB_BOT_CBW_SIG >> 24) & 0xFFu);
    g_cbw[4] = (uint8_t)(tag & 0xFFu);
    g_cbw[5] = (uint8_t)((tag >> 8) & 0xFFu);
    g_cbw[6] = (uint8_t)((tag >> 16) & 0xFFu);
    g_cbw[7] = (uint8_t)((tag >> 24) & 0xFFu);
    g_cbw[8]  = (uint8_t)(data_len & 0xFFu);
    g_cbw[9]  = (uint8_t)((data_len >> 8) & 0xFFu);
    g_cbw[10] = (uint8_t)((data_len >> 16) & 0xFFu);
    g_cbw[11] = (uint8_t)((data_len >> 24) & 0xFFu);
    g_cbw[12] = in ? 0x80u : 0x00u;
    g_cbw[13] = 0;                  /* LUN 0                             */
    g_cbw[14] = cb_len;
    memcpy(g_cbw + 15, cb, cb_len);

    if (usb_bulk(g_bulk_out_ep, g_bulk_out_mps, 0, &g_out_toggle,
                 g_cbw, 31, g_small) != 0)
        return -1;

    if (data_len > 0) {
        uint8_t *buf = (uint8_t *)data;
        uint32_t done = 0;

        /* One bulk transfer per CBW data stage fits the 16 KiB arena; the loop
         * is here so that a future bigger command cannot overflow a qTD's five
         * page pointers. */
        while (done < data_len) {
            uint32_t chunk = data_len - done;

            if (chunk > USB_DATA_BYTES)
                chunk = USB_DATA_BYTES;
            if (in) {
                if (usb_bulk(g_bulk_in_ep, g_bulk_in_mps, 1, &g_in_toggle,
                             buf + done, chunk, g_data) != 0)
                    return -1;
            } else {
                if (usb_bulk(g_bulk_out_ep, g_bulk_out_mps, 0, &g_out_toggle,
                             buf + done, chunk, g_data) != 0)
                    return -1;
            }
            done += chunk;
        }
    }

    memset(g_csw, 0, 13);
    if (usb_bulk(g_bulk_in_ep, g_bulk_in_mps, 1, &g_in_toggle, g_csw, 13,
                 g_small) != 0)
        return -1;

    {
        uint32_t sig = (uint32_t)g_csw[0] | ((uint32_t)g_csw[1] << 8)
                     | ((uint32_t)g_csw[2] << 16) | ((uint32_t)g_csw[3] << 24);
        uint32_t rtag = (uint32_t)g_csw[4] | ((uint32_t)g_csw[5] << 8)
                      | ((uint32_t)g_csw[6] << 16) | ((uint32_t)g_csw[7] << 24);
        uint32_t residue = (uint32_t)g_csw[8] | ((uint32_t)g_csw[9] << 8)
                         | ((uint32_t)g_csw[10] << 16) | ((uint32_t)g_csw[11] << 24);

        if (sig != USB_BOT_CSW_SIG || rtag != tag) {
            console_printf("[usb] bot CSW mismatch sig=%x tag=%x/%x\n",
                           (uint64_t)sig, (uint64_t)rtag, (uint64_t)tag);
            g_xfer_errors++;
            return -1;
        }
        if (g_csw[12] != 0) {
            console_printf("[usb] bot CSW status=%u residue=%u dir=%s\n",
                           (uint64_t)g_csw[12], (uint64_t)residue,
                           in ? "in" : "out");
            g_xfer_errors++;
            return -1;
        }
        if (residue > data_len) {
            console_printf("[usb] bot CSW residue too large (%u > %u)\n",
                           (uint64_t)residue, (uint64_t)data_len);
            g_xfer_errors++;
            return -1;
        }
        if (residue != 0)
            return (int)(data_len - residue);
    }
    return (int)data_len;
}

/* ===================== SCSI commands (over BOT) ===================== */
#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_INQUIRY         0x12u
#define SCSI_READ_CAPACITY   0x25u
#define SCSI_READ10          0x28u
#define SCSI_WRITE10         0x2Au
#define SCSI_SYNC_CACHE10    0x35u

static int scsi_test_unit_ready(void)
{
    uint8_t cb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return usb_bot(cb, 6, NULL, 0, 1) == 0 ? 0 : -1;
}

static int scsi_inquiry(void)
{
    uint8_t cb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t *d = g_data;
    int i;

    if (usb_bot(cb, 6, g_data, 36, 1) < 0)
        return -1;
    /* INQUIRY data: byte 8..15 vendor, 16..31 product. */
    for (i = 0; i < 8; i++)
        g_inq_vendor[i] = (char)d[8 + i];
    for (i = 0; i < 16; i++)
        g_inq_product[i] = (char)d[16 + i];
    for (i = 7; i >= 0 && (g_inq_vendor[i] == ' ' || g_inq_vendor[i] == 0); i--)
        g_inq_vendor[i] = 0;
    for (i = 15; i >= 0 && (g_inq_product[i] == ' ' || g_inq_product[i] == 0); i--)
        g_inq_product[i] = 0;
    return 0;
}

static int scsi_read_capacity(uint64_t *blocks, uint32_t *bsec)
{
    uint8_t cb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t *d = g_data;
    uint32_t last;
    uint32_t bsize;

    if (usb_bot(cb, 10, g_data, 8, 1) < 0)
        return -1;
    last = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
         | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    bsize = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16)
          | ((uint32_t)d[6] << 8) | (uint32_t)d[7];
    if (last == 0xFFFFFFFFu)
        return -1;
    if (bsize == 0)
        return -1;
    *blocks = (uint64_t)last + 1ULL;
    *bsec = bsize;
    return 0;
}

static int scsi_read10(uint32_t lba, uint16_t count)
{
    uint8_t cb[10] = { SCSI_READ10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t bytes = (uint32_t)count * USB_SECTOR_SIZE;

    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[7] = (uint8_t)(count >> 8);
    cb[8] = (uint8_t)count;
    return usb_bot(cb, 10, g_data, bytes, 1) == (int)bytes ? 0 : -1;
}

static int scsi_write10(uint32_t lba, uint16_t count, const void *src)
{
    uint8_t cb[10] = { SCSI_WRITE10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t bytes = (uint32_t)count * USB_SECTOR_SIZE;

    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[7] = (uint8_t)(count >> 8);
    cb[8] = (uint8_t)count;
    memcpy(g_data, src, bytes);
    return usb_bot(cb, 10, g_data, bytes, 0) == (int)bytes ? 0 : -1;
}

static int scsi_sync_cache(void)
{
    uint8_t cb[10] = { SCSI_SYNC_CACHE10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return usb_bot(cb, 10, NULL, 0, 0) >= 0 ? 0 : -1;
}

/* ===================== ports ===================== */
/* Reset a port.
 * Do NOT poll for PR to self-clear (see the file header): hold it for 50 ms,
 * clear it explicitly, then wait for PED. */
static int usb_port_reset(uint32_t port)
{
    uint32_t psc;
    uint32_t i;

    psc = usb_port_rd(port);
    if (!(psc & EHCI_PORTSC_CCS))
        return -1;

    /* Keep PP and PED as read (PED is R/W: dropping it disables the port). */
    usb_port_wr(port, (psc & (EHCI_PORTSC_PP | EHCI_PORTSC_PED))
                      | EHCI_PORTSC_PR | EHCI_PORTSC_RWC);
    usb_delay_ms(50);

    psc = usb_port_rd(port);
    usb_port_wr(port, (psc & (EHCI_PORTSC_PP | EHCI_PORTSC_PED))
                      | EHCI_PORTSC_RWC);              /* clear PR */

    for (i = 0; i < 20u; i++) {
        psc = usb_port_rd(port);
        if (psc & EHCI_PORTSC_PED)
            return 0;
        if (!(psc & EHCI_PORTSC_CCS))
            return -1;
        usb_delay_ms(10);
    }
    console_printf("[usb] port %u reset: never enabled (portsc=%x)\n",
                   (uint64_t)port, (uint64_t)usb_port_rd(port));
    return -1;
}

/* ===================== enumeration ===================== */
static uint8_t usb_alloc_address(void)
{
    static uint8_t next = 1;
    uint8_t addr = next;

    next = (uint8_t)(next % 8u + 1u);
    return addr;
}

static int usb_enum_port(uint32_t port)
{
    uint8_t speed;
    uint16_t mps = 8;
    uint8_t addr;
    uint32_t i;
    uint16_t total;
    uint8_t *p, *end;
    uint8_t cfg_value = 1;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t in_mps = 64, out_mps = 64;
    int msc = 0;
    int ifaces = 0;

    if (usb_port_reset(port) != 0) {
        /* No PED after reset. EHCI spec 2.3.9 sets the enable bit only for a
         * high-speed device ("Table 2.16 Set the enable bit ... to indicate to
         * SW that this port has a high speed device attached"), so this is a
         * full/low-speed device: it belongs to the companion controller, which
         * is where the port is handed back to when one exists (PO=1). This
         * driver does not do split transactions. */
        uint32_t psc = usb_port_rd(port);
        console_printf("[usb] port %u not enabled after reset (portsc=%x) -> "
                       "%s\n", (uint64_t)port, (uint64_t)psc,
                       (psc & EHCI_PORTSC_CCS)
                           ? "full/low-speed device, left to the companion"
                           : "device disconnected");
        if (psc & EHCI_PORTSC_CCS) {
            usb_port_wr(port, (psc & (EHCI_PORTSC_PP | EHCI_PORTSC_PED))
                              | EHCI_PORTSC_PO | EHCI_PORTSC_RWC);
        }
        return -1;
    }

    /* PED set after reset => high-speed device. Do NOT read PORTSC.PSPD to
     * decide this: QEMU's EHCI model never fills those bits (it sets PED on the
     * reset release instead), so a PSPD-based check rejects a perfectly good
     * high-speed device. */
    speed = EHCI_SPEED_HIGH;
    console_printf("[usb] port %u reset ok speed=high (portsc=%x)\n",
                   (uint64_t)port, (uint64_t)usb_port_rd(port));

    g_dev_port = (uint8_t)port;
    g_dev_speed = speed;
    g_dev_addr = 0;

    /* 1) 8-byte device descriptor at the default address to learn MPS0. */
    if (usb_get_descriptor(0, speed, 8, 1, 0, 8) != 0)
        return -1;
    mps = (uint16_t)g_data[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64)
        mps = 8;

    /* 2) SET_ADDRESS. */
    addr = usb_alloc_address();
    if (usb_set_address(speed, mps, addr) != 0)
        return -1;
    g_dev_addr = addr;
    usb_delay_ms(5);

    /* 3) Full device descriptor (idVendor/idProduct after bytes 8..11). */
    if (usb_get_descriptor(addr, speed, mps, 1, 0, 18) != 0)
        return -1;
    {
        uint16_t vid = (uint16_t)g_data[8] | ((uint16_t)g_data[9] << 8);
        uint16_t pid = (uint16_t)g_data[10] | ((uint16_t)g_data[11] << 8);

        console_printf("[usb] device addr=%u mps=%u vendor=%04x product=%04x\n",
                       (uint64_t)addr, (uint64_t)mps,
                       (uint64_t)vid, (uint64_t)pid);
    }

    /* 4) Configuration descriptor: 9 bytes for wTotalLength, then the lot. */
    if (usb_get_descriptor(addr, speed, mps, 2, 0, 9) != 0)
        return -1;
    total = (uint16_t)g_data[2] | ((uint16_t)g_data[3] << 8);
    if (total < 9u || total > 512u)
        return -1;
    if (usb_get_descriptor(addr, speed, mps, 2, 0, total) != 0)
        return -1;

    /* 5) Walk the configuration: interface + bulk endpoints of a mass-storage
     *    interface (class 0x08). */
    p = g_data;
    end = p + total;
    while (p + 2 <= end && p[0] >= 2u && p[1] >= 2u && p + p[0] <= end) {
        if (p[1] == 2u && p[0] >= 9u) {                 /* configuration */
            cfg_value = p[5];
        } else if (p[1] == 4u && p[0] >= 9u) {          /* interface     */
            ifaces++;
            if (p[5] == 0x08u && !msc)                  /* mass storage  */
                msc = 1;
        } else if (p[1] == 5u && p[0] >= 7u) {          /* endpoint      */
            uint8_t ep = (uint8_t)(p[2] & 0x0Fu);
            uint16_t m = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            if ((p[3] & 0x03u) == 0x02u) {              /* bulk          */
                if (p[2] & 0x80u) {
                    if (!ep_in) {
                        ep_in = ep;
                        in_mps = m ? m : 64;
                    }
                } else {
                    if (!ep_out) {
                        ep_out = ep;
                        out_mps = m ? m : 64;
                    }
                }
            }
        }
        p += p[0];
    }

    console_printf("[usb] config set value=%u ifaces=%u msc=%u "
                   "ep_in=%02x ep_out=%02x mps=%u/%u\n",
                   (uint64_t)cfg_value, (uint64_t)ifaces, (uint64_t)msc,
                   (uint64_t)ep_in, (uint64_t)ep_out,
                   (uint64_t)in_mps, (uint64_t)out_mps);

    if (!msc || !ep_in || !ep_out) {
        console_printf("[usb] not a mass-storage device (msc=%u in=%02x "
                       "out=%02x) - skipped\n",
                       (uint64_t)msc, (uint64_t)ep_in, (uint64_t)ep_out);
        return -1;
    }

    if (usb_set_config(addr, speed, mps, cfg_value) != 0)
        return -1;
    /* SET_CONFIGURATION resets both bulk data toggles to DATA0. */
    g_in_toggle = 0;
    g_out_toggle = 0;

    g_bulk_in_ep = ep_in;
    g_bulk_out_ep = ep_out;
    g_bulk_in_mps = in_mps;
    g_bulk_out_mps = out_mps;

    /* 6) Proof the transport works before advertising any capacity. */
    for (i = 0; i < 20u; i++) {
        if (scsi_test_unit_ready() == 0)
            break;
        usb_delay_ms(50);
    }
    if (i == 20u) {
        console_printf("[usb] TEST UNIT READY never succeeded (errors=%u)\n",
                       (uint64_t)g_xfer_errors);
        return -1;
    }
    return 0;
}

/* ===================== block device ===================== */
static int usb_blk_read(struct blk_device *dev, uint64_t lba, uint32_t count,
                        void *buf)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;

    (void)dev;
    if (!g_have_device || count == 0 || !buf)
        return -1;

    while (done < count) {
        uint32_t chunk = count - done;
        uint64_t cl = lba + (uint64_t)done;

        if (chunk > USB_MAX_XFER_SECTORS)
            chunk = USB_MAX_XFER_SECTORS;
        if (cl >= g_blocks)
            return -1;
        if ((uint64_t)chunk > g_blocks - cl)
            chunk = (uint32_t)(g_blocks - cl);
        if (scsi_read10((uint32_t)cl, (uint16_t)chunk) != 0)
            return -1;
        memcpy(out + (uintptr_t)done * USB_SECTOR_SIZE, g_data,
               chunk * USB_SECTOR_SIZE);
        done += chunk;
    }
    return 0;
}

static int usb_blk_write(struct blk_device *dev, uint64_t lba, uint32_t count,
                         const void *buf)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    (void)dev;
    if (!g_have_device || count == 0 || !buf)
        return -1;

    while (done < count) {
        uint32_t chunk = count - done;
        uint64_t cl = lba + (uint64_t)done;

        if (chunk > USB_MAX_XFER_SECTORS)
            chunk = USB_MAX_XFER_SECTORS;
        if (cl >= g_blocks)
            return -1;
        if ((uint64_t)chunk > g_blocks - cl)
            chunk = (uint32_t)(g_blocks - cl);
        if (scsi_write10((uint32_t)cl, (uint16_t)chunk,
                         in + (uintptr_t)done * USB_SECTOR_SIZE) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static int usb_blk_flush(struct blk_device *dev)
{
    (void)dev;
    if (!g_have_device)
        return -1;
    return scsi_sync_cache();
}

static const struct blk_ops g_usb_ops = {
    .read  = usb_blk_read,
    .write = usb_blk_write,
    .flush = usb_blk_flush,
    .ioctl = NULL,
};

/* ===================== controller bring-up ===================== */
struct usb_pci_loc {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
};

/* class 0x0C (serial bus) / sub-class 0x03 (USB) / prog-if 0x20 (EHCI). */
static int usb_pci_find_ehci(struct usb_pci_loc *out)
{
    uint32_t bus;
    uint32_t dev;
    uint32_t fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t id = usb_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0,
                                             USB_PCI_CFG_VENDOR_ID);
            uint32_t hdr, fn_max;

            if ((id & 0xFFFFu) == 0xFFFFu || (id & 0xFFFFu) == 0x0000u)
                continue;
            hdr = usb_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0,
                                     USB_PCI_CFG_HEADER);
            fn_max = (hdr & 0x00800000u) ? 8u : 1u;
            for (fn = 0; fn < fn_max; fn++) {
                uint32_t cc;
                if (fn != 0u) {
                    id = usb_pci_cfg_read32((uint8_t)bus, (uint8_t)dev,
                                            (uint8_t)fn, USB_PCI_CFG_VENDOR_ID);
                    if ((id & 0xFFFFu) == 0xFFFFu)
                        continue;
                }
                cc = usb_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn,
                                        USB_PCI_CFG_REV_CLASS);
                if (((cc >> 24) & 0xFFu) == 0x0Cu &&
                    ((cc >> 16) & 0xFFu) == 0x03u &&
                    ((cc >> 8) & 0xFFu) == 0x20u) {
                    out->bus = (uint8_t)bus;
                    out->dev = (uint8_t)dev;
                    out->fn = (uint8_t)fn;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int usb_ctrl_setup(const struct usb_pci_loc *loc)
{
    uint32_t bar;
    uint32_t caplen;
    uint32_t hcs;
    uint32_t i;
    uint32_t cmd;

    /* Enable memory space + bus master (DMA). */
    cmd = usb_pci_cfg_read32(loc->bus, loc->dev, loc->fn, USB_PCI_CFG_COMMAND);
    usb_pci_cfg_write32(loc->bus, loc->dev, loc->fn, USB_PCI_CFG_COMMAND,
                        cmd | USB_PCI_CMD_MEM_SPACE | USB_PCI_CMD_BUS_MASTER);

    bar = usb_pci_cfg_read32(loc->bus, loc->dev, loc->fn, USB_PCI_CFG_BAR0)
        & ~0x0Fu;
    if (bar == 0u || (bar & 0x1u)) {
        console_printf("[usb] EHCI BAR0 missing or I/O space (%x)\n",
                       (uint64_t)bar);
        return -1;
    }
    /* SutureOS: the BAR can be above 1 GiB; an unmapped MMIO window is a #PF. */
    if (mmu_map_mmio_identity((uint64_t)bar, 0x1000u) != 0) {
        console_printf("[usb] mmu_map_mmio_identity(%x) failed\n",
                       (uint64_t)bar);
        return -1;
    }
    g_mmio = (uint64_t)bar;

    /* Static arena must be reachable by a 32-bit DMA address. */
    if (usb_ptr_phys(g_framelist) > 0xFFFFFFFFULL ||
        usb_ptr_phys(g_qh) > 0xFFFFFFFFULL ||
        usb_ptr_phys(g_qtd) > 0xFFFFFFFFULL ||
        usb_ptr_phys(g_data) > 0xFFFFFFFFULL) {
        console_printf("[usb] DMA arena above 4 GiB, EHCI cannot address it\n");
        return -1;
    }

    caplen = (uint32_t)(*(volatile uint8_t *)(uintptr_t)g_mmio);
    hcs = usb_mmio_rd(EHCI_CAP_HCSPARAMS);
    g_nports = hcs & 0x0Fu;
    if (caplen < 0x10u || caplen > 0xFFu || g_nports == 0u) {
        console_printf("[usb] EHCI caps look wrong (caplen=%u ports=%u)\n",
                       (uint64_t)caplen, (uint64_t)g_nports);
        return -1;
    }
    g_op = caplen;

    console_printf("[usb] ehci: pci %u:%u.%u mmio=%x ports=%u caplen=%u\n",
                   (uint64_t)loc->bus, (uint64_t)loc->dev, (uint64_t)loc->fn,
                   (uint64_t)bar, (uint64_t)g_nports, (uint64_t)caplen);

    /* Controller reset: stop, reset, wait for HCRESET to clear. */
    usb_mmio_wr(g_op + EHCI_OP_USBCMD, 0);
    for (i = 0; i < 100u; i++) {
        if (usb_mmio_rd(g_op + EHCI_OP_USBSTS) & EHCI_USBSTS_HCHALTED)
            break;
        usb_delay_ms(1);
    }
    usb_mmio_wr(g_op + EHCI_OP_USBCMD, EHCI_USBCMD_HCRESET);
    for (i = 0; i < 100u; i++) {
        if ((usb_mmio_rd(g_op + EHCI_OP_USBCMD) & EHCI_USBCMD_HCRESET) == 0u)
            break;
        usb_delay_ms(1);
    }
    if (i == 100u) {
        console_printf("[usb] EHCI reset never completed\n");
        return -1;
    }

    /* Periodic frame list: every entry terminated (this driver only uses the
     * async schedule, but the list must exist and be 4 KiB aligned). */
    memset(g_framelist, 0, sizeof(g_framelist));
    for (i = 0; i < 1024u; i++)
        g_framelist[i] = USB_QH_TERMINATE;

    memset(g_qh, 0, sizeof(g_qh));
    g_qh[0].horiz = (uint32_t)usb_ptr_phys(&g_qh[0]) | USB_QH_TERMINATE;
    g_qh[0].cur_qtd = USB_QH_TERMINATE;
    g_qh[0].next_qtd = USB_QH_TERMINATE;
    g_qh[0].alt_qtd = USB_QH_TERMINATE;

    memset(g_qtd, 0, sizeof(g_qtd));

    usb_mmio_wr(g_op + EHCI_OP_CTRLDSSEGMENT, 0);
    usb_mmio_wr(g_op + EHCI_OP_PERIODICLISTBASE,
                (uint32_t)usb_ptr_phys(g_framelist));
    usb_mmio_wr(g_op + EHCI_OP_ASYNCLISTADDR, (uint32_t)usb_ptr_phys(&g_qh[0]));
    usb_mmio_wr(g_op + EHCI_OP_CONFIGFLAG, 1);      /* we own the controller */
    usb_mmio_wr(g_op + EHCI_OP_USBINTR, 0);        /* polled, no interrupts   */

    usb_clflush(g_framelist, sizeof(g_framelist));
    usb_clflush(g_qh, sizeof(g_qh));
    usb_clflush(g_qtd, sizeof(g_qtd));

    usb_mmio_wr(g_op + EHCI_OP_USBCMD,
                EHCI_USBCMD_RS | EHCI_USBCMD_PSE | EHCI_USBCMD_ASE);
    for (i = 0; i < 100u; i++) {
        if ((usb_mmio_rd(g_op + EHCI_OP_USBSTS) & EHCI_USBSTS_HCHALTED) == 0u)
            break;
        usb_delay_ms(1);
    }
    usb_mmio_wr(g_op + EHCI_OP_USBSTS, 0x3Fu);

    /* One diagnostic line with the raw PORTSC of every port: it is where a
     * bring-up either shows a device or tells you the operational registers are
     * not being read at all (all-zero PORTSC while a device is attached). */
    console_printf("[usb] ehci: usbcmd=%x usbsts=%x hccparams=%x portsc=",
                   (uint64_t)usb_mmio_rd(g_op + EHCI_OP_USBCMD),
                   (uint64_t)usb_mmio_rd(g_op + EHCI_OP_USBSTS),
                   (uint64_t)usb_mmio_rd(EHCI_CAP_HCCPARAMS));
    {
        uint32_t port;
        for (port = 1; port <= g_nports; port++)
            console_printf("%x%c", (uint64_t)usb_port_rd(port),
                           port == g_nports ? '\n' : ' ');
    }

    /* Power ports the firmware left off: a device stays disconnected while
     * PORTSC.PP is clear. */
    {
        uint32_t powered = 0;
        uint32_t port;

        for (port = 1; port <= g_nports; port++) {
            uint32_t psc = usb_port_rd(port);
            if (psc & EHCI_PORTSC_PP)
                continue;
            usb_port_wr(port, (psc & (EHCI_PORTSC_PED))
                              | EHCI_PORTSC_PP | EHCI_PORTSC_RWC);
            powered++;
        }
        if (powered)
            console_printf("[usb] ehci: powered %u port(s)\n",
                           (uint64_t)powered);
    }

    return 0;
}

/* Wait (bounded) for a port to report CCS: after the controller reset the
 * link needs a moment to reflect the device again. */
static int usb_wait_connect(uint32_t port, uint32_t ms)
{
    uint32_t waited = 0;

    while (waited <= ms) {
        if (usb_port_rd(port) & EHCI_PORTSC_CCS)
            return 0;
        usb_delay_ms(10);
        waited += 10;
    }
    return -1;
}

int usb_probe(void)
{
    struct usb_pci_loc loc;
    uint32_t port;
    int port_found = 0;

    if (g_probe_done)
        return g_have_device ? 0 : -1;
    g_probe_done = 1;

    if (usb_pci_find_ehci(&loc) != 0) {
        /* One line only: a machine without USB must keep the boot log it had
         * before this driver existed. */
        console_puts("[usb] no controller\n");
        return -1;
    }
    if (usb_ctrl_setup(&loc) != 0) {
        console_puts("[usb] controller init failed\n");
        return -1;
    }

    for (port = 1; port <= g_nports; port++) {
        uint32_t psc;

        if (usb_wait_connect(port, 200u) != 0)
            continue;                       /* nothing plugged in: normal */
        psc = usb_port_rd(port);
        console_printf("[usb] port %u connected (portsc=%x)\n",
                       (uint64_t)port, (uint64_t)psc);
        port_found = 1;
        if (usb_enum_port(port) != 0)
            continue;

        /* Mass storage found: INQUIRY + READ CAPACITY, then register. */
        if (scsi_inquiry() != 0) {
            console_printf("[usb] INQUIRY failed - not usable storage\n");
            continue;
        }
        console_printf("[usb] inquiry vendor=%s product=%s\n",
                       g_inq_vendor, g_inq_product);
        {
            uint64_t blocks = 0;
            uint32_t bsec = 0;

            if (scsi_read_capacity(&blocks, &bsec) != 0) {
                console_printf("[usb] READ CAPACITY failed\n");
                continue;
            }
            if (bsec != USB_SECTOR_SIZE) {
                console_printf("[usb] block size %u != 512, unsupported\n",
                               (uint64_t)bsec);
                continue;
            }
            g_blocks = blocks;
            g_block_size = bsec;
        }
        g_have_device = 1;
        g_drive = BLK_DRIVE_USB;

        memset(&g_blk, 0, sizeof(g_blk));
        g_blk.name = "usb0";
        g_blk.type = BLK_TYPE_USB;
        g_blk.drive = BLK_DRIVE_USB;
        g_blk.flags = BLK_FLAG_REMOVABLE;   /* read-write, hot-pluggable */
        g_blk.sector_count = g_blocks;
        g_blk.sector_size = USB_SECTOR_SIZE;
        g_blk.block_size = USB_SECTOR_SIZE;
        g_blk.ops = &g_usb_ops;
        g_blk.driver_data = NULL;
        g_blk.next = NULL;

        if (blk_register(&g_blk) != 0) {
            console_printf("[usb] blk_register(drive %d) failed\n",
                           (uint64_t)BLK_DRIVE_USB);
            g_have_device = 0;
            return -1;
        }
        console_printf("[usb] storage: %llu blocks x %u (drive %d, rw)\n",
                       (unsigned long long)g_blocks,
                       (uint64_t)g_block_size, (uint64_t)g_drive);
        return 0;
    }

    if (!port_found)
        console_puts("[usb] no device\n");
    return -1;
}

int usb_present(void)      { return g_have_device ? 1 : 0; }
int usb_drive(void)        { return g_have_device ? g_drive : -1; }
uint64_t usb_blocks(void)  { return g_have_device ? g_blocks : 0; }
uint32_t usb_block_size(void) { return g_have_device ? g_block_size : 0; }

/* ===================== boot self-test ===================== */
void usb_selftest(void)
{
    int read_ok = 0;
    int cat_ok = 0;
    int write_ok = 0;
    char line[128];

    if (!g_have_device) {
        return;
    }

    /* 1) raw block read: BPB/FAT signature in sector 0 (0x55AA at 510..511). */
    memset(g_data, 0, USB_SECTOR_SIZE);
    if (usb_blk_read(&g_blk, 0, 1, g_data) == 0 &&
        g_data[510] == 0x55u && g_data[511] == 0xAAu) {
        read_ok = 1;
        console_printf("[usb] read lba=0 count=1 ok (boot sig 55aa, "
                       "oem=%c%c%c%c%c)\n",
                       (uint64_t)g_data[3], (uint64_t)g_data[4],
                       (uint64_t)g_data[5], (uint64_t)g_data[6],
                       (uint64_t)g_data[7]);
    } else {
        console_puts("[usb] read lba=0 count=1 FAILED (no 55aa signature)\n");
    }

    /* 2) read a known file through the mounted filesystem. */
    {
        extern void *vfs_read_file(const char *path, size_t *len);
        size_t len = 0;
        void *p = vfs_read_file("/mnt/usb/USB.TXT", &len);

        if (p && len > 0) {
            const char *expect = "SutureOS USB selftest";
            uint32_t n = (uint32_t)(len < 120 ? len : 120);
            uint32_t i;
            uint8_t *b = (uint8_t *)p;

            line[0] = 0;
            for (i = 0; i < n && i + 1 < sizeof(line); i++) {
                char c = (char)b[i];
                line[i] = (c == '\n' || c == '\r') ? ' ' : c;
                line[i + 1] = 0;
            }
            if (strncmp((const char *)p, expect, strlen(expect)) == 0) {
                cat_ok = 1;
                console_printf("[usb] cat /mnt/usb/USB.TXT (%llu bytes): \"%s\"\n",
                               (unsigned long long)len, line);
            } else {
                console_printf("[usb] cat /mnt/usb/USB.TXT content mismatch: "
                               "\"%s\"\n", line);
            }
        } else {
            console_puts("[usb] cat /mnt/usb/USB.TXT FAILED (not found)\n");
        }
    }

    /* 3) write path: read the last block, write a pattern, read it back,
     *    restore the original bytes and prove the restore. The image stays
     *    byte-identical, so a host-side whole-image CRC still has to match. */
    if (g_blocks > 0) {
        uint32_t lba = (uint32_t)(g_blocks - 1);
        uint32_t i;

        for (i = 0; i < USB_SECTOR_SIZE; i++)
            g_pat[i] = (uint8_t)(i * 7u + 0x5Au);
        memcpy(g_pat, "SUTUREOS-USB-WRITE-TEST", 23);

        if (usb_blk_read(&g_blk, lba, 1, g_save) != 0) {
            console_printf("[usb] write test: read-back of lba=%u failed\n",
                           (uint64_t)lba);
        } else if (usb_blk_write(&g_blk, lba, 1, g_pat) != 0) {
            console_printf("[usb] write test: WRITE(10) lba=%u failed "
                           "(read-only device?)\n", (uint64_t)lba);
        } else {
            uint8_t *back = (uint8_t *)g_data;
            int same = 0;

            if (usb_blk_read(&g_blk, lba, 1, back) == 0 &&
                memcmp(back, g_pat, USB_SECTOR_SIZE) == 0)
                same = 1;

            /* Always try to restore, even when the compare failed. */
            if (usb_blk_write(&g_blk, lba, 1, g_save) != 0)
                console_printf("[usb] write test: RESTORE of lba=%u FAILED\n",
                               (uint64_t)lba);
            else if (usb_blk_read(&g_blk, lba, 1, back) != 0 ||
                     memcmp(back, g_save, USB_SECTOR_SIZE) != 0)
                console_printf("[usb] write test: restore verify FAILED\n");
            else if (same) {
                write_ok = 1;
                console_printf("[usb] write test PASS (lba=%u, read-back "
                               "byte-for-byte, original restored)\n",
                               (uint64_t)lba);
            } else {
                console_printf("[usb] write test: read-back mismatch at "
                               "lba=%u (restore ok)\n", (uint64_t)lba);
            }
        }
    }

    if (read_ok && cat_ok)
        console_printf("[usb] selftest PASS (read=%d cat=%d)\n",
                       (int64_t)read_ok, (int64_t)cat_ok);
    else
        console_printf("[usb] selftest FAIL (read=%d cat=%d write=%d "
                       "errors=%u)\n",
                       (int64_t)read_ok, (int64_t)cat_ok,
                       (int64_t)write_ok, (uint64_t)g_xfer_errors);
}
