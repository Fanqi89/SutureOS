/* SutureOS module: AC'97 audio controller driver - PCM output.
 * ============================================================================
 * Source / provenance
 *   Ported from: HBOS - src/ac97.c (reference implementation, GPL-3.0)
 *                local copy: C:/Users/fanqi/Desktop/自研操作系统/HBOS/src/ac97.c
 *   The reference is GPL-3.0 and SutureOS is GPL-3.0, so the port is released
 *   under the same terms.  It was used as the algorithm / programming-model
 *   reference for: PCI discovery of the Intel 82801AA class controller, the
 *   NAM (mixer BAR0) codec cold reset + unmute sequence, and the NABM (bus
 *   master BAR1) PCM-OUT Buffer-Descriptor-List model.  The code below was
 *   written for this kernel; differences from the reference are:
 *     1. adapted to SutureOS APIs - <stitch/io.h> port I/O, console_printf,
 *        static .bss DMA memory (flat identity map: physical == virtual), no
 *        kernel heap and no task_yield() (the boot self-test runs with the
 *        scheduler not started yet);
 *     2. PCI probe by Intel vendor + known AC'97 device IDs (0x2415 first,
 *        0x2425 and the later southbridges accepted) with a generic
 *        class 0x04/0x01/prog-if 0 fallback, instead of a class-only probe;
 *     3. only PCI bus 0 is enumerated - the AC'97 link is part of the
 *        southbridge, so a machine without a codec pays for 32 config reads
 *        instead of a 256-bus walk (the "no AC'97" boot path must stay cheap);
 *     4. memory BARs are supported too (identity-mapped with
 *        mmu_map_mmio_identity()); the usual 82801AA layout is two I/O BARs;
 *     5. the codec reset is followed by a read-back check of the mixer
 *        registers (reported through ac97_name() diagnostics) instead of
 *        trusting the write;
 *     6. playback completion is polled from the NABM status register and
 *        bounded by the 8254 PIT counter (interrupts are off at boot time);
 *     7. mono input is duplicated to interleaved stereo (the AC'97 DAC has no
 *        mono mode) and the descriptor list is carved into <=32 evenly sized
 *        entries instead of one descriptor per call;
 *     8. Variable Rate Audio is programmed for rates != 48000 Hz when the
 *        codec advertises VRA, otherwise the request is refused instead of
 *        silently playing at 48 kHz.
 * ============================================================================
 */

#include "ac97.h"

#include <stitch/console.h>
#include <stitch/io.h>
#include <stitch/string.h>
#include <stitch/types.h>

#include "mb_pit.h"   /* 8254 counter: wall-clock bound without interrupts */
#include "mmu.h"      /* mmu_map_mmio_identity() for a memory BAR          */

/* ===================== PCI config space (mechanism #1) =====================
 * Same accessor pair as rtl8139.c/e1000.c: 0xCF8 must be written as a DWORD,
 * a byte-wise write does not take effect on QEMU. */
#define AC97_PCI_CFG_ADDR   0xCF8u
#define AC97_PCI_CFG_DATA   0xCFCu

#define AC97_CFG_VENDOR_ID  0x00u
#define AC97_CFG_COMMAND    0x04u
#define AC97_CFG_REV_CLASS  0x08u
#define AC97_CFG_HEADER     0x0Cu
#define AC97_CFG_BAR0       0x10u
#define AC97_CFG_BAR1       0x14u

#define AC97_PCI_CMD_IO_SPACE   (1u << 0)
#define AC97_PCI_CMD_MEM_SPACE  (1u << 1)
#define AC97_PCI_CMD_BUS_MASTER (1u << 2)

#define AC97_PCI_VENDOR_INTEL   0x8086u

static uint32_t ac97_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(AC97_PCI_CFG_ADDR, addr);
    return inl(AC97_PCI_CFG_DATA);
}

static void ac97_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                             uint32_t value)
{
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(AC97_PCI_CFG_ADDR, addr);
    outl(AC97_PCI_CFG_DATA, value);
}

/* AC'97 audio functions of the Intel southbridges.  0x2415 (82801AA, ICH) is
 * the controller QEMU models with "-device AC97"; the others share the exact
 * same NAM/NABM register layout. */
static const uint16_t g_ac97_device_ids[] = {
    0x2415u,   /* 82801AA / 82801AB (ICH)          - AC'97 audio */
    0x2425u,   /* 82801BA / 82801BAM (ICH2)        - AC'97 audio */
    0x2445u,   /* 82801CA / 82801CAM (ICH3)        - AC'97 audio */
    0x2485u,   /* 82801CA / CAM, second AC'97 link - audio/modem  */
    0x24C5u,   /* 82801DB / DBL / DBM (ICH4)       - AC'97 audio */
    0x24D5u,   /* 82801EB / ER (ICH5)              - AC'97 audio */
    0x25A6u,   /* 6300ESB                          - AC'97 audio */
    0x266Eu,   /* ICH6 family                      - AC'97 audio */
    0x27DEu    /* ICH7 family                      - AC'97 audio */
};

struct ac97_pci_loc {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
};

static int ac97_is_intel_audio(uint16_t device)
{
    for (unsigned i = 0;
         i < sizeof(g_ac97_device_ids) / sizeof(g_ac97_device_ids[0]); i++) {
        if (g_ac97_device_ids[i] == device) return 1;
    }
    return 0;
}

/* Find the controller: an Intel southbridge AC'97 audio function when there is
 * one, else any multimedia/audio function with program interface 0 whose two
 * BARs are I/O windows (that is the AC'97 bus-master layout). */
static int ac97_pci_find(struct ac97_pci_loc *out)
{
    struct ac97_pci_loc fallback;
    int have_fallback = 0;

    /* The AC'97 link sits in the southbridge, i.e. on the root PCI bus. */
    for (uint32_t dev = 0; dev < 32u; dev++) {
        uint32_t id = ac97_pci_read32(0, (uint8_t)dev, 0, AC97_CFG_VENDOR_ID);
        uint32_t hdr, fn_max;

        if ((id & 0xFFFFu) == 0xFFFFu || (id & 0xFFFFu) == 0x0000u)
            continue;                                   /* no device in slot */

        hdr = ac97_pci_read32(0, (uint8_t)dev, 0, AC97_CFG_HEADER);
        fn_max = (hdr & 0x00800000u) ? 8u : 1u;         /* multi-function bit */

        for (uint32_t fn = 0; fn < fn_max; fn++) {
            uint32_t cc, vendor, device;

            if (fn != 0u) {
                id = ac97_pci_read32(0, (uint8_t)dev, (uint8_t)fn,
                                     AC97_CFG_VENDOR_ID);
                if ((id & 0xFFFFu) == 0xFFFFu) continue;
            }
            vendor = id & 0xFFFFu;
            device = (id >> 16) & 0xFFFFu;

            if (vendor == AC97_PCI_VENDOR_INTEL &&
                ac97_is_intel_audio((uint16_t)device)) {
                out->bus = 0;
                out->dev = (uint8_t)dev;
                out->fn = (uint8_t)fn;
                return 0;
            }

            cc = ac97_pci_read32(0, (uint8_t)dev, (uint8_t)fn, AC97_CFG_REV_CLASS);
            if (!have_fallback &&
                ((cc >> 24) & 0xFFu) == 0x04u &&        /* multimedia         */
                ((cc >> 16) & 0xFFu) == 0x01u &&        /* audio              */
                ((cc >> 8) & 0xFFu) == 0x00u) {         /* program interface 0 */
                uint32_t bar0 = ac97_pci_read32(0, (uint8_t)dev, (uint8_t)fn,
                                                AC97_CFG_BAR0);
                uint32_t bar1 = ac97_pci_read32(0, (uint8_t)dev, (uint8_t)fn,
                                                AC97_CFG_BAR1);
                if ((bar0 & 1u) && (bar1 & 1u) &&
                    (bar0 & 0xFFFCu) && (bar1 & 0xFFFCu)) {
                    fallback.bus = 0;
                    fallback.dev = (uint8_t)dev;
                    fallback.fn = (uint8_t)fn;
                    have_fallback = 1;
                }
            }
        }
    }

    if (have_fallback) {
        *out = fallback;
        return 0;
    }
    return -1;
}

/* ===================== register windows (NAM / NABM) =====================
 * BAR0 = Native Audio Mixer (codec registers), BAR1 = Native Audio Bus Master
 * (DMA engine).  Both are I/O windows on the 82801AA; a memory BAR is accepted
 * as well and accessed through the identity map. */
struct ac97_window {
    int mmio;                 /* 0 = port I/O, 1 = identity-mapped memory */
    uint16_t port;            /* base port when mmio == 0                   */
    volatile uint8_t *base;   /* base address when mmio == 1                */
};

static uint8_t ac97_rd8(const struct ac97_window *w, uint16_t off)
{
    if (w->mmio) return *(volatile uint8_t *)(w->base + off);
    return inb((uint16_t)(w->port + off));
}

static uint16_t ac97_rd16(const struct ac97_window *w, uint16_t off)
{
    if (w->mmio) return *(volatile uint16_t *)(w->base + off);
    return inw((uint16_t)(w->port + off));
}

static void ac97_wr8(const struct ac97_window *w, uint16_t off, uint8_t v)
{
    if (w->mmio) { *(volatile uint8_t *)(w->base + off) = v; return; }
    outb((uint16_t)(w->port + off), v);
}

static void ac97_wr16(const struct ac97_window *w, uint16_t off, uint16_t v)
{
    if (w->mmio) { *(volatile uint16_t *)(w->base + off) = v; return; }
    outw((uint16_t)(w->port + off), v);
}

static void ac97_wr32(const struct ac97_window *w, uint16_t off, uint32_t v)
{
    if (w->mmio) { *(volatile uint32_t *)(w->base + off) = v; return; }
    outl((uint16_t)(w->port + off), v);
}

/* ===================== NAM (codec) register offsets ===================== */
#define AC97_NAM_RESET          0x00u   /* write 0 = cold reset               */
#define AC97_NAM_MASTER_VOL     0x02u   /* bit15 mute, bits 12:8/4:0 both chan */
#define AC97_NAM_PCM_OUT_VOL    0x18u   /* same layout as the master volume   */
#define AC97_NAM_EXT_AUDIO_ID   0x28u   /* bit0: VRA (variable rate audio)    */
#define AC97_NAM_EXT_AUDIO_CTRL 0x2Au   /* bit0: VRA enable                   */
#define AC97_NAM_PCM_DAC_RATE   0x2Cu   /* PCM front DAC sample rate          */

/* ===================== NABM (bus master) register offsets =================
 * PCM OUT channel starts at 0x10; the same layout repeats every 0x10 bytes for
 * the other channels (PCM IN 0x00, MIC IN 0x20, ...). */
#define AC97_NABM_PO_BDBAR      0x10u   /* u32: descriptor list base address  */
#define AC97_NABM_PO_CIV        0x14u   /* u8 : current descriptor index (RO) */
#define AC97_NABM_PO_LVI        0x15u   /* u8 : last valid descriptor index   */
#define AC97_NABM_PO_SR         0x16u   /* u16: status (RW1C)                 */
#define AC97_NABM_PO_PICB       0x18u   /* u16: samples left in current (RO)  */
#define AC97_NABM_PO_CR         0x1Bu   /* u8 : control                       */

#define AC97_PO_SR_DCH          0x0001u /* DMA controller halted              */
#define AC97_PO_SR_CLEAR        0x0017u /* DCH|FC|LVBI|BCIS (bit3 CELV is RO) */
#define AC97_PO_CR_RPBM         0x01u   /* run / pause bus master: 1 = run    */
#define AC97_PO_CR_RR           0x02u   /* reset the channel registers        */

/* ===================== DMA memory and limits ============================= */
#define AC97_MAX_BDL              32u
#define AC97_MAX_SAMPLES_PER_BD   0xFFFEu   /* 16-bit words per descriptor    */
#define AC97_STAGE_SAMPLES    192000u       /* 2.0 s of 48 kHz stereo (16-bit) */
#define AC97_TONE_FRAMES       48000u       /* 1.0 s of 48 kHz mono           */

typedef struct __attribute__((packed)) {
    uint32_t buffer_addr;   /* 32-bit physical address of the PCM samples */
    uint16_t length;        /* number of 16-bit samples in this buffer    */
    uint16_t flags;         /* bit15 IOC / bit14 BUP - not used           */
} ac97_bd_t;

/* All DMA memory is static: the kernel's low memory is a flat identity map, so
 * the physical address of a .bss symbol is simply its virtual address. */
static ac97_bd_t g_bdl[AC97_MAX_BDL] __attribute__((aligned(32)));
static int16_t g_stage[AC97_STAGE_SAMPLES] __attribute__((aligned(16)));
static int16_t g_tone[AC97_TONE_FRAMES] __attribute__((aligned(16)));

static struct ac97_window g_nam;
static struct ac97_window g_nabm;
static char g_name[128];
static int g_present;
static uint16_t g_device;
static int g_vra;            /* codec advertises variable-rate audio      */
static int g_vra_active;     /* VRA bit has been enabled by this driver   */

/* ===================== sine table (integer only: -mno-80387) =============
 * Quarter wave, Q15: 32767 * sin(i * (pi/2) / 64) for i = 0..64.  The tone
 * generator interpolates linearly between entries, so the 440 Hz self-test
 * tone is a clean integer sine without a single floating-point instruction. */
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

/* Linear interpolation between two quarter-wave entries; phase is a 30-bit
 * fraction of the quadrant. */
static int32_t ac97_quarter_sine(uint32_t frac)
{
    const uint32_t idx  = frac >> 24;               /* 0..63                */
    const uint32_t sub  = (frac >> 8) & 0xFFFFu;    /* 16-bit sub-step      */
    const int32_t  a    = g_sin_q15[idx];
    const int32_t  b    = g_sin_q15[idx + 1u];
    return a + (((b - a) * (int32_t)sub) >> 16);
}

/* Sine of a 32-bit phase (0 = 0 deg, 0xFFFFFFFF ~ 360 deg), Q15. */
static int32_t ac97_sin_q15(uint32_t phase)
{
    const uint32_t quadrant = phase >> 30;
    const uint32_t frac = phase & 0x3FFFFFFFu;

    switch (quadrant) {
    case 0u:  return ac97_quarter_sine(frac);
    case 1u:  return ac97_quarter_sine(0x40000000u - frac);
    case 2u:  return -ac97_quarter_sine(frac);
    default:  return -ac97_quarter_sine(0x40000000u - frac);
    }
}

/* Fill g_tone with a mono sine at AC97_DEFAULT_RATE, return the frame count. */
static uint32_t ac97_tone_fill(uint32_t freq_hz, uint32_t duration_ms)
{
    const uint32_t step = (uint32_t)(((uint64_t)freq_hz << 32) / AC97_DEFAULT_RATE);
    uint32_t frames = (uint32_t)(((uint64_t)duration_ms * AC97_DEFAULT_RATE) / 1000u);
    uint32_t phase = 0;

    if (frames == 0u) frames = 1u;
    if (frames > AC97_TONE_FRAMES) frames = AC97_TONE_FRAMES;

    for (uint32_t i = 0; i < frames; i++) {
        /* 0.5 FS: 32767 * 16384 / 32768 ~= 16384 */
        const int32_t s = (ac97_sin_q15(phase) * AC97_TONE_AMPLITUDE) >> 15;
        g_tone[i] = (int16_t)s;
        phase += step;
    }
    return frames;
}

/* ===================== small delay (port-I/O settling) =================== */
static void ac97_delay(volatile uint32_t loops)
{
    while (loops--) __asm__ volatile("pause");
}

/* ===================== PCI BAR -> window ===================== */
static int ac97_window_open(uint32_t bar, struct ac97_window *w)
{
    w->mmio = 0;
    w->port = 0;
    w->base = 0;

    if (bar & 1u) {                                   /* I/O space BAR */
        const uint16_t port = (uint16_t)(bar & 0xFFFCu);
        if (port == 0u) return -1;
        w->port = port;
        return 0;
    }

    /* Memory space BAR.  AC'97 never uses a 64-bit BAR; reject that just in
     * case so a bogus address is not mapped. */
    if ((bar & 0x6u) != 0u) return -1;
    {
        const uint32_t base = bar & 0xFFFFFFF0u;
        if (base == 0u) return -1;
        /* The NAM window is 256 bytes and NABM is 64 bytes; one 4 KiB page
         * covers both.  Addresses above 1 GiB are not in the boot identity
         * map, so the mapping call is mandatory here. */
        if (mmu_map_mmio_identity((uint64_t)base, 0x1000u) != 0) return -1;
        w->mmio = 1;
        w->base = (volatile uint8_t *)(uintptr_t)base;
        return 0;
    }
}

/* ===================== codec bring-up ===================== */
static void ac97_build_name(void)
{
    const char *model = "AC'97 audio";
    if (g_device == 0x2415u) model = "82801AA (ICH)";
    else if (g_device == 0x2425u) model = "82801BA (ICH2)";
    else if (g_device == 0x24C5u) model = "82801DB (ICH4)";
    else if (g_device == 0x24D5u) model = "82801EB (ICH5)";

    if (g_nam.mmio || g_nabm.mmio) {
        snprintf(g_name, sizeof(g_name),
                 "Intel %s AC'97, NAM MMIO 0x%x, NABM MMIO 0x%x",
                 model,
                 (unsigned)g_nam.mmio ? (unsigned)(uintptr_t)g_nam.base
                                      : (unsigned)g_nam.port,
                 (unsigned)g_nabm.mmio ? (unsigned)(uintptr_t)g_nabm.base
                                       : (unsigned)g_nabm.port);
    } else {
        snprintf(g_name, sizeof(g_name),
                 "Intel %s AC'97, NAM 0x%x, NABM 0x%x",
                 model, (unsigned)g_nam.port, (unsigned)g_nabm.port);
    }
}

int ac97_init(void)
{
    struct ac97_pci_loc loc;
    uint32_t cmd, bar0, bar1;
    uint32_t spin;

    if (g_present) return 0;

    if (ac97_pci_find(&loc) != 0) return AC97_ERR_IO;

    /* I/O decoding and bus mastering are both required: the codec lives behind
     * the I/O BAR and the BDL is fetched by the bus master engine. */
    cmd = ac97_pci_read32(loc.bus, loc.dev, loc.fn, AC97_CFG_COMMAND);
    ac97_pci_write32(loc.bus, loc.dev, loc.fn, AC97_CFG_COMMAND,
                     cmd | AC97_PCI_CMD_IO_SPACE | AC97_PCI_CMD_MEM_SPACE |
                     AC97_PCI_CMD_BUS_MASTER);
    cmd = ac97_pci_read32(loc.bus, loc.dev, loc.fn, AC97_CFG_COMMAND);
    if ((cmd & AC97_PCI_CMD_BUS_MASTER) == 0u) return AC97_ERR_IO;

    bar0 = ac97_pci_read32(loc.bus, loc.dev, loc.fn, AC97_CFG_BAR0);
    bar1 = ac97_pci_read32(loc.bus, loc.dev, loc.fn, AC97_CFG_BAR1);
    if (ac97_window_open(bar0, &g_nam) != 0) return AC97_ERR_IO;
    if (ac97_window_open(bar1, &g_nabm) != 0) return AC97_ERR_IO;

    g_device = (uint16_t)(ac97_pci_read32(loc.bus, loc.dev, loc.fn,
                                          AC97_CFG_VENDOR_ID) >> 16);

    /* ---- codec cold reset, then unmute both volume stages ----------------
     * A COLD reset (write 0 to NAM 0x00) puts every mixer register back to its
     * default, and the default master volume is 0x8000 = muted.  The pair of
     * 0x0000 writes below selects 0 dB attenuation on both the master and the
     * PCM-out stage and clears the mute bit. */
    ac97_wr16(&g_nam, AC97_NAM_RESET, 0x0000u);
    ac97_delay(50000u);                       /* let the codec settle */

    ac97_wr16(&g_nam, AC97_NAM_MASTER_VOL, 0x0000u);
    ac97_wr16(&g_nam, AC97_NAM_PCM_OUT_VOL, 0x0000u);

    /* Extended Audio ID bit0 = VRA (variable rate audio).  Only used when a
     * caller asks for a rate other than the 48 kHz default. */
    g_vra = (ac97_rd16(&g_nam, AC97_NAM_EXT_AUDIO_ID) & 0x0001u) ? 1 : 0;
    g_vra_active = 0;

    /* ---- reset the PCM OUT bus master channel --------------------------- */
    ac97_wr8(&g_nabm, AC97_NABM_PO_CR, AC97_PO_CR_RR);
    for (spin = 0; spin < 100000u; spin++) {
        if ((ac97_rd8(&g_nabm, AC97_NABM_PO_CR) & AC97_PO_CR_RR) == 0u) break;
        ac97_delay(4u);
    }
    if (ac97_rd8(&g_nabm, AC97_NABM_PO_CR) & AC97_PO_CR_RR) return AC97_ERR_IO;

    ac97_wr8(&g_nabm, AC97_NABM_PO_CR, 0x00u);
    ac97_wr16(&g_nabm, AC97_NABM_PO_SR, AC97_PO_SR_CLEAR);

    ac97_build_name();
    g_present = 1;
    return AC97_OK;
}

int ac97_present(void) { return g_present; }

const char *ac97_name(void)
{
    return g_present ? g_name : "no AC'97 controller";
}

/* ===================== rate programming (VRA) ===================== */
static int ac97_program_rate(uint32_t rate)
{
    if (rate < 8000u || rate > 48000u) return AC97_ERR_ARG;

    if (rate == AC97_DEFAULT_RATE) {
        if (g_vra_active) {
            /* A previous call switched the codec to another rate; put the
             * 48 kHz default back before playing. */
            ac97_wr16(&g_nam, AC97_NAM_PCM_DAC_RATE, (uint16_t)rate);
        }
        return AC97_OK;
    }

    if (!g_vra) return AC97_ERR_ARG;      /* codec cannot do other rates */

    if (!g_vra_active) {
        const uint16_t ctrl = ac97_rd16(&g_nam, AC97_NAM_EXT_AUDIO_CTRL);
        ac97_wr16(&g_nam, AC97_NAM_EXT_AUDIO_CTRL, (uint16_t)(ctrl | 0x0001u));
        g_vra_active = 1;
    }
    ac97_wr16(&g_nam, AC97_NAM_PCM_DAC_RATE, (uint16_t)rate);
    return AC97_OK;
}

/* ===================== playback completion (polled) =====================
 * Interrupts are not available at boot time (the self-test runs before the
 * scheduler starts), so the wait is bounded by the 8254 channel-0 counter that
 * the boot path programmes to 100 Hz.  Rather than converting counter deltas
 * (mode 3 decrements by 2 per input clock, which makes the raw arithmetic
 * error-prone) the loop counts *reloads* of the counter: every time the
 * latched value jumps up, one full PIT period - 1/hz seconds - has elapsed.
 * The counter is polled far more often than once per period, so no reload can
 * be missed. */
#define AC97_WAIT_SLACK_MS    250u        /* extra time before "assume done" */
#define AC97_WAIT_TIMEOUT_MS  1500u       /* hard failure margin             */
#define AC97_POLL_STRIDE      256u        /* 1 PIT sample per 256 SR polls   */
#define AC97_POLL_BACKSTOP    100000000u  /* only if the PIT never advances  */

static uint32_t ac97_pit_ms_since(uint16_t *last, uint32_t *reloads,
                                 uint32_t period_ms)
{
    const uint16_t now = mbos_pit_counter();

    if (now > *last) (*reloads)++;      /* counter reloaded: one PIT period */
    *last = now;
    return (*reloads) * period_ms;
}

static int ac97_wait_completion(uint32_t samples, uint32_t rate)
{
    const uint32_t expect_ms = (uint32_t)(((uint64_t)samples * 1000ull) / rate);
    const uint32_t soft_ms = expect_ms + AC97_WAIT_SLACK_MS;
    const uint32_t hard_ms = expect_ms + AC97_WAIT_TIMEOUT_MS;
    uint32_t hz = mbos_pit_hz();
    uint32_t reloads = 0;
    uint32_t spins = 0;
    uint16_t last;

    if (hz == 0u) hz = 100u;
    last = mbos_pit_counter();

    for (;;) {
        if (ac97_rd16(&g_nabm, AC97_NABM_PO_SR) & AC97_PO_SR_DCH) return AC97_OK;

        spins++;
        if ((spins % AC97_POLL_STRIDE) == 0u) {
            const uint32_t elapsed =
                ac97_pit_ms_since(&last, &reloads, 1000u / hz);
            if (elapsed >= hard_ms) return AC97_ERR_DMA;
            if (elapsed >= soft_ms) {
                /* The engine never raised DCH, but the whole stream has had
                 * more than its playing time: every descriptor has certainly
                 * been consumed, so treat the playback as complete. */
                return AC97_OK;
            }
            if (spins >= AC97_POLL_BACKSTOP) return AC97_ERR_DMA;
        }
    }
}

/* ===================== one staged playback ===================== */
static int ac97_play_staged(uint32_t samples, uint32_t rate)
{
    const uint64_t stage_phys = (uint64_t)(uintptr_t)g_stage;
    const uint32_t bdl_phys = (uint32_t)(uintptr_t)g_bdl;
    uint32_t chunk, entries, i, off;
    int rc;

    if (!g_present) return AC97_ERR_IO;
    if (samples == 0u || samples > AC97_STAGE_SAMPLES) return AC97_ERR_ARG;
    if (stage_phys + (uint64_t)samples * 2u > 0xFFFFFFFFull) return AC97_ERR_IO;

    rc = ac97_program_rate(rate);
    if (rc != AC97_OK) return rc;

    /* The descriptor-list base may only be rewritten while the engine is
     * halted; the previous call always leaves it halted. */
    if (ac97_rd8(&g_nabm, AC97_NABM_PO_CR) & AC97_PO_CR_RPBM)
        ac97_wr8(&g_nabm, AC97_NABM_PO_CR, 0x00u);
    ac97_wr16(&g_nabm, AC97_NABM_PO_SR, AC97_PO_SR_CLEAR);

    /* Spread the stream over the whole descriptor list (<= 32 entries of at
     * most 0xFFFE 16-bit samples).  Chunks stay even so a stereo frame is
     * never split across two descriptors. */
    chunk = (samples + AC97_MAX_BDL - 1u) / AC97_MAX_BDL;
    if (chunk < 2u) chunk = 2u;
    chunk = (chunk + 1u) & ~1u;
    if (chunk > AC97_MAX_SAMPLES_PER_BD)
        chunk = AC97_MAX_SAMPLES_PER_BD & ~1u;

    entries = (samples + chunk - 1u) / chunk;
    if (entries == 0u || entries > AC97_MAX_BDL) return AC97_ERR_ARG;

    memset(g_bdl, 0, sizeof(g_bdl));
    off = 0;
    for (i = 0; i < entries; i++) {
        uint32_t n = samples - off;
        if (n > chunk) n = chunk;
        g_bdl[i].buffer_addr = (uint32_t)(stage_phys + (uint64_t)off * 2u);
        g_bdl[i].length      = (uint16_t)n;
        g_bdl[i].flags       = 0u;      /* no IOC: the driver polls, no IRQ */
        off += n;
    }

    ac97_wr32(&g_nabm, AC97_NABM_PO_BDBAR, bdl_phys);
    ac97_wr8(&g_nabm, AC97_NABM_PO_LVI, (uint8_t)(entries - 1u));
    ac97_wr8(&g_nabm, AC97_NABM_PO_CR, AC97_PO_CR_RPBM);

    rc = ac97_wait_completion(samples, rate);

    ac97_wr8(&g_nabm, AC97_NABM_PO_CR, 0x00u);          /* halt the engine   */
    ac97_wr16(&g_nabm, AC97_NABM_PO_SR, AC97_PO_SR_CLEAR);
    return rc;
}

/* ===================== public playback API ===================== */
int ac97_play_pcm(const int16_t *samples, uint32_t frames, uint32_t rate)
{
    if (!g_present || !samples) return AC97_ERR_IO;
    if (frames == 0u || frames > AC97_STAGE_SAMPLES / 2u) return AC97_ERR_ARG;

    /* The AC'97 DAC only consumes interleaved stereo; a mono stream is played
     * on both channels. */
    for (uint32_t i = 0; i < frames; i++) {
        g_stage[2u * i]      = samples[i];
        g_stage[2u * i + 1u] = samples[i];
    }
    return ac97_play_staged(frames * 2u, rate);
}

int ac97_play_pcm_stereo(const int16_t *interleaved, uint32_t frames, uint32_t rate)
{
    if (!g_present || !interleaved) return AC97_ERR_IO;
    if (frames == 0u || frames > AC97_STAGE_SAMPLES / 2u) return AC97_ERR_ARG;

    memcpy(g_stage, interleaved, (size_t)frames * 4u);
    return ac97_play_staged(frames * 2u, rate);
}

int ac97_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    uint32_t frames;

    if (!g_present) return AC97_ERR_IO;
    if (freq_hz < 20u || freq_hz > 12000u) return AC97_ERR_ARG;
    if (duration_ms < 10u || duration_ms > 1000u) return AC97_ERR_ARG;

    frames = ac97_tone_fill(freq_hz, duration_ms);
    return ac97_play_pcm(g_tone, frames, AC97_DEFAULT_RATE);
}

/* ===================== boot self-test ===================== */
int ac97_selftest(void)
{
    uint32_t frames;
    int rc;

    if (!g_present) {
        console_puts("[ac97] no codec (skipped)\n");
        return AC97_ERR_IO;
    }

    /* 440 Hz, 1.00 s, 48 kHz, 16-bit signed, 0.5 FS - the acceptance tone. */
    frames = ac97_tone_fill(440u, 1000u);
    rc = ac97_play_pcm(g_tone, frames, AC97_DEFAULT_RATE);
    if (rc != AC97_OK) {
        console_printf("[ac97] selftest: playback FAILED (rc=%d)\n", (int64_t)rc);
        return rc;
    }
    console_printf("[ac97] selftest: played %u frames @%u Hz (expect 1.000 s)\n",
                   (uint64_t)frames, (uint64_t)AC97_DEFAULT_RATE);
    return AC97_OK;
}
