/* SutureOS: Intel e1000 (8254x) driver. See e1000.h for the porting notes. */
#include "e1000.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "mmu.h"

/* ---- PCI config space (same port-I/O method rtl8139.c uses) ---- */
#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu
#define PCI_CFG_VENDOR_ID 0x00u
#define PCI_CFG_COMMAND   0x04u
#define PCI_CFG_BAR0      0x10u
#define PCI_CMD_MEM_SPACE  (1u << 1)
#define PCI_CMD_BUS_MASTER (1u << 2)

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (off & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (off & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, v);
}

/* ---- e1000 register offsets (BAR0, sparse MMIO block) ---- */
#define E1000_CTRL   0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD   0x0014u
#define E1000_IMC    0x00D8u
#define E1000_RCTL   0x0100u
#define E1000_TCTL   0x0400u
#define E1000_RDBAL  0x2800u
#define E1000_RDBAH  0x2804u
#define E1000_RDLEN  0x2808u
#define E1000_RDH    0x2810u
#define E1000_RDT    0x2818u
#define E1000_TDBAL  0x3800u
#define E1000_TDBAH  0x3804u
#define E1000_TDLEN  0x3808u
#define E1000_TDH    0x3810u
#define E1000_TDT    0x3818u
#define E1000_MTA    0x5200u
#define E1000_RAL0   0x5400u
#define E1000_RAH0   0x5404u
#define E1000_GPRC   0x4074u   /* good packets received (diagnostics) */

#define E1000_CTRL_FD    (1u << 0)
#define E1000_CTRL_SLU   (1u << 6)
#define E1000_CTRL_RST   (1u << 26)
#define E1000_STATUS_LU  (1u << 1)
#define E1000_STATUS_FD  (1u << 0)
#define E1000_EERD_START (1u << 0)
#define E1000_EERD_DONE  (1u << 4)
#define E1000_RCTL_EN    (1u << 1)
#define E1000_RCTL_UPE   (1u << 3)
#define E1000_RCTL_BAM   (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_TCTL_EN    (1u << 1)
#define E1000_TCTL_PSP   (1u << 3)

/* ---- rings (legacy 16-byte descriptors; static memory is identity mapped) ---- */
#define E1000_RX_DESCS 32u
#define E1000_TX_DESCS 8u
#define E1000_BUF_SIZE 2048u

#define RX_STATUS_DD 0x01u
#define TX_CMD_EOP   0x01u
#define TX_CMD_IFCS  0x02u
#define TX_CMD_RS    0x08u
#define TX_STATUS_DD 0x01u

typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

static e1000_rx_desc_t g_rx_desc[E1000_RX_DESCS] __attribute__((aligned(4096)));
static e1000_tx_desc_t g_tx_desc[E1000_TX_DESCS] __attribute__((aligned(4096)));
static uint8_t g_rx_buf[E1000_RX_DESCS][E1000_BUF_SIZE] __attribute__((aligned(2048)));
static uint8_t g_tx_buf[E1000_BUF_SIZE] __attribute__((aligned(2048)));

static volatile uint32_t *g_mmio;      /* BAR0, identity mapped */
static uint64_t g_bar0;
static uint8_t  g_mac[6];
static int      g_present;
static uint32_t g_rx_cur;
static uint32_t g_tx_tail;
static uint64_t g_rx_frames;
static uint64_t g_tx_frames;

static uint32_t reg_read(uint32_t off) { return g_mmio[off / 4]; }
static void reg_write(uint32_t off, uint32_t v) { g_mmio[off / 4] = v; }

static uint16_t read_eeprom(uint8_t addr)
{
    reg_write(E1000_EERD, ((uint32_t)addr << 8) | E1000_EERD_START);
    for (uint32_t i = 0; i < 100000u; i++) {
        const uint32_t v = reg_read(E1000_EERD);
        if ((v & E1000_EERD_DONE) != 0) return (uint16_t)((v >> 16) & 0xFFFFu);
    }
    return 0xFFFFu;
}

static void read_mac(void)
{
    const uint16_t w0 = read_eeprom(0);
    const uint16_t w1 = read_eeprom(1);
    const uint16_t w2 = read_eeprom(2);
    g_mac[0] = (uint8_t)(w0 & 0xFF);
    g_mac[1] = (uint8_t)((w0 >> 8) & 0xFF);
    g_mac[2] = (uint8_t)(w1 & 0xFF);
    g_mac[3] = (uint8_t)((w1 >> 8) & 0xFF);
    g_mac[4] = (uint8_t)(w2 & 0xFF);
    g_mac[5] = (uint8_t)((w2 >> 8) & 0xFF);
}

int e1000_present(void) { return g_present; }
uint64_t e1000_bar0(void) { return g_bar0; }
uint64_t e1000_rx_frames(void) { return g_rx_frames; }
uint64_t e1000_tx_frames(void) { return g_tx_frames; }

int e1000_get_mac(uint8_t mac_out[6])
{
    if (!g_present || mac_out == NULL) return -1;
    memcpy(mac_out, g_mac, 6);
    return 0;
}

static int start_rx(void)
{
    for (uint32_t i = 0; i < E1000_RX_DESCS; i++) {
        memset(&g_rx_desc[i], 0, sizeof(g_rx_desc[i]));
        g_rx_desc[i].buffer_addr = (uint64_t)(uintptr_t)g_rx_buf[i];
    }
    memset(g_rx_buf, 0, sizeof(g_rx_buf));

    /* unicast MAC acceptance (RAH0 bit31 = address valid) + clear multicast table */
    reg_write(E1000_RAL0, (uint32_t)g_mac[0] | ((uint32_t)g_mac[1] << 8) |
                          ((uint32_t)g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24));
    reg_write(E1000_RAH0, (uint32_t)g_mac[4] | ((uint32_t)g_mac[5] << 8) | (1u << 31));
    for (uint32_t i = 0; i < 128u; i++) reg_write(E1000_MTA + i * 4u, 0);

    {
        const uint64_t base = (uint64_t)(uintptr_t)g_rx_desc;
        reg_write(E1000_RDBAL, (uint32_t)base);
        reg_write(E1000_RDBAH, (uint32_t)(base >> 32));
    }
    reg_write(E1000_RDLEN, E1000_RX_DESCS * 16u);
    reg_write(E1000_RDH, 0);
    reg_write(E1000_RDT, E1000_RX_DESCS - 1u);   /* sentinel: tail != head */
    g_rx_cur = 0;

    reg_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_SECRC | E1000_RCTL_BAM |
                          E1000_RCTL_UPE);
    return 0;
}

static int start_tx(void)
{
    memset(g_tx_desc, 0, sizeof(g_tx_desc));
    memset(g_tx_buf, 0, sizeof(g_tx_buf));

    {
        const uint64_t base = (uint64_t)(uintptr_t)g_tx_desc;
        reg_write(E1000_TDBAL, (uint32_t)base);
        reg_write(E1000_TDBAH, (uint32_t)(base >> 32));
    }
    reg_write(E1000_TDLEN, E1000_TX_DESCS * 16u);
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);
    g_tx_tail = 0;
    reg_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP);
    return 0;
}

int e1000_init(void)
{
    uint8_t bus, dev, fn;
    int found = 0;
    uint32_t bar0 = 0;
    uint32_t cmd;

    g_present = 0;
    g_mmio = NULL;

    for (bus = 0; bus < 255 && !found; bus++) {
        for (dev = 0; dev < 32 && !found; dev++) {
            for (fn = 0; fn < 8 && !found; fn++) {
                const uint32_t id = pci_cfg_read32(bus, dev, fn, PCI_CFG_VENDOR_ID);
                const uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                const uint16_t device = (uint16_t)((id >> 16) & 0xFFFFu);
                if (vendor == 0xFFFFu) break;                  /* no function */
                if (vendor == 0x8086u &&
                    (device == 0x100Eu || device == 0x100Fu || device == 0x10D3u)) {
                    found = 1;
                    bar0 = pci_cfg_read32(bus, dev, fn, PCI_CFG_BAR0) & ~0xFu;
                    cmd = pci_cfg_read32(bus, dev, fn, PCI_CFG_COMMAND);
                    pci_cfg_write32(bus, dev, fn, PCI_CFG_COMMAND,
                                    cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
                    console_printf("[e1000] PCI cmd 0x%x -> 0x%x\n",
                                   (uint64_t)cmd,
                                   (uint64_t)pci_cfg_read32(bus, dev, fn, PCI_CFG_COMMAND));
                    console_printf("[e1000] found at %u:%u.%u dev=0x%x BAR0=0x%x\n",
                                   (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
                                   (uint64_t)device, (uint64_t)bar0);
                }
            }
        }
    }
    if (!found) return E1000_ERR_NOT_FOUND;
    if (bar0 == 0) return E1000_ERR_BAR;

    /* The whole 128 KiB register window has to be mapped (BAR0 often sits below
     * 4 GiB and outside the boot identity map). */
    if (mmu_map_mmio_identity(bar0, 0x20000) != 0) {
        console_puts("[e1000] cannot map BAR0\n");
        return E1000_ERR_BAR;
    }
    g_mmio = (volatile uint32_t *)(uintptr_t)bar0;
    g_bar0 = bar0;

    /* reset (self-clearing) */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | E1000_CTRL_RST);
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((reg_read(E1000_CTRL) & E1000_CTRL_RST) == 0) break;
    }
    reg_write(E1000_IMC, 0xFFFFFFFFu);                  /* polled, no interrupts */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_FD);

    read_mac();
    start_rx();
    start_tx();

    g_present = 1;
    console_printf("[  OK  ] e1000: MAC=%x:%x:%x:%x:%x:%x link=%u fd=%u RCTL=0x%x\n",
                   (uint64_t)g_mac[0], (uint64_t)g_mac[1], (uint64_t)g_mac[2],
                   (uint64_t)g_mac[3], (uint64_t)g_mac[4], (uint64_t)g_mac[5],
                   (uint64_t)((reg_read(E1000_STATUS) & E1000_STATUS_LU) ? 1u : 0u),
                   (uint64_t)((reg_read(E1000_STATUS) & E1000_STATUS_FD) ? 1u : 0u),
                   (uint64_t)reg_read(E1000_RCTL));
    return E1000_ERR_NONE;
}

int e1000_send_frame(const void *frame, uint32_t len)
{
    e1000_tx_desc_t *d;
    const uint32_t slot = g_tx_tail;

    if (!g_present || frame == NULL) return -1;
    if (len > E1000_BUF_SIZE) return -1;

    memcpy(g_tx_buf, frame, len);
    d = &g_tx_desc[slot];
    d->buffer_addr = (uint64_t)(uintptr_t)g_tx_buf;
    d->length = (uint16_t)len;
    d->cso = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;

    g_tx_tail = (slot + 1u) % E1000_TX_DESCS;
    reg_write(E1000_TDT, g_tx_tail);                    /* doorbell */

    /* bounded wait for completion so the single TX buffer can be reused */
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((d->status & TX_STATUS_DD) != 0) break;
    }
    g_tx_frames++;
    return 0;
}

int e1000_receive(void (*fn)(const uint8_t *frame, uint32_t len))
{
    int delivered = 0;
    static uint32_t idle_dumps;

    if (!g_present || fn == NULL) return 0;

    /* THE QEMU QUIRK (ported from Cinux): reading a MMIO register makes the device
     * model run its main loop, which is what actually moves received packets into
     * our descriptor ring. Without this read a polled driver never sees a frame. */
    (void)reg_read(E1000_RDH);

    for (;;) {
        e1000_rx_desc_t *d = &g_rx_desc[g_rx_cur];
        uint32_t len;
        if ((d->status & RX_STATUS_DD) == 0) break;
        len = d->length;
        if (len > E1000_BUF_SIZE) len = E1000_BUF_SIZE;
        if ((d->errors & 0x7u) == 0 && len >= 14u) {    /* no CE/SE/SEQ errors */
            fn(g_rx_buf[g_rx_cur], len);
            g_rx_frames++;
            delivered++;
        }
        d->status = 0;
        d->length = 0;
        d->buffer_addr = (uint64_t)(uintptr_t)g_rx_buf[g_rx_cur];
        g_rx_cur = (g_rx_cur + 1u) % E1000_RX_DESCS;
        reg_write(E1000_RDT, (g_rx_cur == 0u) ? (E1000_RX_DESCS - 1u) : (g_rx_cur - 1u));
    }
    if (delivered == 0 && idle_dumps < 2 && g_tx_frames > 0) {
        idle_dumps++;
        console_printf("[e1000] idle: GPRC=%llu RDH=%llu RDT=%llu d0=0x%x "
                       "TDH=%llu TDT=%llu txd0=0x%x TCTL=0x%x RCTL=0x%x RDBAL=0x%x TDBAL=0x%x\n",
                       (uint64_t)reg_read(E1000_GPRC), (uint64_t)reg_read(E1000_RDH),
                       (uint64_t)reg_read(E1000_RDT), (uint64_t)g_rx_desc[0].status,
                       (uint64_t)reg_read(E1000_TDH), (uint64_t)reg_read(E1000_TDT),
                       (uint64_t)g_tx_desc[0].status,
                       (uint64_t)reg_read(E1000_TCTL), (uint64_t)reg_read(E1000_RCTL),
                       (uint64_t)reg_read(E1000_RDBAL), (uint64_t)reg_read(E1000_TDBAL));
    }
    return delivered;
}
