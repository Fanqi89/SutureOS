/* SutureOS module: Intel High Definition Audio (HDA) controller + codec driver.
 * ============================================================================
 * Source / provenance
 *   Ported from: PlantOS - src/kernel/drivers/sound/hda.c (MIT,
 *                Copyright (c) 2024 plos-clan)
 *                local copy: C:/Users/fanqi/Desktop/自研操作系统/PlantOS/src/
 *                kernel/drivers/sound/hda.c
 *   The reference is MIT and SutureOS is GPL-3.0, so the port is license
 *   compatible.  It was used as the programming-model reference for: PCI
 *   discovery of the Intel HDA controller (class 0x04/0x03 and the known
 *   8086:2668/27D8/293E ids), the 0x80-based stream-descriptor layout, the
 *   CORB/RIRB verb-ring transport and the "walk the Audio Function Group
 *   widgets, then find the output pin and its converter" enumeration idea.
 *   Register ordering, the verb encoding, stream-reset handshakes and the
 *   failure criteria were cross-checked against the Intel HDA 1.0a
 *   specification and the Apache-2.0 ReliefOS HDA design/handoff notes (design
 *   reference only, no code copied).
 *   Everything below is written for SutureOS; differences from the reference:
 *     1. adapted to SutureOS APIs - <stitch/io.h> port I/O, MMIO accessors,
 *        console_printf, static .bss DMA memory (flat identity map:
 *        physical == virtual), no kernel heap, no page_alloc, no interrupts;
 *     2. the PCI probe scans bus 0 (the HDA link is part of the chipset),
 *        accepts a 32- or 64-bit BAR0 and maps it with
 *        mmu_map_mmio_identity() because the BAR is outside the boot identity
 *        map (QEMU puts it at 0xfebf0000, i.e. above 1 GiB);
 *     3. the controller reset follows the GCTL.CRST handshake and the
 *        CORB/RIRB rings follow the spec order (CORBRP reset acknowledge,
 *        RIRBWP reset, then DMA enable), instead of raw register writes;
 *     4. verbs use the correct payload encoding: 12-bit verbs (GET_PARAMETER
 *        0xF00, SET_CHANNEL_STREAM 0x706, SET_PIN_WIDGET_CTL 0x707, ...) with
 *        an 8-bit payload, and the 16-bit-payload verbs (SET_STREAM_FORMAT
 *        0x200, SET_AMP_GAIN_MUTE 0x300) with the payload in bits 15:0;
 *     5. the codec enumeration resolves pin -> (mixer/selector) -> converter
 *        through the connection list instead of assuming a fixed widget
 *        order, powers the path to D0 and opens every output amp at 0 dB;
 *     6. the output stream-descriptor index is derived from GCAP plus a
 *        read-only probe of the implemented descriptor blocks (QEMU reports
 *        GCAP=0x4401 for 8 implemented blocks), which avoids the classic
 *        "programmed an input stream as playback" silence failure;
 *     7. playback completion is polled from SDLPIB (wrap detection) and
 *        bounded by the 8254 PIT counter, because interrupts are off during
 *        the boot self-test; no IRQ handler is installed.
 * ============================================================================
 */

#include "hda.h"

#include <stitch/console.h>
#include <stitch/io.h>
#include <stitch/string.h>
#include <stitch/types.h>

#include "mb_pit.h"   /* 8254 counter: wall-clock bound without interrupts */
#include "mmu.h"      /* mmu_map_mmio_identity() for the MMIO BAR          */

/* ===================== PCI config space (mechanism #1) =====================
 * Same accessor pair as ac97.c/rtl8139.c/e1000.c: 0xCF8 must be written as a
 * DWORD; a byte-wise write does not take effect on QEMU. */
#define HDA_PCI_CFG_ADDR   0xCF8u
#define HDA_PCI_CFG_DATA   0xCFCu

#define HDA_CFG_VENDOR_ID  0x00u
#define HDA_CFG_COMMAND    0x04u
#define HDA_CFG_REV_CLASS  0x08u
#define HDA_CFG_HEADER     0x0Cu
#define HDA_CFG_BAR0       0x10u
#define HDA_CFG_BAR1       0x14u

#define HDA_PCI_CMD_MEM_SPACE  (1u << 1)
#define HDA_PCI_CMD_BUS_MASTER (1u << 2)

#define HDA_PCI_VENDOR_INTEL   0x8086u

/* Known Intel HDA controller ids (ICH6 ... 100 series); 0x2668 is what QEMU's
 * "-device intel-hda" reports. */
static const uint16_t g_hda_device_ids[] = {
    0x2668u,   /* ICH6 / ICH6R                                            */
    0x27D8u,   /* ICH7 family                                             */
    0x284Bu,   /* ICH8 family                                             */
    0x293Eu,   /* ICH9 family                                             */
    0x3A3Eu,   /* ICH10 family                                            */
    0x3B56u,   /* 5 series / Ibex Peak                                    */
    0x811Bu,   /* US15W / Poulsbo                                         */
    0x8C20u,   /* 8 series / Lynx Point                                   */
    0x9C20u,   /* 9 series                                                */
    0xA170u    /* 100 series / Sunrise Point                              */
};

struct hda_pci_loc {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
};

static uint32_t hda_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(HDA_PCI_CFG_ADDR, addr);
    return inl(HDA_PCI_CFG_DATA);
}

static void hda_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                            uint32_t value)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(HDA_PCI_CFG_ADDR, addr);
    outl(HDA_PCI_CFG_DATA, value);
}

static int hda_is_known_device(uint16_t device)
{
    for (unsigned i = 0;
         i < sizeof(g_hda_device_ids) / sizeof(g_hda_device_ids[0]); i++) {
        if (g_hda_device_ids[i] == device) return 1;
    }
    return 0;
}

/* Find the controller: a known Intel HDA id first, else the generic
 * multimedia/audio (class 0x04, subclass 0x03) function.  Only bus 0 is
 * walked: the HDA link is part of the chipset, so a machine without HDA pays
 * 32 config reads and no more. */
static int hda_pci_find(struct hda_pci_loc *out)
{
    struct hda_pci_loc fallback;
    int have_fallback = 0;

    for (uint32_t dev = 0; dev < 32u; dev++) {
        uint32_t id = hda_pci_read32(0, (uint8_t)dev, 0, HDA_CFG_VENDOR_ID);
        uint32_t hdr, fn_max;

        if ((id & 0xFFFFu) == 0xFFFFu || (id & 0xFFFFu) == 0x0000u)
            continue;                                   /* no device in slot */

        hdr = hda_pci_read32(0, (uint8_t)dev, 0, HDA_CFG_HEADER);
        fn_max = (hdr & 0x00800000u) ? 8u : 1u;         /* multi-function bit */

        for (uint32_t fn = 0; fn < fn_max; fn++) {
            uint32_t cc, vendor, device;

            if (fn != 0u) {
                id = hda_pci_read32(0, (uint8_t)dev, (uint8_t)fn,
                                    HDA_CFG_VENDOR_ID);
                if ((id & 0xFFFFu) == 0xFFFFu) continue;
            }
            vendor = id & 0xFFFFu;
            device = (id >> 16) & 0xFFFFu;

            if (vendor == HDA_PCI_VENDOR_INTEL &&
                hda_is_known_device((uint16_t)device)) {
                out->bus = 0;
                out->dev = (uint8_t)dev;
                out->fn = (uint8_t)fn;
                return 0;
            }

            cc = hda_pci_read32(0, (uint8_t)dev, (uint8_t)fn, HDA_CFG_REV_CLASS);
            if (!have_fallback &&
                ((cc >> 24) & 0xFFu) == 0x04u &&        /* multimedia         */
                ((cc >> 16) & 0xFFu) == 0x03u) {        /* audio device (HDA) */
                fallback.bus = 0;
                fallback.dev = (uint8_t)dev;
                fallback.fn = (uint8_t)fn;
                have_fallback = 1;
            }
        }
    }

    if (have_fallback) {
        *out = fallback;
        return 0;
    }
    return -1;
}

/* ===================== controller register map ============================
 * Intel HDA 1.0a section 3.3; the controller registers live in the first
 * 0x80 bytes of BAR0, stream descriptors start at 0x80 (input streams first,
 * then output streams) and are 0x20 bytes each. */
#define HDA_REG_GCAP        0x00u   /* u16: global capabilities              */
#define HDA_REG_VMIN        0x02u
#define HDA_REG_VMAJ        0x03u
#define HDA_REG_GCTL        0x08u   /* u32: bit0 CRST (controller reset)     */
#define HDA_REG_WAKEEN      0x0Cu
#define HDA_REG_STATESTS    0x0Eu   /* u16: codec state-change status (W1C)  */
#define HDA_REG_GSTS        0x10u
#define HDA_REG_INTCTL      0x20u   /* u32: global interrupt enable          */
#define HDA_REG_INTSTS      0x24u   /* u32: global interrupt status          */
#define HDA_REG_WALLCLK     0x30u
#define HDA_REG_CORBLBASE   0x40u   /* u32: command ring base (low)          */
#define HDA_REG_CORBUBASE   0x44u   /* u32: command ring base (high)         */
#define HDA_REG_CORBWP      0x48u   /* u16: command ring write pointer       */
#define HDA_REG_CORBRP      0x4Au   /* u16: command ring read pointer        */
#define HDA_REG_CORBCTL     0x4Cu   /* u8 : bit1 CORBDMAEN                   */
#define HDA_REG_CORBSTS     0x4Du   /* u8 : W1C                              */
#define HDA_REG_CORBSIZE    0x4Eu   /* u8 : bits1:0 size, bits7:4 supported  */
#define HDA_REG_RIRBLBASE   0x50u   /* u32: response ring base (low)         */
#define HDA_REG_RIRBUBASE   0x54u   /* u32: response ring base (high)        */
#define HDA_REG_RIRBWP      0x58u   /* u16: response ring write pointer      */
#define HDA_REG_RINTCNT     0x5Au   /* u16: response interrupt count         */
#define HDA_REG_RIRBCTL     0x5Cu   /* u8 : bit1 RIRBDMAEN, bit0 RINTCTL    */
#define HDA_REG_RIRBSTS     0x5Du   /* u8 : W1C (bit0 RINTFL, bit2 OIS)      */
#define HDA_REG_RIRBSIZE    0x5Eu   /* u8 : bits1:0 size, bits7:4 supported  */

/* Stream descriptor (0x20 bytes), offsets from the descriptor base. */
#define HDA_SD_CTL          0x00u   /* u32: bits1:0 SRST, bit2 RUN,          */
                                    /*      bits23:20 stream tag             */
#define HDA_SD_STS          0x03u   /* u8 : W1C BCIS2/FIFOE3/DESE4, FIFORDY5 */
#define HDA_SD_LPIB         0x04u   /* u32: link position inside the buffer  */
#define HDA_SD_CBL          0x08u   /* u32: cyclic buffer length (bytes)     */
#define HDA_SD_LVI          0x0Cu   /* u16: last valid descriptor index      */
#define HDA_SD_FIFOSIZE     0x10u   /* u16: FIFO size (RO, 0 = no block)     */
#define HDA_SD_FMT          0x12u   /* u16: stream format                    */
#define HDA_SD_BDPL         0x18u   /* u32: BDL base (low)                   */
#define HDA_SD_BDPU         0x1Cu   /* u32: BDL base (high)                  */

#define HDA_SD_CTL_SRST      0x00000001u
#define HDA_SD_CTL_RUN       0x00000002u   /* SD_CTL_DMA_START */
#define HDA_SD_CTL_TAG(t)    ((((uint32_t)(t)) & 0xFu) << 20)
#define HDA_SD_STS_CLEAR     0x1Cu    /* BCIS|FIFOE|DESE                     */
#define HDA_SD_STS_ERROR     0x18u    /* FIFOE|DESE                          */

#define HDA_SD_BASE          0x80u
#define HDA_SD_STRIDE        0x20u

/* ===================== HDA verbs =========================================
 * Command word: bits 31:28 codec address, 27:20 node id, 19:8 verb id, 7:0
 * payload.  The verbs that need a 16-bit payload (SET_STREAM_FORMAT 0x200,
 * SET_AMP_GAIN_MUTE 0x300) use the lower 16 bits for it, which the same
 * expression produces because their verb numbers already occupy bits 19:16. */
#define HDA_VERB_GET_PARAM          0xF00u
#define HDA_VERB_GET_CONN_LIST      0xF02u
#define HDA_VERB_GET_CFG_DEFAULT    0xF1Cu
#define HDA_VERB_SET_STREAM_FORMAT  0x200u
#define HDA_VERB_SET_AMP_GAIN_MUTE  0x300u
#define HDA_VERB_SET_CONNECT_SEL    0x701u
#define HDA_VERB_SET_POWER_STATE    0x705u
#define HDA_VERB_SET_CHANNEL_STREAM 0x706u
#define HDA_VERB_SET_PIN_WIDGET_CTL 0x707u
#define HDA_VERB_SET_EAPD_BTL       0x70Cu

/* GET_PARAMETER ids. */
#define HDA_PARAM_VENDOR_ID         0x00u
#define HDA_PARAM_SUBSYSTEM_ID      0x01u
#define HDA_PARAM_REVISION_ID       0x02u
#define HDA_PARAM_SUBNODE_COUNT     0x04u
#define HDA_PARAM_FUNCTION_TYPE     0x05u
#define HDA_PARAM_WIDGET_CAP        0x09u
#define HDA_PARAM_PCM_CAP           0x0Au
#define HDA_PARAM_PIN_CAP           0x0Cu
#define HDA_PARAM_CONN_LIST_LEN     0x0Eu
#define HDA_PARAM_OUT_AMP_CAP       0x12u

/* Widget types (AUDIO_WIDGET_CAP bits 3:0). */
#define HDA_WIDGET_AUDIO_OUTPUT   0x0u
#define HDA_WIDGET_MIXER          0x2u
#define HDA_WIDGET_SELECTOR       0x3u
#define HDA_WIDGET_PIN_COMPLEX    0x4u

/* Pin capability bit 4 = output capable. */
#define HDA_PINCAP_OUTPUT         (1u << 4)
/* Pin control bit 6 = OUT_ENABLE, bit 7 = HP amp enable. */
#define HDA_PINCTL_OUT_EN         (1u << 6)
#define HDA_PINCTL_HP_EN          (1u << 7)

/* GET_PARAMETER answers that cannot be a real capability value. */
#define HDA_PARAM_INVALID 0xFFFFFFFFu

/* Rings.  256 entries is the largest CORB/RIRB size; the accepted size is
 * read back from CORBSIZE/RIRBSIZE. */
#define HDA_RING_MAX 256u
#define HDA_VERB_TIMEOUT_MS 100u

/* Staging BDL: 1.0 s of 48 kHz 16-bit stereo. */
#define HDA_BDL_ENTRIES 4u

/* How long the stream keeps running on silence after the tone has been
 * consumed, so the codec FIFO can flush its last samples into the backend. */
#define HDA_DRAIN_MS 600u

/* ===================== DMA memory ========================================
 * Static .bss: the kernel runs in a flat identity map, so the physical address
 * of a symbol is simply its virtual address.  CORB/RIRB/BDL must be 128-byte
 * aligned (HDA spec); every descriptor's buffer start must be 128-byte
 * aligned too, which the chunking below guarantees. */
static uint32_t g_corb[HDA_RING_MAX] __attribute__((aligned(128)));
static uint32_t g_rirb[HDA_RING_MAX * 2u] __attribute__((aligned(128)));
static uint8_t  g_bdl[HDA_BDL_ENTRIES * 16u] __attribute__((aligned(128)));
static int16_t  g_pcm[HDA_MAX_FRAMES * 2u] __attribute__((aligned(128)));

static volatile uint8_t *g_mmio;
static char g_name[160];

/* 0 = unprobed, 1 = ready, -1 = no controller, -2 = controller failed. */
static int g_state;
static uint8_t g_codec;
static uint8_t g_afg;
static uint8_t g_dac;
static uint8_t g_pin;
static uint8_t g_stream_tag = 1u;
static uint32_t g_gcap;
static uint32_t g_sd_blocks;
static uint32_t g_num_in_sd;
static uint32_t g_sd_base;

/* Best resolved path: nodes[0] = pin, nodes[len-1] = converter. */
#define HDA_PATH_MAX 8u
static uint8_t g_path_nodes[HDA_PATH_MAX];
static uint8_t g_path_sels[HDA_PATH_MAX];
static uint8_t g_path_len;

static uint16_t g_corb_entries;
static uint16_t g_rirb_entries;
static uint16_t g_corb_wp;
static uint16_t g_rirb_last_wp;
static uint8_t g_rirb_entry_shift;   /* 0 = response at WP, 1 = at WP-1     */

/* ===================== MMIO accessors ==================================== */
static inline uint8_t hda_rd8(uint32_t off)
{
    return *(volatile uint8_t *)(g_mmio + off);
}

static inline uint16_t hda_rd16(uint32_t off)
{
    return *(volatile uint16_t *)(g_mmio + off);
}

static inline uint32_t hda_rd32(uint32_t off)
{
    return *(volatile uint32_t *)(g_mmio + off);
}

static inline void hda_wr8(uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(g_mmio + off) = v;
}

static inline void hda_wr16(uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(g_mmio + off) = v;
}

static inline void hda_wr32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(g_mmio + off) = v;
}

/* ===================== timing ============================================
 * The 8254 channel-0 counter is the only wall clock available at this point:
 * interrupts are not used for audio and the boot self-test runs before the
 * scheduler.  Mode 3 decrements the count by 2 per input clock, so the raw
 * delta is not a time base; the number of counter *reloads* (value jumping
 * back up) is counted instead, exactly as ac97.c does. */
static uint16_t g_pit_last;
static uint32_t g_pit_reloads;
static uint32_t g_pit_period_ms;

static void hda_clock_begin(void)
{
    uint32_t hz = mbos_pit_hz();
    g_pit_period_ms = (hz != 0u) ? (1000u / hz) : 10u;
    if (g_pit_period_ms == 0u) g_pit_period_ms = 1u;
    g_pit_last = mbos_pit_counter();
    g_pit_reloads = 0u;
}

static uint32_t hda_clock_ms(void)
{
    const uint16_t now = mbos_pit_counter();
    if (now > g_pit_last) g_pit_reloads++;
    g_pit_last = now;
    return g_pit_reloads * g_pit_period_ms;
}

static void hda_delay_loops(uint32_t loops)
{
    while (loops--) __asm__ volatile("pause");
}

/* Wait for a 32-bit register field to reach `want`; -1 = timeout. */
static int hda_wait32(uint32_t off, uint32_t mask, uint32_t want,
                      uint32_t timeout_ms)
{
    uint32_t spins = 0u;

    hda_clock_begin();
    for (;;) {
        if ((hda_rd32(off) & mask) == want) return 0;
        if (++spins >= 256u) {
            spins = 0u;
            if (hda_clock_ms() >= timeout_ms) return -1;
        }
    }
}

static int hda_wait16(uint32_t off, uint16_t mask, uint16_t want,
                      uint32_t timeout_ms)
{
    uint32_t spins = 0u;

    hda_clock_begin();
    for (;;) {
        if ((hda_rd16(off) & mask) == want) return 0;
        if (++spins >= 256u) {
            spins = 0u;
            if (hda_clock_ms() >= timeout_ms) return -1;
        }
    }
}

/* ===================== command ring (CORB/RIRB) ========================== */
static int hda_ring_sizes(void)
{
    const uint8_t corb = hda_rd8(HDA_REG_CORBSIZE);
    const uint8_t rirb = hda_rd8(HDA_REG_RIRBSIZE);

    /* Bits 7:4 report the supported sizes (bit4=2, bit5=16, bit6=256); pick
     * the largest one the controller supports and program bits 1:0. */
    if (corb & 0x40u)      { g_corb_entries = 256u; hda_wr8(HDA_REG_CORBSIZE, 0x02u); }
    else if (corb & 0x20u) { g_corb_entries = 16u;  hda_wr8(HDA_REG_CORBSIZE, 0x01u); }
    else if (corb & 0x10u) { g_corb_entries = 2u;   hda_wr8(HDA_REG_CORBSIZE, 0x00u); }
    else return -1;

    if (rirb & 0x40u)      { g_rirb_entries = 256u; hda_wr8(HDA_REG_RIRBSIZE, 0x02u); }
    else if (rirb & 0x20u) { g_rirb_entries = 16u;  hda_wr8(HDA_REG_RIRBSIZE, 0x01u); }
    else if (rirb & 0x10u) { g_rirb_entries = 2u;   hda_wr8(HDA_REG_RIRBSIZE, 0x00u); }
    else return -1;

    return 0;
}

static int hda_ring_init(void)
{
    if (hda_ring_sizes() != 0) return -1;

    /* CORB: base, reset the read pointer (write 8000h, wait for the
     * controller to acknowledge bit15, clear it), write pointer to 0, then
     * enable the DMA engine. */
    hda_wr32(HDA_REG_CORBLBASE, (uint32_t)(uintptr_t)g_corb);
    hda_wr32(HDA_REG_CORBUBASE, 0u);
    hda_wr16(HDA_REG_CORBWP, 0u);
    hda_wr16(HDA_REG_CORBRP, 0x8000u);
    if (hda_wait16(HDA_REG_CORBRP, 0x8000u, 0x8000u, HDA_VERB_TIMEOUT_MS) != 0)
        return -1;
    hda_wr16(HDA_REG_CORBRP, 0x0000u);
    if (hda_wait16(HDA_REG_CORBRP, 0x8000u, 0x0000u, HDA_VERB_TIMEOUT_MS) != 0)
        return -1;
    hda_wr8(HDA_REG_CORBSTS, 0x01u);          /* clear stale CMEI             */
    hda_wr8(HDA_REG_CORBCTL, 0x02u);          /* CORBDMAEN                    */

    /* RIRB: base, reset the write pointer (write 8000h), clear status, enable
     * DMA. */
    hda_wr32(HDA_REG_RIRBLBASE, (uint32_t)(uintptr_t)g_rirb);
    hda_wr32(HDA_REG_RIRBUBASE, 0u);
    hda_wr16(HDA_REG_RIRBWP, 0x8000u);
    /* RINTCNT is the number of responses per interrupt batch.  The controller
     * stops consuming CORB entries while the number of responses in the
     * current batch equals RINTCNT, so software has to acknowledge batches:
     * use a batch size of 1 and enable the response interrupt *status* bit,
     * then write-1-clear RIRBSTS after every response (the ack in hda_verb()).
     * Interrupt *delivery* stays off through INTCTL=0, so this remains a
     * polled driver - the status bit is only the batch handshake. */
    hda_wr16(HDA_REG_RINTCNT, 0x0001u);
    hda_wr8(HDA_REG_RIRBSTS, 0x05u);          /* RINTFL|OIS (W1C)             */
    hda_wr8(HDA_REG_RIRBCTL, 0x03u);          /* RINTCTL|RIRBDMAEN            */

    memset(g_corb, 0, sizeof(g_corb));
    memset(g_rirb, 0, sizeof(g_rirb));
    g_corb_wp = 0u;
    g_rirb_last_wp = 0u;
    g_rirb_entry_shift = 0u;
    return 0;
}

/* Send one verb and wait for its response.
 * The controller writes the response and advances RIRBWP; whether the answer
 * of interest sits at the new WP or at WP-1 is implementation dependent, so
 * the first successful GET_PARAMETER calibrates g_rirb_entry_shift. */
static int hda_verb(uint8_t codec, uint8_t nid, uint16_t verb, uint16_t payload,
                    uint32_t *resp, int calibrate)
{
    const uint32_t cmd = ((uint32_t)codec << 28) | ((uint32_t)nid << 20)
                       | ((uint32_t)verb << 8) | (uint32_t)payload;
    uint16_t idx;
    uint32_t spins = 0u;
    uint32_t value = HDA_PARAM_INVALID;

    /* CORBWP points at the last command handed to the controller, so the next
     * command always goes to WP+1: after a reset WP is 0 and entry 0 is never
     * used (writing WP=0 again would not signal a new command). */
    g_corb_wp = (uint16_t)((g_corb_wp + 1u) & (uint16_t)(g_corb_entries - 1u));
    idx = g_corb_wp;
    g_corb[idx] = cmd;
    __asm__ volatile("mfence" ::: "memory");
    hda_wr16(HDA_REG_CORBWP, idx);

    hda_clock_begin();
    for (;;) {
        const uint16_t wp = (uint16_t)(hda_rd16(HDA_REG_RIRBWP) & 0xFFu);

        if (wp != g_rirb_last_wp) {
            uint32_t entry = (uint32_t)wp;

            if (g_rirb_entry_shift != 0u)
                entry = (uint32_t)((wp - 1u) & 0xFFu);
            value = g_rirb[entry * 2u];

            if (calibrate && (value == 0u || value == HDA_PARAM_INVALID)) {
                /* Try the other convention once and remember it. */
                const uint32_t alt = (uint32_t)((wp + 1u) & 0xFFu);
                const uint32_t alt_value = g_rirb[alt * 2u];
                if (alt_value != 0u && alt_value != HDA_PARAM_INVALID) {
                    value = alt_value;
                    g_rirb_entry_shift = 1u;
                }
            }

            g_rirb_last_wp = wp;
            /* Acknowledge this response batch (W1C RINTFL): the controller
             * resets its batch counter and resumes CORB processing. */
            hda_wr8(HDA_REG_RIRBSTS, 0x01u);
            if (resp) *resp = value;
            return 0;
        }
        if (++spins >= 256u) {
            spins = 0u;
            if (hda_clock_ms() >= HDA_VERB_TIMEOUT_MS) return -1;
        }
    }
}

static uint32_t hda_param(uint8_t codec, uint8_t nid, uint16_t param)
{
    uint32_t v = HDA_PARAM_INVALID;
    if (hda_verb(codec, nid, HDA_VERB_GET_PARAM, param, &v, 0) != 0)
        return HDA_PARAM_INVALID;
    return v;
}

static uint32_t hda_cfg_default(uint8_t codec, uint8_t nid)
{
    uint32_t v = HDA_PARAM_INVALID;
    if (hda_verb(codec, nid, HDA_VERB_GET_CFG_DEFAULT, 0u, &v, 0) != 0)
        return HDA_PARAM_INVALID;
    return v;
}

static int hda_widget_type(uint8_t codec, uint8_t nid)
{
    const uint32_t cap = hda_param(codec, nid, HDA_PARAM_WIDGET_CAP);
    if (cap == HDA_PARAM_INVALID) return -1;
    return (int)((cap >> 20) & 0x0Fu);   /* AC_WCAP_TYPE */
}

/* Number of connection-list entries of a widget (-1 = not available). */
static int hda_conn_len(uint8_t codec, uint8_t nid)
{
    const uint32_t raw = hda_param(codec, nid, HDA_PARAM_CONN_LIST_LEN);
    int n;

    if (raw == HDA_PARAM_INVALID) return -1;
    n = (int)(raw & 0x7Fu);
    if (n > 32) n = 32;
    return n;
}

/* Read one connection-list entry.  Short form: 4 NIDs per response, long form
 * (bit7 of the length answer): 2 NIDs per response. 0 = ok. */
static int hda_conn_entry(uint8_t codec, uint8_t nid, uint32_t index,
                          uint8_t *out_nid)
{
    const uint32_t raw = hda_param(codec, nid, HDA_PARAM_CONN_LIST_LEN);
    const int long_form = (raw != HDA_PARAM_INVALID) && ((raw & 0x80u) != 0u);
    const uint32_t base = long_form ? ((index / 2u) * 2u) : ((index / 4u) * 4u);
    const uint32_t shift = long_form ? ((index % 2u) * 16u) : ((index % 4u) * 8u);
    uint32_t v = HDA_PARAM_INVALID;

    if (hda_verb(codec, nid, HDA_VERB_GET_CONN_LIST, (uint16_t)base, &v, 0) != 0)
        return -1;
    *out_nid = long_form ? (uint8_t)((v >> shift) & 0xFFFFu)
                         : (uint8_t)((v >> shift) & 0xFFu);
    return 0;
}

/* ===================== codec bring-up ==================================== */

/* Resolve pin -> [mixer/selector ...] -> converter.  nodes[0] is the pin and
 * nodes[len-1] the converter; sels[k] is the connection-select index to use on
 * nodes[k] to reach nodes[k+1]. */
static int hda_path_dfs(uint8_t codec, uint8_t *nodes, uint8_t *sels,
                        uint8_t *len, uint8_t k, uint8_t depth)
{
    const int n = hda_conn_len(codec, nodes[k]);

    if (n <= 0 || depth >= 4u) return -1;
    for (int i = 0; i < n; i++) {
        uint8_t child = 0u;
        int type;

        if (hda_conn_entry(codec, nodes[k], (uint32_t)i, &child) != 0) continue;
        type = hda_widget_type(codec, child);
        if (type < 0) continue;
        if (k + 2u > HDA_PATH_MAX) return -1;

        if (type == HDA_WIDGET_AUDIO_OUTPUT) {
            sels[k] = (uint8_t)i;
            nodes[k + 1u] = child;
            *len = (uint8_t)(k + 2u);
            return 0;
        }
        if (type == HDA_WIDGET_MIXER || type == HDA_WIDGET_SELECTOR) {
            sels[k] = (uint8_t)i;
            nodes[k + 1u] = child;
            if (hda_path_dfs(codec, nodes, sels, len, (uint8_t)(k + 1u),
                             (uint8_t)(depth + 1u)) == 0)
                return 0;
        }
    }
    return -1;
}

/* Open every output amp on the path at 0 dB, unmuted. */
static void hda_open_amps(uint8_t codec, const uint8_t *nodes, uint8_t len)
{
    for (uint8_t k = 0; k < len; k++) {
        const uint32_t cap = hda_param(codec, nodes[k], HDA_PARAM_OUT_AMP_CAP);
        uint32_t payload;

        if (cap == HDA_PARAM_INVALID || cap == 0u) continue;
        /* AMP SET: bit15 output, bit13 left, bit12 right, bits11:8 index,
         * bit7 mute, bits6:0 gain.  The 0 dB gain is the capability offset. */
        payload = 0x8000u | 0x2000u | 0x1000u | (cap & 0x7Fu);
        (void)hda_verb(codec, nodes[k], HDA_VERB_SET_AMP_GAIN_MUTE,
                       (uint16_t)payload, NULL, 0);
    }
}

static void hda_power_d0(uint8_t codec, uint8_t nid)
{
    (void)hda_verb(codec, nid, HDA_VERB_SET_POWER_STATE, 0x0000u, NULL, 0);
}

/* Program the converter and the pin of a resolved path. */
static int hda_program_path(uint8_t codec, const uint8_t *nodes,
                            const uint8_t *sels, uint8_t len)
{
    const uint8_t dac = nodes[len - 1u];
    const uint8_t pin = nodes[0];
    const uint32_t pcm = hda_param(codec, dac, HDA_PARAM_PCM_CAP);
    const uint32_t pincap = hda_param(codec, pin, HDA_PARAM_PIN_CAP);
    const uint32_t cfg = hda_cfg_default(codec, pin);
    uint32_t pin_ctl = HDA_PINCTL_OUT_EN;

    /* 48 kHz is rate bit 6 of PCM_CAP; 16-bit is sample-size bit 17. */
    if (pcm != HDA_PARAM_INVALID && pcm != 0u &&
        (((pcm & (1u << 6)) == 0u) || ((pcm & (1u << 17)) == 0u))) {
        console_printf("[hda] converter nid=%u does not support 48 kHz/16-bit "
                       "(pcm_cap=0x%x)\n",
                       (uint64_t)dac, (uint64_t)pcm);
        return HDA_ERR_IO;
    }

    for (uint8_t k = 0; k + 1u < len; k++) {
        if (hda_widget_type(codec, nodes[k]) == HDA_WIDGET_SELECTOR) {
            (void)hda_verb(codec, nodes[k], HDA_VERB_SET_CONNECT_SEL,
                           (uint16_t)sels[k], NULL, 0);
        }
    }

    /* Power the whole path to D0, then open the amps (0 dB, unmuted). */
    for (uint8_t k = 0; k < len; k++) hda_power_d0(codec, nodes[k]);
    hda_open_amps(codec, nodes, len);

    /* Converter: format + stream tag/channel (tag 1 / channel 0). */
    if (hda_verb(codec, dac, HDA_VERB_SET_STREAM_FORMAT,
                 (uint16_t)HDA_FMT_48K_16BIT_STEREO, NULL, 0) != 0)
        return HDA_ERR_IO;
    if (hda_verb(codec, dac, HDA_VERB_SET_CHANNEL_STREAM,
                 (uint16_t)((g_stream_tag << 4) | 0u), NULL, 0) != 0)
        return HDA_ERR_IO;

    /* Pin: output enable (plus the headphone amp when the pin drives one) and
     * EAPD, which many codecs need to power their speaker amplifier. */
    if (pincap != HDA_PARAM_INVALID && (pincap & 0x08u) != 0u &&
        (((cfg >> 20) & 0x0Fu) == 0x02u))
        pin_ctl |= HDA_PINCTL_HP_EN;
    if (hda_verb(codec, pin, HDA_VERB_SET_PIN_WIDGET_CTL, (uint16_t)pin_ctl,
                 NULL, 0) != 0)
        return HDA_ERR_IO;
    (void)hda_verb(codec, pin, HDA_VERB_SET_EAPD_BTL, 0x0002u, NULL, 0);

    console_printf("[hda] converter fmt=0x%x stream=%u ch=0 dac=%u pin=%u "
                   "path_len=%u\n",
                   (uint64_t)HDA_FMT_48K_16BIT_STEREO, (uint64_t)g_stream_tag,
                   (uint64_t)dac, (uint64_t)pin, (uint64_t)len);
    g_dac = dac;
    g_pin = pin;
    return HDA_OK;
}

/* Walk the Audio Function Group's widgets, pick the best analog output pin,
 * resolve the path to its converter and program it. */
static int hda_codec_program(uint8_t codec)
{
    uint32_t counts = hda_param(codec, 0u, HDA_PARAM_SUBNODE_COUNT);
    uint32_t start, count;
    uint8_t best_nodes[HDA_PATH_MAX];
    uint8_t best_sels[HDA_PATH_MAX];
    uint8_t best_len = 0u;
    uint8_t best_pin = 0xFFu;
    int best_score = -1;
    uint32_t ndacs = 0u;
    uint32_t npins = 0u;

    if (counts == HDA_PARAM_INVALID) return HDA_ERR_IO;
    start = (counts >> 16) & 0xFFu;
    count = counts & 0xFFu;

    /* ---- find the Audio Function Group (function type 0x01) ------------- */
    g_afg = 0u;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t nid = (uint8_t)(start + i);
        const uint32_t type = hda_param(codec, nid, HDA_PARAM_FUNCTION_TYPE);
        if (type != HDA_PARAM_INVALID && (type & 0xFFu) == 0x01u) {
            g_afg = nid;
            break;
        }
    }
    if (g_afg == 0u) return HDA_ERR_IO;
    hda_power_d0(codec, g_afg);

    /* ---- enumerate the AFG's widgets ----------------------------------- */
    counts = hda_param(codec, g_afg, HDA_PARAM_SUBNODE_COUNT);
    if (counts == HDA_PARAM_INVALID) return HDA_ERR_IO;
    start = (counts >> 16) & 0xFFu;
    count = counts & 0xFFu;
    if (count == 0u || count > 64u) return HDA_ERR_IO;

    console_printf("[hda] afg=%u widgets=%u..%u\n", (uint64_t)g_afg,
                   (uint64_t)start, (uint64_t)(start + count - 1u));

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t nid = (uint8_t)(start + i);
        const int type = hda_widget_type(codec, nid);
        const uint32_t cfg = hda_cfg_default(codec, nid);
        const uint32_t pincap = hda_param(codec, nid, HDA_PARAM_PIN_CAP);
        const uint32_t devtype = (cfg == HDA_PARAM_INVALID)
                               ? 0xFu : ((cfg >> 20) & 0x0Fu);
        uint8_t nodes[HDA_PATH_MAX];
        uint8_t sels[HDA_PATH_MAX];
        uint8_t len = 0u;
        int score;

        if (type == HDA_WIDGET_AUDIO_OUTPUT) ndacs++;
        if (type != HDA_WIDGET_PIN_COMPLEX) continue;
        npins++;

        /* Only analog output device types (line out / speaker / HP out). */
        if (devtype > 2u) continue;
        if (pincap != HDA_PARAM_INVALID && pincap != 0u &&
            (pincap & HDA_PINCAP_OUTPUT) == 0u) continue;

        nodes[0] = nid;                 /* DFS starts at the pin widget       */
        if (hda_path_dfs(codec, nodes, sels, &len, 0u, 0u) != 0) continue;

        score = 4;                                  /* has a converter      */
        if (devtype == 0u) score += 3;              /* line out             */
        else if (devtype == 1u) score += 2;         /* speaker              */
        else score += 1;                            /* headphone            */
        if ((pincap & HDA_PINCAP_OUTPUT) != 0u) score += 2;
        if (score > best_score) {
            best_score = score;
            best_pin = nid;
            best_len = len;
            for (uint8_t k = 0; k < len; k++) {
                best_nodes[k] = nodes[k];
                best_sels[k] = sels[k];
            }
        }
    }

    if (best_pin == 0xFFu) {
        console_printf("[hda] no usable output pin (dacs=%u pins=%u)\n",
                       (uint64_t)ndacs, (uint64_t)npins);
        return HDA_ERR_IO;
    }

    g_path_len = best_len;
    for (uint8_t k = 0; k < best_len; k++) {
        g_path_nodes[k] = best_nodes[k];
        g_path_sels[k] = best_sels[k];
    }
    g_dac = best_nodes[best_len - 1u];
    g_pin = best_pin;
    return hda_program_path(codec, g_path_nodes, g_path_sels, g_path_len);
}

/* ===================== stream descriptor ================================= */
/* Work out how many stream descriptors are implemented and which one is the
 * first OUTPUT descriptor.
 * The controller reports the stream counts in GCAP, but implementations differ
 * on whether the field is the count or the count minus one (QEMU reports
 * 0x4401 with 8 implemented blocks = 4 in + 4 out), so the block count is
 * measured from the read-only FIFOSIZE register and used to pick the reading
 * that is consistent with it. */
static int hda_setup_stream_base(void)
{
    uint32_t blocks = 0u;
    uint32_t f_in, f_out, n_in;

    for (uint32_t i = 0; i < 16u; i++) {
        const uint32_t base = HDA_SD_BASE + i * HDA_SD_STRIDE;
        if (hda_rd16(base + HDA_SD_FIFOSIZE) != 0u) blocks = i + 1u;
        else break;                     /* descriptors are contiguous        */
    }
    if (blocks == 0u) {
        console_puts("[hda] no stream descriptor found\n");
        return -1;
    }

    f_in = (g_gcap >> 8) & 0xFu;
    f_out = (g_gcap >> 12) & 0xFu;
    n_in = f_in;                        /* interpretation A: field is count  */
    if (f_in + f_out != blocks) {
        if ((f_in + 1u) + (f_out + 1u) == blocks) {
            n_in = f_in + 1u;           /* interpretation B: field is count-1 */
        } else {
            n_in = blocks / 2u;         /* fall back to an even split         */
        }
    }
    if (n_in >= blocks) n_in = blocks - 1u;

    g_sd_blocks = blocks;
    g_num_in_sd = n_in;
    g_sd_base = HDA_SD_BASE + n_in * HDA_SD_STRIDE;
    return 0;
}

static int hda_sd_reset(uint32_t sd)
{
    /* Clear RUN and SRST, assert SRST, wait for the acknowledge bit, then
     * release it and wait for the cleared readback. */
    hda_wr32(sd + HDA_SD_CTL, 0u);
    if (hda_wait32(sd + HDA_SD_CTL, 0x3u, 0x0u, HDA_VERB_TIMEOUT_MS) != 0)
        return -1;
    hda_wr32(sd + HDA_SD_CTL, HDA_SD_CTL_SRST);
    if (hda_wait32(sd + HDA_SD_CTL, 0x1u, 0x1u, HDA_VERB_TIMEOUT_MS) != 0)
        return -1;
    hda_wr32(sd + HDA_SD_CTL, 0u);
    if (hda_wait32(sd + HDA_SD_CTL, 0x3u, 0x0u, HDA_VERB_TIMEOUT_MS) != 0)
        return -1;
    return 0;
}

static void hda_bdl_entry(uint32_t index, uint32_t addr, uint32_t len,
                          uint32_t flags)
{
    uint8_t *e = g_bdl + index * 16u;

    *(volatile uint32_t *)(void *)(e + 0)  = addr;
    *(volatile uint32_t *)(void *)(e + 4)  = 0u;
    *(volatile uint32_t *)(void *)(e + 8)  = len;
    *(volatile uint32_t *)(void *)(e + 12) = flags;
}

/* Play the staged g_pcm buffer (frames stereo frames) through output SD0. */
static int hda_stream_play(uint32_t frames)
{
    const uint32_t bytes = frames * 4u;          /* 16-bit stereo            */
    const uint32_t expect_ms = (uint32_t)(((uint64_t)frames * 1000ull)
                                          / HDA_DEFAULT_RATE);
    const uint32_t sd = g_sd_base;
    uint32_t chunk, entries, off, spins = 0u, prev = 0u;
    uint32_t elapsed = 0u;
    int rc = HDA_OK;

    if (frames == 0u || frames > HDA_MAX_FRAMES) return HDA_ERR_ARG;
    if (bytes < 8u) return HDA_ERR_ARG;

    /* Split the stream into at most HDA_BDL_ENTRIES descriptors.  Every
     * descriptor start must be 128-byte aligned, so the chunk size (except
     * the last one) is rounded up to a multiple of 128. */
    chunk = (bytes + HDA_BDL_ENTRIES - 1u) / HDA_BDL_ENTRIES;
    chunk = (chunk + 127u) & ~127u;
    entries = (bytes + chunk - 1u) / chunk;
    if (entries > HDA_BDL_ENTRIES || entries == 0u) return HDA_ERR_ARG;

    memset(g_bdl, 0, sizeof(g_bdl));
    off = 0u;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t n = bytes - off;
        if (n > chunk) n = chunk;
        /* No IOC: this driver polls, it does not take interrupts. */
        hda_bdl_entry(i, (uint32_t)(uintptr_t)(g_pcm + off / 2u), n, 0u);
        off += n;
    }

    if (hda_sd_reset(sd) != 0) {
        console_puts("[hda] stream reset failed\n");
        return HDA_ERR_DMA;
    }
    hda_wr8(sd + HDA_SD_STS, HDA_SD_STS_CLEAR);
    hda_wr32(sd + HDA_SD_CBL, bytes);
    hda_wr16(sd + HDA_SD_LVI, (uint16_t)(entries - 1u));
    hda_wr16(sd + HDA_SD_FMT, (uint16_t)HDA_FMT_48K_16BIT_STEREO);
    hda_wr32(sd + HDA_SD_BDPL, (uint32_t)(uintptr_t)g_bdl);
    hda_wr32(sd + HDA_SD_BDPU, 0u);
    hda_wr32(sd + HDA_SD_CTL, HDA_SD_CTL_TAG(g_stream_tag));
    __asm__ volatile("mfence" ::: "memory");
    hda_wr32(sd + HDA_SD_CTL,
             HDA_SD_CTL_TAG(g_stream_tag) | HDA_SD_CTL_RUN);

    /* Poll SDLPIB and wait for a full pass: the stream descriptor list is
     * cyclic, so when the last descriptor has been consumed the controller
     * wraps LPIB back to 0.  Completion is judged by the *position*, not by
     * wall-clock time: QEMU's codec pulls samples through a virtual-clock
     * timer, and while the guest polls MMIO in a tight loop that timer can run
     * behind real time, so stopping on "expected duration elapsed" truncates
     * the tone (the WAV then contains only part of the 1.0 s acceptance
     * signal).  The PIT only provides a generous backstop so a stalled engine
     * cannot hang the boot. */
    hda_clock_begin();
    for (;;) {
        const uint32_t lpib = hda_rd32(sd + HDA_SD_LPIB);

        if (lpib < prev && prev >= bytes / 2u) break;       /* one full pass */
        if ((hda_rd8(sd + HDA_SD_STS) & HDA_SD_STS_ERROR) != 0u) {
            console_printf("[hda] stream status error (sts=0x%x lpib=%u)\n",
                           (uint64_t)hda_rd8(sd + HDA_SD_STS),
                           (uint64_t)lpib);
            rc = HDA_ERR_DMA;
            break;
        }
        prev = lpib;
        /* Throttle the poll: a tight MMIO loop keeps taking QEMU's BQL away
         * from the audio/codec timers, which then run behind real time and
         * leave the last few KB of the tone stuck in the codec FIFO.  A short
         * pause burst per sample keeps the position granularity fine without
         * starving the device model. */
        hda_delay_loops(4000u);
        if (++spins >= 16u) {
            spins = 0u;
            elapsed = hda_clock_ms();
            if (elapsed >= expect_ms * 8u + 2000u) {
                console_printf("[hda] stream timeout (lpib=%u of %u bytes)\n",
                               (uint64_t)lpib, (uint64_t)bytes);
                rc = HDA_ERR_DMA;
                break;
            }
        }
    }

    /* The controller consumed the whole tone, but the codec still holds a few
     * KB of it in its sample FIFO.  Turning the stream off here would drop
     * that tail (the WAV then ends ~40 ms early).  Overwrite the staged buffer
     * with silence and keep the cyclic stream running for a short drain
     * window: the FIFO flushes the rest of the tone into the audio backend and
     * then feeds it silence, so the recorded tone is complete and the extra
     * material is silence rather than a repeated tone. */
    memset(g_pcm, 0, (size_t)bytes);
    __asm__ volatile("mfence" ::: "memory");
    hda_clock_begin();
    while (hda_clock_ms() < HDA_DRAIN_MS) __asm__ volatile("pause");

    hda_wr32(sd + HDA_SD_CTL, HDA_SD_CTL_TAG(g_stream_tag));   /* stop      */
    hda_wr8(sd + HDA_SD_STS, HDA_SD_STS_CLEAR);

    if (rc == HDA_OK) {
        console_printf("[hda] stream: %u bytes played, lpib=%u\n",
                       (uint64_t)bytes, (uint64_t)prev);
    }
    return rc;
}

/* ===================== sine tone (integer only: -mno-80387) =============
 * Quarter wave, Q15: 32767 * sin(i * (pi/2) / 64) for i = 0..64, with linear
 * interpolation in between; the same table shape ac97.c uses. */
static const int16_t g_sin_q15[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

static int32_t hda_quarter_sine(uint32_t frac)
{
    const uint32_t idx = frac >> 24;
    const uint32_t sub = (frac >> 8) & 0xFFFFu;
    const int32_t a = g_sin_q15[idx];
    const int32_t b = g_sin_q15[idx + 1u];
    return a + (((b - a) * (int32_t)sub) >> 16);
}

static int32_t hda_sin_q15(uint32_t phase)
{
    const uint32_t quadrant = phase >> 30;
    const uint32_t frac = phase & 0x3FFFFFFFu;

    switch (quadrant) {
    case 0u:  return hda_quarter_sine(frac);
    case 1u:  return hda_quarter_sine(0x40000000u - frac);
    case 2u:  return -hda_quarter_sine(frac);
    default:  return -hda_quarter_sine(0x40000000u - frac);
    }
}

/* Fill g_pcm with a 48 kHz / 16-bit / stereo sine, return the frame count. */
static uint32_t hda_tone_fill(uint32_t freq_hz, uint32_t duration_ms)
{
    const uint32_t step =
        (uint32_t)(((uint64_t)freq_hz << 32) / HDA_DEFAULT_RATE);
    uint32_t frames = (uint32_t)(((uint64_t)duration_ms * HDA_DEFAULT_RATE)
                                 / 1000u);
    uint32_t phase = 0u;

    if (frames == 0u) frames = 1u;
    if (frames > HDA_MAX_FRAMES) frames = HDA_MAX_FRAMES;

    for (uint32_t i = 0; i < frames; i++) {
        const int32_t s = (hda_sin_q15(phase) * HDA_TONE_AMPLITUDE) >> 15;
        g_pcm[2u * i] = (int16_t)s;
        g_pcm[2u * i + 1u] = (int16_t)s;
        phase += step;
    }
    return frames;
}

/* ===================== public playback API =============================== */
int hda_play_pcm(const int16_t *interleaved, uint32_t frames)
{
    if (g_state != 1) return HDA_ERR_NODEV;
    if (!interleaved || frames == 0u || frames > HDA_MAX_FRAMES)
        return HDA_ERR_ARG;
    memcpy(g_pcm, interleaved, (size_t)frames * 4u);
    return hda_stream_play(frames);
}

int hda_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    uint32_t frames;

    if (g_state != 1) return HDA_ERR_NODEV;
    if (freq_hz < 20u || freq_hz > 12000u) return HDA_ERR_ARG;
    if (duration_ms < 10u || duration_ms > 1000u) return HDA_ERR_ARG;

    frames = hda_tone_fill(freq_hz, duration_ms);
    return hda_stream_play(frames);
}

/* ===================== probe / init ====================================== */
static int hda_probe_codec(uint16_t mask, uint8_t *codec_out)
{
    for (uint32_t c = 0; c < 15u; c++) {
        uint32_t v = HDA_PARAM_INVALID;

        if (mask != 0u && (mask & (1u << c)) == 0u) continue;
        /* The first successful verb also calibrates the RIRB entry index. */
        if (hda_verb((uint8_t)c, 0u, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID,
                     &v, 1) != 0)
            continue;
        if (v == 0u || v == HDA_PARAM_INVALID) continue;
        *codec_out = (uint8_t)c;
        return HDA_OK;
    }
    return HDA_ERR_IO;
}

static void hda_build_name(uint32_t bar, uint32_t pci_id, uint32_t codec_id)
{
    /* Plain %x only: the kernel's vsnprintf does not implement field widths
     * such as %04x, so keep the one-line description to simple conversions. */
    snprintf(g_name, sizeof(g_name),
             "Intel HDA %x:%x BAR0=0x%x, codec %u vendor=0x%x device=0x%x",
             (unsigned)(pci_id & 0xFFFFu), (unsigned)((pci_id >> 16) & 0xFFFFu),
             (unsigned)bar, (unsigned)g_codec,
             (unsigned)(codec_id >> 16), (unsigned)(codec_id & 0xFFFFu));
}

int hda_init(void)
{
    struct hda_pci_loc loc;
    uint32_t cmd, id, codec_id = 0u, bar0;
    uint64_t base;
    uint16_t states;
    uint16_t codec_mask;

    if (g_state == 1) return HDA_OK;
    if (g_state != 0) return (g_state == -1) ? HDA_ERR_NODEV : HDA_ERR_IO;

    if (hda_pci_find(&loc) != 0) {
        console_puts("[hda] no controller\n");
        g_state = -1;
        return HDA_ERR_NODEV;
    }

    /* Memory decoding and bus mastering are both required: the register file
     * and the rings/BDL are fetched by the bus master engine. */
    cmd = hda_pci_read32(loc.bus, loc.dev, loc.fn, HDA_CFG_COMMAND);
    hda_pci_write32(loc.bus, loc.dev, loc.fn, HDA_CFG_COMMAND,
                    cmd | HDA_PCI_CMD_MEM_SPACE | HDA_PCI_CMD_BUS_MASTER);
    cmd = hda_pci_read32(loc.bus, loc.dev, loc.fn, HDA_CFG_COMMAND);
    if ((cmd & (HDA_PCI_CMD_MEM_SPACE | HDA_PCI_CMD_BUS_MASTER)) !=
        (HDA_PCI_CMD_MEM_SPACE | HDA_PCI_CMD_BUS_MASTER)) {
        console_puts("[hda] cannot enable MEM/BUS_MASTER\n");
        g_state = -2;
        return HDA_ERR_IO;
    }

    id = hda_pci_read32(loc.bus, loc.dev, loc.fn, HDA_CFG_VENDOR_ID);
    bar0 = hda_pci_read32(loc.bus, loc.dev, loc.fn, HDA_CFG_BAR0);
    if ((bar0 & 1u) != 0u || (bar0 & 0xFFFFFFF0u) == 0u) {
        console_printf("[hda] unusable BAR0=0x%x\n", (uint64_t)bar0);
        g_state = -2;
        return HDA_ERR_IO;
    }
    base = (uint64_t)(bar0 & 0xFFFFFFF0u);
    if ((bar0 & 0x6u) == 0x4u) {            /* 64-bit memory BAR            */
        const uint32_t upper =
            hda_pci_read32(loc.bus, loc.dev, loc.fn, HDA_CFG_BAR1);
        base |= (uint64_t)upper << 32;
    }
    base &= ~(uint64_t)0xFu;

    /* The BAR is above the boot identity map, so it must be mapped; 16 KiB
     * covers the controller registers and all stream descriptors. */
    if (mmu_map_mmio_identity(base, 0x4000u) != 0) {
        console_printf("[hda] cannot map MMIO BAR0=0x%x\n", (uint64_t)base);
        g_state = -2;
        return HDA_ERR_IO;
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)base;
    g_state = -2;                            /* failed until fully ready     */

    /* ---- controller reset: assert CRST, then release and wait ---------- */
    hda_wr32(HDA_REG_INTCTL, 0u);            /* polling only: no interrupts  */
    hda_wr32(HDA_REG_GCTL, 0u);
    if (hda_wait32(HDA_REG_GCTL, 0x1u, 0x0u, HDA_VERB_TIMEOUT_MS) != 0) {
        console_puts("[hda] controller reset (assert) failed\n");
        return HDA_ERR_IO;
    }
    hda_delay_loops(10000u);
    hda_wr32(HDA_REG_GCTL, 0x1u);
    if (hda_wait32(HDA_REG_GCTL, 0x1u, 0x1u, HDA_VERB_TIMEOUT_MS) != 0) {
        console_puts("[hda] controller reset (release) failed\n");
        return HDA_ERR_IO;
    }
    hda_delay_loops(50000u);                 /* let the codec settle         */

    g_gcap = (uint32_t)hda_rd16(HDA_REG_GCAP);
    states = hda_rd16(HDA_REG_STATESTS);
    codec_mask = (uint16_t)(states & 0x7FFFu);
    if (states != 0u) hda_wr16(HDA_REG_STATESTS, states);  /* W1C clear        */

    console_printf("[hda] controller BAR0=0x%x codec mask=0x%x gcap=0x%x\n",
                   (uint64_t)(uint32_t)base, (uint64_t)codec_mask,
                   (uint64_t)g_gcap);

    if (hda_ring_init() != 0) {
        console_puts("[hda] CORB/RIRB init failed\n");
        return HDA_ERR_IO;
    }

    /* ---- codec --------------------------------------------------------- */
    if (hda_probe_codec(codec_mask, &g_codec) != 0) {
        console_printf("[hda] controller present but no codec answered "
                       "(corbwp=%u rirbwp=%u corbctl=0x%x rirbctl=0x%x)\n",
                       (uint64_t)g_corb_wp,
                       (uint64_t)(hda_rd16(HDA_REG_RIRBWP) & 0xFFu),
                       (uint64_t)hda_rd8(HDA_REG_CORBCTL),
                       (uint64_t)hda_rd8(HDA_REG_RIRBCTL));
        return HDA_ERR_IO;
    }
    codec_id = hda_param(g_codec, 0u, HDA_PARAM_VENDOR_ID);
    console_printf("[hda] codec vendor=0x%x device=0x%x rev=0x%x\n",
                   (uint64_t)(codec_id >> 16), (uint64_t)(codec_id & 0xFFFFu),
                   (uint64_t)hda_param(g_codec, 0u, HDA_PARAM_REVISION_ID));

    if (hda_codec_program(g_codec) != HDA_OK) {
        console_puts("[hda] codec bring-up failed\n");
        return HDA_ERR_IO;
    }

    if (hda_setup_stream_base() != 0) return HDA_ERR_IO;
    console_printf("[hda] stream sd=0x%x blocks=%u in_sd=%u tag=%u\n",
                   (uint64_t)g_sd_base, (uint64_t)g_sd_blocks,
                   (uint64_t)g_num_in_sd, (uint64_t)g_stream_tag);

    hda_build_name((uint32_t)base, id, codec_id);
    g_state = 1;
    return HDA_OK;
}

int hda_present(void)
{
    return g_state == 1;
}

const char *hda_name(void)
{
    return g_name[0] ? g_name : "no HDA controller";
}

/* ===================== boot self-test ==================================== */
int hda_selftest(void)
{
    int rc;

    if (g_state != 1) return HDA_ERR_NODEV;  /* hda_init() already reported */

    console_puts("[hda] beep 440Hz 1000ms\n");
    rc = hda_beep(440u, 1000u);
    if (rc != HDA_OK) {
        console_printf("[hda] selftest: playback FAILED (rc=%d)\n",
                       (int64_t)rc);
        return rc;
    }
    console_printf("[hda] selftest: PASS played %u frames @%u Hz "
                   "(expect 1.000 s)\n",
                   (uint64_t)(((uint64_t)1000u * HDA_DEFAULT_RATE) / 1000u),
                   (uint64_t)HDA_DEFAULT_RATE);
    return HDA_OK;
}
