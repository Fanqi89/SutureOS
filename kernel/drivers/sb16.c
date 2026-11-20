/* SutureOS module: Sound Blaster 16 (ISA) PCM output + 8237 DMA channel 1.
 * ----------------------------------------------------------------------------
 * NEW driver, self-written for SutureOS.  No source code was copied from the
 * reference projects; they were read to confirm the register order and the
 * hardware quirks only (details in sb16.h, "Source / provenance").
 *
 * The driver is deliberately small and polled: it is the piece that was
 * missing for the ISA sound path (AC'97 and Intel HDA already exist), and it
 * is verified on QEMU's `-device sb16`, which is strict about the 8237
 * programming (page register, flip-flop, count = length-1) and about the DSP
 * command handshake (0xC0 takes a format byte and then two length bytes).
 * ----------------------------------------------------------------------------
 * Implementation notes worth remembering
 *   - 0x22C read = DSP write-buffer status (bit 7 busy), 0x22E read = DSP
 *     read-buffer status (bit 7 data available).  0x22A carries the data.
 *     After a reset the DSP answers 0xAA on 0x22A: that is the probe
 *     signature (a machine without the card reads 0xFF everywhere instead).
 *   - 0xC0 = single-cycle 8-bit DMA output.  Its data bytes are
 *     <format> <length-1 lo> <length-1 hi>, where format 0x00 = unsigned mono
 *     8-bit.  Omitting the format byte (a common reading of the programming
 *     guide) makes the DSP wait for a third byte forever: the transfer never
 *     starts and the WAV capture stays empty even though the driver "finished"
 *     (first run of this port did exactly that on QEMU 11.1).
 *   - 8237: mode 0x45 = single transfer, address increment, memory->device,
 *     channel 1 (0x40 | 0x04 | 0x01).
 *   - completion is time-bounded with the 8254 counter reload trick used by
 *     ac97.c/hda.c: interrupts are off on the boot path and this driver never
 *     installs an IRQ handler, so it cannot wait on IRQ 5.
 */
#include "sb16.h"

#include <stitch/console.h>
#include <stitch/io.h>
#include <stitch/types.h>

#include "mb_pit.h"   /* 8254 counter: wall-clock bound without interrupts */

/* ===================== SB16 DSP (ISA fixed I/O base 0x220) ================= */
#define SB16_PORT_MIXER_ADDR  0x224u
#define SB16_PORT_MIXER_DATA  0x225u
#define SB16_PORT_DSP_RESET   0x226u
#define SB16_PORT_DSP_READ    0x22Au
#define SB16_PORT_DSP_WRITE   0x22Cu
#define SB16_PORT_DSP_WSTAT   0x22Cu   /* read: bit7 = write buffer busy    */
#define SB16_PORT_DSP_RSTAT   0x22Eu   /* read: bit7 = read data available  */
#define SB16_PORT_DSP_IRQACK  0x22Eu   /* read: acknowledges the 8-bit IRQ  */

#define SB16_DSP_CMD_SET_RATE    0x41u  /* + rate hi, rate lo               */
#define SB16_DSP_CMD_OUT8_SINGLE 0xC0u  /* + mode, length-1 lo, hi          */
#define SB16_DSP_CMD_SPEAKER_ON  0xD1u
#define SB16_DSP_CMD_GET_VERSION 0xE1u  /* -> major, minor                  */
/* The SB16 8-bit DMA commands (0xC0..0xC6) are "format + length" commands:
 * the first data byte is the format selector (bit5 stereo, bit4 signed), the
 * next two are the transfer length-1 as lo,hi.  0x00 = unsigned mono 8-bit.
 * Sending only the two length bytes leaves the DSP waiting for the third byte
 * and it never starts the transfer (QEMU's sb16 needs exactly these 3 bytes:
 * hw/audio/sb16.c, dma_cmd() over `s->cmd > 0xaf && s->cmd < 0xd0`). */
#define SB16_DSP_FMT_MONO_U8     0x00u

#define SB16_MIXER_MASTER_VOLUME 0x22u  /* bits 7:4 left, 3:0 right         */
#define SB16_MIXER_VOICE_VOLUME  0x04u

/* ===================== 8237 DMA (8-bit controller) ======================== */
#define SB16_DMA_CHANNEL      1u
#define SB16_DMA_MASK_PORT    0x0Au
#define SB16_DMA_MODE_PORT    0x0Bu
#define SB16_DMA_CLEAR_FF     0x0Cu
#define SB16_DMA_ADDR_PORT    0x02u     /* channel 1 address                */
#define SB16_DMA_COUNT_PORT   0x03u     /* channel 1 count                  */
#define SB16_DMA_PAGE_PORT    0x83u     /* channel 1 page                   */
/* 0x40 = single mode, bit5 = 0 (increment), bit4 = 0 (no auto-init),
 * bits 3:2 = 01 (write transfer: memory -> device), bits 1:0 = channel. */
#define SB16_DMA_MODE_SINGLE_WRITE (0x40u | 0x04u)
#define SB16_DMA_ADDR_LIMIT   0x1000000u   /* 8237 is a 24-bit address bus */

/* Transfer completion: playing time plus a generous margin.  The tone is the
 * first thing this driver plays, so the first DMA request can start a few
 * milliseconds late; the hard timeout only bounds a hung device. */
#define SB16_WAIT_SLACK_MS    250u
#define SB16_WAIT_TIMEOUT_MS  1500u
#define SB16_POLL_STRIDE      128u
/* Last-resort bound for a frozen PIT (the hard time bound normally fires
 * first).  Deliberately huge: a spin budget that is too small truncates a
 * transfer that is still playing (it did: 11025 samples cut to 0.39 s). */
#define SB16_POLL_BACKSTOP    2000000000u

/* Mixer register 0x82 = IRQ status.  Bit0 = 8-bit DMA transfer finished and
 * its interrupt is pending; reading the DSP read-status port (0x22E) then
 * acknowledges it.  This is the hardware's own completion signal, so the
 * driver does not have to guess from wall-clock time. */
#define SB16_MIXER_IRQ_STATUS 0x82u
#define SB16_MIXER_IRQ_8BIT   0x01u

/* Port handshake bounds: a missing ISA device reads 0xFF, so every wait must
 * be finite or a machine without a sound card would stall the boot. */
#define SB16_IO_TIMEOUT       20000u

/* ===================== state ============================================= */
static int      g_sb16_present;
static int      g_sb16_probed;
static uint8_t  g_sb16_major;
static uint8_t  g_sb16_minor;

/* The 8237 can only reach the low 16 MiB and an 8-bit transfer must not cross
 * a 64 KiB boundary, hence a 64 KiB aligned array of at most 64 KiB.  The
 * kernel is linked at 1 MiB and runs in a flat identity map, so the virtual
 * address is the physical address.  The array lives in the dedicated
 * .sb16_dma section registered in boot/linker.ld *before* .bss: .bss extends
 * past 16 MiB on this kernel (GUI caches), so a plain .bss symbol would be out
 * of the 8237's reach.  The tone is synthesised directly into this window
 * (8-bit unsigned mono = one byte per sample).
 */
static uint8_t  g_sb16_dma_buf[SB16_DMA_BYTES]
    __attribute__((section(".sb16_dma"), aligned(65536)));

/* ===================== small helpers ===================================== */
static void sb16_delay_us(uint32_t us)
{
    /* Port 0x80 write = one ISA bus cycle (~1 us on the machines this runs
     * on); a bounded busy loop is enough for the 100 us DSP reset pulse and
     * does not depend on the PIT being programmed yet. */
    uint32_t i;
    for (i = 0; i < us; i++) io_wait();
}

static int sb16_write_cmd(uint8_t value)
{
    uint32_t i;
    for (i = 0; i < SB16_IO_TIMEOUT; i++) {
        if ((inb(SB16_PORT_DSP_WSTAT) & 0x80u) == 0u) {
            outb(SB16_PORT_DSP_WRITE, value);
            return SB16_OK;
        }
    }
    return SB16_ERR_IO;
}

static int sb16_read_data(uint8_t *out)
{
    uint32_t i;
    for (i = 0; i < SB16_IO_TIMEOUT; i++) {
        if (inb(SB16_PORT_DSP_RSTAT) & 0x80u) {
            *out = inb(SB16_PORT_DSP_READ);
            return SB16_OK;
        }
    }
    return SB16_ERR_IO;
}

static void sb16_mixer_write(uint8_t reg, uint8_t value)
{
    outb(SB16_PORT_MIXER_ADDR, reg);
    outb(SB16_PORT_MIXER_DATA, value);
}

static uint8_t sb16_mixer_read(uint8_t reg)
{
    outb(SB16_PORT_MIXER_ADDR, reg);
    return inb(SB16_PORT_MIXER_DATA);
}

/* 1 when the 8-bit DMA transfer has finished (IRQ pending in the mixer). */
static int sb16_irq_pending(void)
{
    return (sb16_mixer_read(SB16_MIXER_IRQ_STATUS) & SB16_MIXER_IRQ_8BIT) != 0;
}

/* Append an unsigned decimal number to a small string; avoids depending on a
 * printf-family formatter at a point where the console is already up but the
 * name string must be static. */
static uint32_t sb16_name_num(char *dst, uint32_t pos, uint32_t value)
{
    char tmp[12];
    int n = 0;
    if (value == 0u) {
        dst[pos++] = '0';
        return pos;
    }
    while (value > 0u && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0) dst[pos++] = tmp[--n];
    return pos;
}

/* ===================== wall clock (8254 reloads) ========================= */
static uint32_t sb16_pit_ms_since(uint16_t *last, uint32_t *reloads,
                                  uint32_t period_ms)
{
    const uint16_t now = mbos_pit_counter();

    if (now > *last) (*reloads)++;      /* counter reloaded: one PIT period */
    *last = now;
    return (*reloads) * period_ms;
}

static int sb16_wait_transfer(uint32_t bytes, uint32_t rate)
{
    const uint32_t expect_ms = (uint32_t)(((uint64_t)bytes * 1000ull) / rate);
    const uint32_t soft_ms = expect_ms + SB16_WAIT_SLACK_MS;
    const uint32_t hard_ms = expect_ms + SB16_WAIT_TIMEOUT_MS;
    uint32_t hz = mbos_pit_hz();
    uint32_t reloads = 0;
    uint32_t spins = 0;
    uint16_t last;

    if (hz == 0u) hz = 100u;
    last = mbos_pit_counter();

    for (;;) {
        if ((spins % SB16_POLL_STRIDE) == 0u) {
            /* The DSP raises the 8-bit IRQ when the block is done; the mixer
             * status register is the only readable copy of it, and reading
             * the DSP read-status port (0x22E) acknowledges it. */
            if (sb16_irq_pending()) {
                (void)inb(SB16_PORT_DSP_IRQACK);
                return SB16_OK;
            }
            {
                const uint32_t elapsed =
                    sb16_pit_ms_since(&last, &reloads, 1000u / hz);
                if (elapsed >= hard_ms) return SB16_ERR_DMA;
                if (elapsed >= soft_ms) {
                    /* The transfer has had more than its playing time even
                     * though no IRQ was observed (a machine where the mixer
                     * status read does not work): treat it as complete. */
                    (void)inb(SB16_PORT_DSP_IRQACK);
                    return SB16_OK;
                }
            }
        }
        spins++;
        if (spins >= SB16_POLL_BACKSTOP) return SB16_ERR_DMA;
    }
}

/* ===================== 8237 programming ================================== */
static void sb16_dma_program(uint32_t phys, uint32_t bytes)
{
    const uint16_t offset = (uint16_t)(phys & 0xFFFFu);
    const uint16_t count  = (uint16_t)(bytes - 1u);
    const uint8_t  page   = (uint8_t)((phys >> 16) & 0xFFu);

    outb(SB16_DMA_MASK_PORT, (uint8_t)(0x04u | SB16_DMA_CHANNEL));  /* mask    */
    outb(SB16_DMA_CLEAR_FF,  0x00u);                                /* reset FF */
    outb(SB16_DMA_MODE_PORT,
         (uint8_t)(SB16_DMA_MODE_SINGLE_WRITE | SB16_DMA_CHANNEL));
    outb(SB16_DMA_ADDR_PORT,  (uint8_t)(offset & 0xFFu));
    outb(SB16_DMA_ADDR_PORT,  (uint8_t)((offset >> 8) & 0xFFu));
    outb(SB16_DMA_PAGE_PORT,  page);
    outb(SB16_DMA_COUNT_PORT, (uint8_t)(count & 0xFFu));
    outb(SB16_DMA_COUNT_PORT, (uint8_t)((count >> 8) & 0xFFu));
    outb(SB16_DMA_MASK_PORT,  (uint8_t)SB16_DMA_CHANNEL);           /* unmask  */
}

static void sb16_dma_stop(void)
{
    outb(SB16_DMA_MASK_PORT, (uint8_t)(0x04u | SB16_DMA_CHANNEL));
    /* Defensive IRQ acknowledge: the transfer-end interrupt is masked, but the
     * read clears the pending DSP state if the PIC line ever gets enabled. */
    (void)inb(SB16_PORT_DSP_IRQACK);
}

/* ===================== probe / bring-up ================================== */
static int sb16_dsp_reset(void)
{
    uint8_t sig = 0;

    outb(SB16_PORT_DSP_RESET, 1u);
    sb16_delay_us(100u);                /* the reset pulse must be > 3 us  */
    outb(SB16_PORT_DSP_RESET, 0u);
    sb16_delay_us(100u);

    if (sb16_read_data(&sig) != SB16_OK) return SB16_ERR_IO;
    return (sig == 0xAAu) ? SB16_OK : SB16_ERR_IO;
}

int sb16_init(void)
{
    uint8_t major = 0, minor = 0;
    uint32_t phys;

    if (g_sb16_probed) return g_sb16_present ? SB16_OK : SB16_ERR_IO;
    g_sb16_probed = 1;

    if (sb16_dsp_reset() != SB16_OK) {
        console_puts("[sb16] no DSP (not present)\n");
        g_sb16_present = 0;
        return SB16_ERR_IO;
    }

    /* Version: 0xE1 answers major then minor (QEMU's sb16: 4.5). */
    if (sb16_write_cmd(SB16_DSP_CMD_GET_VERSION) != SB16_OK ||
        sb16_read_data(&major) != SB16_OK ||
        sb16_read_data(&minor) != SB16_OK) {
        console_puts("[sb16] DSP version read failed -> unusable\n");
        g_sb16_present = 0;
        return SB16_ERR_IO;
    }
    g_sb16_major = major;
    g_sb16_minor = minor;

    console_printf("[sb16] dsp reset ok version=%u.%u\n",
                   (uint64_t)major, (uint64_t)minor);

    /* Unmute the mixer paths the DSP output goes through.  On the SB16 a
     * volume nibble of 0 is maximum gain; these registers can reset muted on
     * some machines, and a muted mixer looks exactly like a broken DMA path
     * in a WAV capture. */
    sb16_mixer_write(SB16_MIXER_MASTER_VOLUME, 0x00u);
    sb16_mixer_write(SB16_MIXER_VOICE_VOLUME,  0x00u);

    (void)sb16_write_cmd(SB16_DSP_CMD_SPEAKER_ON);

    /* Report the DMA window once: if the kernel ever grows past 16 MiB this
     * line becomes the explanation instead of a silent no-sound bug. */
    phys = (uint32_t)(uintptr_t)g_sb16_dma_buf;
    console_printf("[sb16] dma buffer @0x%x (%u bytes, %s, align %u)\n",
                   (uint64_t)phys, (uint64_t)SB16_DMA_BYTES,
                   (phys + SB16_DMA_BYTES <= SB16_DMA_ADDR_LIMIT)
                       ? "in 24-bit range" : "OUT OF RANGE",
                   (uint64_t)(phys % 65536u));

    /* 22050 Hz, 8-bit, mono: programmed once here; 0xC0 re-uses it. */
    if (sb16_write_cmd(SB16_DSP_CMD_SET_RATE) != SB16_OK ||
        sb16_write_cmd((uint8_t)((SB16_RATE_HZ >> 8) & 0xFFu)) != SB16_OK ||
        sb16_write_cmd((uint8_t)(SB16_RATE_HZ & 0xFFu)) != SB16_OK) {
        console_puts("[sb16] sample-rate command failed -> unusable\n");
        g_sb16_present = 0;
        return SB16_ERR_IO;
    }

    g_sb16_present = 1;
    return SB16_OK;
}

int sb16_present(void)
{
    return g_sb16_present;
}

const char *sb16_name(void)
{
    static const char *absent = "Sound Blaster 16 (absent)";
    static char name[96];
    uint32_t p = 0;

    if (!g_sb16_present) return absent;

    {
        const char *pre = "Sound Blaster 16 DSP ";
        const char *post = " @0x220, DMA 1, 8-bit mono 22050 Hz";
        while (*pre && p < 60u) name[p++] = *pre++;
        p = sb16_name_num(name, p, (uint32_t)g_sb16_major);
        name[p++] = '.';
        p = sb16_name_num(name, p, (uint32_t)g_sb16_minor);
        while (*post && p < 94u) name[p++] = *post++;
        name[p] = '\0';
    }
    return name;
}

/* ===================== tone synthesis + playback ========================= */
/* Trailing silence appended to every transfer.  QEMU's audio backend keeps a
 * large chunk of already-mixed samples in its own buffer and drops the tail
 * when the last voice goes inactive, so a transfer that ends exactly with the
 * tone loses part of it in a WAV capture.  Measured on QEMU 11.1:
 *   - no drain      -> a 0.500 s tone was recorded as 0.395 s
 *   - 200 ms drain  -> 0.469 s (still short)
 *   - 400 ms drain  -> exactly 0.500 s (24 000 frames at 48 kHz)
 * This is the same "let the backend drain" trick the HDA driver uses. */
#define SB16_DRAIN_MS 400u

/* Fill the DMA window with an 8-bit UNSIGNED mono square wave.  A phase
 * accumulator is used instead of a sample period because 22050/440 is not an
 * integer: a truncated period would drift the pitch.  Returns the number of
 * tone samples (== DMA bytes for 8-bit mono). */
static uint32_t sb16_tone_fill(uint32_t freq_hz, uint32_t duration_ms)
{
    uint32_t total;
    uint32_t acc = 0;
    uint32_t high = 0;
    uint32_t i;

    if (duration_ms > SB16_MAX_MS) duration_ms = SB16_MAX_MS;
    total = (SB16_RATE_HZ * duration_ms) / 1000u;
    if (total > SB16_DMA_BYTES) total = SB16_DMA_BYTES;

    for (i = 0; i < total; i++) {
        g_sb16_dma_buf[i] = (uint8_t)(high ? (SB16_TONE_MID + SB16_TONE_AMP)
                                          : (SB16_TONE_MID - SB16_TONE_AMP));
        /* One square-wave period = two level changes, so the accumulator must
         * advance by 2*freq per sample: toggling once per period produces a
         * tone at half the requested frequency (measured 220 Hz for a 440 Hz
         * request on the first WAV capture of this port). */
        acc += freq_hz * 2u;
        while (acc >= SB16_RATE_HZ) {
            acc -= SB16_RATE_HZ;
            high ^= 1u;
        }
    }
    return total;
}

int sb16_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    uint32_t bytes, phys;
    int rc;

    if (!g_sb16_present) {
        if (sb16_init() != SB16_OK) return SB16_ERR_IO;
    }
    if (freq_hz < 20u) freq_hz = 20u;
    if (freq_hz > 12000u) freq_hz = 12000u;
    if (duration_ms < 10u) duration_ms = 10u;
    if (duration_ms > SB16_MAX_MS) duration_ms = SB16_MAX_MS;

    phys = (uint32_t)(uintptr_t)g_sb16_dma_buf;
    if (phys + SB16_DMA_BYTES > SB16_DMA_ADDR_LIMIT) {
        console_printf("[sb16] dma buffer 0x%x is above the 16 MiB 8237 limit\n",
                       (uint64_t)phys);
        return SB16_ERR_DMA;
    }
    /* The 8237 needs a page-aligned window so a single transfer cannot cross a
     * 64 KiB boundary; the static array is declared aligned(65536). */
    if (phys % 65536u != 0u) {
        console_printf("[sb16] dma buffer 0x%x is not 64 KiB aligned\n",
                       (uint64_t)phys);
        return SB16_ERR_DMA;
    }

    bytes = sb16_tone_fill(freq_hz, duration_ms);
    if (bytes == 0u || bytes > SB16_DMA_BYTES) return SB16_ERR_ARG;

    /* Append the silence drain (see SB16_DRAIN_MS). */
    {
        const uint32_t tone_bytes = bytes;
        uint32_t drain = (SB16_RATE_HZ * SB16_DRAIN_MS) / 1000u;
        uint32_t i;

        if (tone_bytes + drain > SB16_DMA_BYTES) drain = SB16_DMA_BYTES - tone_bytes;
        for (i = tone_bytes; i < tone_bytes + drain; i++)
            g_sb16_dma_buf[i] = (uint8_t)SB16_TONE_MID;
        bytes = tone_bytes + drain;

        console_printf("[sb16] beep %uHz %ums (%u Hz, 8-bit mono)\n",
                       (uint64_t)freq_hz, (uint64_t)duration_ms,
                       (uint64_t)SB16_RATE_HZ);
        console_printf("[sb16] dma: base=0x%x count=%u (page=0x%x) tone=%u\n",
                       (uint64_t)phys, (uint64_t)(bytes - 1u),
                       (uint64_t)((phys >> 16) & 0xFFu),
                       (uint64_t)tone_bytes);
    }

    sb16_dma_program(phys, bytes);

    /* Clear any stale transfer-end state from the reset/probe and make sure
     * the speaker is on (a single-cycle transfer switches it off when it
     * ends, so every beep re-enables it). */
    if (sb16_irq_pending()) (void)inb(SB16_PORT_DSP_IRQACK);
    (void)sb16_write_cmd(SB16_DSP_CMD_SPEAKER_ON);

    rc = sb16_write_cmd(SB16_DSP_CMD_OUT8_SINGLE);
    if (rc == SB16_OK) rc = sb16_write_cmd(SB16_DSP_FMT_MONO_U8);
    if (rc == SB16_OK) rc = sb16_write_cmd((uint8_t)((bytes - 1u) & 0xFFu));
    if (rc == SB16_OK) rc = sb16_write_cmd((uint8_t)(((bytes - 1u) >> 8) & 0xFFu));
    if (rc != SB16_OK) {
        sb16_dma_stop();
        console_puts("[sb16] DSP rejected the 0xC0 command\n");
        return SB16_ERR_IO;
    }

    rc = sb16_wait_transfer(bytes, SB16_RATE_HZ);
    sb16_dma_stop();

    if (rc != SB16_OK) {
        console_printf("[sb16] beep timed out after %u bytes\n",
                       (uint64_t)bytes);
        return rc;
    }
    console_puts("[sb16] beep done\n");
    return SB16_OK;
}

/* ===================== self test ========================================= */
int sb16_selftest(void)
{
    uint32_t n;
    int rc;

    if (sb16_init() != SB16_OK) {
        console_puts("[sb16] no DSP (skipped)\n");
        return SB16_ERR_IO;
    }

    n = (SB16_RATE_HZ * 1000u) / 1000u;
    rc = sb16_beep(440u, 1000u);
    if (rc != SB16_OK) {
        console_printf("[sb16] selftest: playback FAILED (rc=%d)\n", (int64_t)rc);
        return rc;
    }
    console_printf("[sb16] selftest: played %u samples @%u Hz (expect 1.000 s)\n",
                   (uint64_t)n, (uint64_t)SB16_RATE_HZ);
    return SB16_OK;
}
