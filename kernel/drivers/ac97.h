/* SutureOS module: AC'97 audio controller driver - PCM output.
 * ============================================================================
 * Source / provenance
 *   Ported from: HBOS - src/ac97.h + src/ac97.c
 *                (local copy: C:/Users/fanqi/Desktop/自研操作系统/HBOS/src/ac97.c)
 *   Upstream license: GPL-3.0.  SutureOS is GPL-3.0 too, so this port keeps the
 *   same license and the same spirit of the reference implementation.
 *   The reference implementation was used as an algorithm/programming-model
 *   reference (PCI probe of the 82801AA controller, NAM codec reset + unmute,
 *   NABM PCM-OUT Buffer-Descriptor-List programming).  The code below was
 *   re-written against SutureOS' own kernel APIs; the differences are listed
 *   at the top of ac97.c.
 * ============================================================================
 * What this driver does
 *   - finds the Intel 82801AA (vendor 0x8086, device 0x2415) AC'97 controller
 *     on PCI bus 0, plus the compatible southbridge AC'97 IDs (0x2425 ...) and
 *     a generic "multimedia/audio, program interface 0" fallback;
 *   - enables I/O decoding + bus mastering, resets the AC'97 codec through the
 *     Native Audio Mixer (NAM, BAR0), clears the mute bits and sets the
 *     Master/PCM-OUT volumes to 0 dB;
 *   - drives the PCM-OUT channel of the Native Audio Bus Master (NABM, BAR1,
 *     registers at offset 0x10) with a <=32-entry Buffer Descriptor List that
 *     points into static .bss memory (the kernel runs in a flat identity map,
 *     so physical == virtual) and plays 16-bit signed PCM in a blocking way.
 * ============================================================================
 * Public API
 *   ac97_init()        probe + bring-up; 0 = a controller is ready, -1 = none.
 *                      Completely silent when no controller exists (the boot
 *                      path must stay free of side effects on such machines).
 *   ac97_present()     1 when ac97_init() succeeded.
 *   ac97_name()        one-line human readable description ("Intel 82801AA
 *                      (ICH) AC'97, NAM 0xc000 NABM 0xc400"), used by the
 *                      boot log.
 *   ac97_play_pcm()    play a MONO 16-bit signed stream (frames = mono
 *                      frames); the driver duplicates each sample into L+R
 *                      because the AC'97 DAC expects interleaved stereo.
 *                      Blocks until the last descriptor has been consumed.
 *   ac97_play_pcm_stereo() same, but for interleaved L,R,L,R,... input.
 *   ac97_beep()        synthesise and play a sine tone (used by the boot
 *                      self-test and the shell's `beep` command).
 *   ac97_selftest()    boot self-test: 440 Hz / 1.00 s / 48 kHz / 16-bit /
 *                      0.5 FS sine, then prints the acceptance line.
 * ============================================================================
 * Limits (deliberate, documented)
 *   - one call stages at most 2.0 s of 48 kHz 16-bit stereo (static DMA
 *     buffer), longer material has to be split by the caller;
 *   - native rate is 48000 Hz (the AC'97 power-on default).  Other rates are
 *     programmed through Variable Rate Audio (NAM 0x2A/0x2C) when the codec
 *     advertises VRA, otherwise they are rejected instead of being played at
 *     the wrong speed;
 *   - polled (no INTx): the driver waits on the NABM status register DCH bit,
 *     bounded by wall-clock time measured with the 8254 PIT counter, because
 *     the boot self-test runs before the scheduler/interrupts are up.
 */
#ifndef SUTUREOS_AC97_H
#define SUTUREOS_AC97_H

#include <stitch/types.h>

#define AC97_OK        0
#define AC97_ERR_IO   (-1)   /* no controller / register access failed      */
#define AC97_ERR_ARG  (-2)   /* bad pointer, frame count, rate or length    */
#define AC97_ERR_DMA  (-3)   /* descriptor list / completion timeout         */

#define AC97_DEFAULT_RATE 48000u          /* AC'97 power-on DAC rate (Hz)    */
#define AC97_TONE_AMPLITUDE 16384         /* 0.5 FS for 16-bit samples       */

/* Probe PCI, bring up the codec and the PCM-OUT bus master.
 * Returns 0 when a controller is present and ready, -1 otherwise.
 * Silent on failure: a machine without AC'97 must see no log line and no
 * added boot latency from this driver.  Idempotent (a second call is a no-op
 * that returns the cached result). */
int ac97_init(void);

/* 1 when ac97_init() found and initialised a controller. */
int ac97_present(void);

/* One-line description of the controller (valid after ac97_init()). */
const char *ac97_name(void);

/* Play a mono 16-bit signed PCM stream.  `frames` counts mono sample frames
 * (i.e. `frames` int16_t values in `samples`); each value is played on both
 * channels.  `rate` is in Hz (48000 native, see the header comment for the
 * other rates).  Blocks until the DMA engine reports the last descriptor as
 * consumed (bounded by wall-clock time + margin).
 * Returns AC97_OK, or a negative AC97_ERR_* code. */
int ac97_play_pcm(const int16_t *samples, uint32_t frames, uint32_t rate);

/* Play interleaved stereo 16-bit PCM (L,R,L,R...).  `frames` counts stereo
 * frames, so the buffer holds `frames * 2` int16_t values. */
int ac97_play_pcm_stereo(const int16_t *interleaved, uint32_t frames, uint32_t rate);

/* Synthesise and play a sine tone at AC97_DEFAULT_RATE with 0.5 FS amplitude.
 * freq_hz is clamped to 20..12000 Hz, duration_ms to 10..1000 ms. */
int ac97_beep(uint32_t freq_hz, uint32_t duration_ms);

/* Boot self-test.  Without a controller it prints exactly
 *   [ac97] no codec (skipped)
 * and returns -1.  With a controller it plays a 440 Hz / 1.00 s / 48 kHz /
 * 16-bit / 0.5 FS sine and prints
 *   [ac97] selftest: played 48000 frames @48000 Hz (expect 1.000 s) */
int ac97_selftest(void);

#endif /* SUTUREOS_AC97_H */
