/* SutureOS module: Sound Blaster 16 (ISA) PCM output + 8237 DMA channel 1.
 * ============================================================================
 * Source / provenance
 *   NEW driver written for SutureOS (self-written, no code copied).
 *
 *   Mechanism / timing reference (read only, no code taken):
 *     - Neumann (C:\Users\fanqi\Desktop\自研操作系统\Neumann)
 *         kernel/drivers/audio/sb16/sb16.c  (DSP reset -> status handshake,
 *         speaker-on, 0x41 sample-rate command, 0xC0 single-cycle 8-bit DMA,
 *         IRQ acknowledge through 0x22F)
 *         kernel/drivers/dma/dma.c         (8237 channel/page/address/count
 *         programming, 24-bit address limit, no 64 KiB boundary crossing)
 *     - Powerint-DOS-386 (C:\Users\fanqi\Desktop\自研操作系统\Powerint-DOS-386)
 *         kernel/drivers/sb16.c, kernel/drivers/dma.c  (same 8237 register
 *         sequence; mode 0x48/0x44 + channel, mask/flip-flop ports)
 *   Both projects were only read to confirm the register order and the
 *   hardware quirks listed below; every line here is written against
 *   SutureOS' own kernel APIs (port I/O helpers, PIT time base, console log).
 *
 *   The SB16 register-level ABI itself is public hardware documentation
 *   (Creative Labs SB16 Programming Guide; 8237A datasheet), not code.
 * ============================================================================
 * What this driver does
 *   - probes the SB16 DSP at the ISA fixed base 0x220 by resetting it
 *     (write 1 to 0x226, wait, write 0) and reading back 0xAA from 0x22A
 *     after polling the read-status port 0x22E for bit 7;
 *   - reads the DSP version (0xE1) and switches the speaker on (0xD1);
 *   - plays 8-bit UNSIGNED mono PCM at 22050 Hz through DMA channel 1:
 *       * sets the sample rate with 0x41 <hi> <lo>;
 *       * programmes the 8237 controller (mask 0x0A, flip-flop 0x0C, mode
 *         0x0B = single / address increment / memory->device / channel 1,
 *         address 0x02 lo+hi, page 0x83, count 0x03 lo+hi where count =
 *         bytes-1, then unmasks the channel);
 *       * starts a single-cycle 8-bit output with 0xC0 <format> <lo> <hi>
 *         where format 0x00 = unsigned mono 8-bit and lo/hi are bytes-1 (the
 *         format byte is mandatory; without it the DSP never starts);
 *       * waits for the transfer to finish with the same wall-clock scheme
 *         the AC'97/HDA drivers use (8254 counter reloads), because
 *         interrupts are off during the boot path and this driver never
 *         installs an IRQ handler.
 * ============================================================================
 * Hardware constraints honoured here
 *   - 8237 address is 24 bit: the DMA buffer must live below 16 MiB.  The
 *     buffer is a static .bss array aligned to 64 KiB, and sb16_init() logs
 *     its physical address so a regression is visible immediately.  If the
 *     address is out of reach the driver refuses to play instead of driving
 *     the controller with a truncated page (silent wrong playback).
 *   - an 8-bit 8237 transfer must not cross a 64 KiB boundary; a 64 KiB
 *     aligned buffer that is at most 64 KiB long cannot cross one.
 *   - 8-bit DMA uses channel 1 (SB16 defaults: irq 5, dma 1, dma16 5).
 * ============================================================================
 * Public API
 *   sb16_init()      probe + bring-up.  0 = a DSP answered 0xAA, -1 = none.
 *                    Idempotent; prints
 *                      "[sb16] dsp reset ok version=4.5"          (present)
 *                      "[sb16] no DSP (not present)"               (absent)
 *   sb16_present()   1 once sb16_init() succeeded.
 *   sb16_name()      one-line description for the boot log.
 *   sb16_beep()      synthesise and DMA-play a square tone (8-bit unsigned
 *                    mono, 22050 Hz).  Blocks until the transfer has had its
 *                    playing time (bounded wait).
 *   sb16_selftest()  boot-style self test (440 Hz / 1.00 s).  NOT called from
 *                    the boot path on purpose: the acceptance tone is played
 *                    from the shell (`sb16test`) so the boot self-test tally
 *                    stays at the frozen 16/16.
 * ============================================================================
 * Limits (deliberate, documented)
 *   - one call stages at most 1000 ms of tone (32768-byte static DMA buffer;
 *     the transfer also carries a 400 ms silence tail that lets QEMU's audio
 *     backend flush the tone into the WAV, so a 1 s tone uses 30870 bytes);
 *   - 8-bit unsigned MONO only, 22050 Hz (the simplest SB16 mode, as agreed
 *     for this port); 16-bit stereo is not implemented;
 *   - completion is polled from the mixer IRQ-status register 0x82 with the
 *     8254 time bound as a fallback; no IRQ handler is installed, so IRQ 5
 *     stays masked and the pending interrupt is acknowledged by reading 0x22E;
 *   - verified on QEMU's -device sb16 only; no real ISA sound card was
 *     available on the build machine.
 */
#ifndef SUTUREOS_SB16_H
#define SUTUREOS_SB16_H

#include <stitch/types.h>

#define SB16_OK         0
#define SB16_ERR_IO   (-1)   /* no DSP / register handshake failed          */
#define SB16_ERR_ARG  (-2)   /* bad length, rate or duration                */
#define SB16_ERR_DMA  (-3)   /* DMA completion timeout                      */

/* Default (and only) PCM format: 8-bit unsigned mono. */
#define SB16_RATE_HZ       22050u
#define SB16_CHANNELS      1u
#define SB16_BITS          8u
#define SB16_DMA_BYTES     32768u        /* static DMA buffer, 64 KiB aligned */
#define SB16_MAX_MS        1000u         /* buffer caps one call at 1 s      */
#define SB16_TONE_MID      128u          /* 8-bit unsigned silence           */
#define SB16_TONE_AMP      64            /* 0.5 FS square wave               */

/* Probe the ISA DSP at 0x220 and bring it up.  0 on success, -1 if no DSP
 * answered.  Idempotent (later calls return the cached result and print
 * nothing). */
int sb16_init(void);

/* 1 when sb16_init() found and initialised a DSP. */
int sb16_present(void);

/* One-line description (valid after sb16_init()). */
const char *sb16_name(void);

/* Synthesise a square tone (8-bit unsigned mono, SB16_RATE_HZ) and play it
 * through DMA channel 1.  freq_hz is clamped to 20..12000, duration_ms to
 * 10..SB16_MAX_MS.  Blocks until the transfer has had its playing time.
 * Returns SB16_OK or a negative SB16_ERR_* code. */
int sb16_beep(uint32_t freq_hz, uint32_t duration_ms);

/* Boot-style self test: 440 Hz / 1.00 s, prints
 *   "[sb16] selftest: played 22050 samples @22050 Hz (expect 1.000 s)"
 * or "[sb16] no DSP (skipped)" when absent. */
int sb16_selftest(void);

#endif /* SUTUREOS_SB16_H */
