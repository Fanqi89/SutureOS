/* SutureOS module: Intel High Definition Audio (HDA) controller + codec driver.
 * ============================================================================
 * Source / provenance
 *   Algorithm reference: PlantOS - src/kernel/drivers/sound/hda.c
 *                         (local copy:
 *                          C:/Users/fanqi/Desktop/自研操作系统/PlantOS/src/kernel/
 *                          drivers/sound/hda.c)
 *   PlantOS is MIT (Copyright (c) 2024 plos-clan), so the port is license
 *   compatible with SutureOS (GPL-3.0).  It was used as the programming-model
 *   reference for: the PCI probe of the HDA controller (8086:2668 and the
 *   related Intel southbridge/ICH IDs), the 0x80-based stream-descriptor
 *   layout, the CORB/RIRB command-ring model and the "walk the widgets from the
 *   Audio Function Group" codec enumeration idea.
 *
 *   Hardware/register ordering and the failure criteria were cross-checked
 *   against the Intel HDA 1.0a specification and the (Apache-2.0) ReliefOS HDA
 *   design/handoff notes only; no ReliefOS code is copied.
 *
 *   Everything below is written against SutureOS' own kernel APIs; the
 *   differences from the reference are listed at the top of hda.c.
 * ============================================================================
 * What this driver does
 *   - finds the HDA controller on PCI bus 0 (class 0x04/0x03 or a known Intel
 *     device id), enables memory decode + bus mastering and maps BAR0 with
 *     mmu_map_mmio_identity() (the BAR is usually above 1 GiB on QEMU/real
 *     machines and is not in the boot identity map);
 *   - resets the controller (GCTL.CRST handshake), brings up the CORB/RIRB
 *     command rings and speaks HDA verbs over them (polled: no interrupts -
 *     the boot self-test runs before the scheduler is alive and this kernel
 *     keeps audio polling-only on purpose);
 *   - enumerates the codec(s) from STATESTS, reads the vendor/device id, walks
 *     the Audio Function Group widgets, resolves the output pin -> converter
 *     path and programs it for 48 kHz / 16-bit / 2 channels;
 *   - plays interleaved 16-bit stereo PCM through output stream descriptor 0
 *     with a small Buffer Descriptor List carved out of static .bss memory,
 *     polling SDLPIB until the stream has been consumed (bounded by the 8254
 *     PIT counter, because interrupts are off during boot).
 * ============================================================================
 * Public API
 *   hda_init()        probe + controller reset + codec bring-up.
 *                     0 = a codec is ready, HDA_ERR_NODEV = no controller.
 *                     A machine without HDA sees exactly one added boot line:
 *                       [hda] no controller
 *                     (and no additional boot latency worth measuring: the
 *                     probe only walks PCI bus 0).
 *   hda_present()     1 when hda_init() found and programmed a codec.
 *   hda_name()        one-line controller/codec description for the boot log.
 *   hda_play_pcm()    play interleaved 16-bit stereo PCM at 48 kHz.  `frames`
 *                     counts stereo frames (frames * 2 int16_t values).
 *                     Blocks until the stream has played.
 *   hda_beep()        synthesise and play a sine tone (440 Hz/1000 ms is the
 *                     boot self-test tone, also used by the shell `hdatest`).
 *   hda_selftest()    boot self-test: 440 Hz / 1.00 s / 48 kHz / 16-bit /
 *                     0.5 FS, then prints the acceptance line.  Silent when no
 *                     controller was found (hda_init() already said so).
 * ============================================================================
 * Limits (deliberate, documented)
 *   - one call stages at most 1.0 s of 48 kHz 16-bit stereo (static DMA
 *     buffer); longer material has to be split by the caller;
 *   - only the native 48 kHz / 16-bit / stereo format is programmed; other
 *     rates are refused instead of being played at the wrong speed;
 *   - one codec (the first one that answers), one output pin, one converter
 *     (the first converter on that pin's connection path) - enough for the
 *     QEMU `intel-hda` + `hda-duplex` pair and for a typical single-codec
 *     notebook, not a general multi-codec mixer;
 *   - no interrupts, no capture, no volume controls (amps are opened at 0 dB
 *     and unmuted so the tone is audible).
 */
#ifndef SUTUREOS_HDA_H
#define SUTUREOS_HDA_H

#include <stitch/types.h>

#define HDA_OK            0
#define HDA_ERR_IO      (-1)   /* no controller / register or verb failure   */
#define HDA_ERR_ARG     (-2)   /* bad pointer, frame count or format         */
#define HDA_ERR_DMA     (-3)   /* stream descriptor timeout                  */
#define HDA_ERR_NODEV   (-4)   /* no HDA controller on the bus               */

#define HDA_DEFAULT_RATE     48000u   /* the only rate this driver programs */
#define HDA_MAX_FRAMES       48000u   /* 1.0 s of 48 kHz interleaved stereo */
#define HDA_TONE_AMPLITUDE   16384    /* 0.5 FS for 16-bit samples          */

/* Format word: 2 channels, 16 bits per sample, 48 kHz base rate. */
#define HDA_FMT_48K_16BIT_STEREO 0x0011u

/* Probe PCI, reset the controller, enumerate the codec and program the
 * pin/converter path for 48 kHz 16-bit stereo.
 * Returns HDA_OK when a codec is ready, HDA_ERR_NODEV when there is no
 * controller (prints the single "[hda] no controller" line) and a negative
 * HDA_ERR_* code when a controller exists but could not be brought up.
 * Idempotent (a second call returns the cached result). */
int hda_init(void);

/* 1 when hda_init() succeeded. */
int hda_present(void);

/* One-line description (valid after hda_init(); empty before). */
const char *hda_name(void);

/* Play interleaved 16-bit signed stereo PCM at HDA_DEFAULT_RATE.
 * `frames` counts stereo frames, so the buffer holds frames * 2 int16_t
 * values.  Blocks until the stream descriptor reports completion (bounded by
 * the PIT counter).  Returns HDA_OK or a negative HDA_ERR_* code. */
int hda_play_pcm(const int16_t *interleaved, uint32_t frames);

/* Synthesise and play a sine tone at 48 kHz / 16-bit / stereo, 0.5 FS.
 * freq_hz is clamped to 20..12000 Hz, duration_ms to 10..1000 ms. */
int hda_beep(uint32_t freq_hz, uint32_t duration_ms);

/* Boot self-test: 440 Hz / 1000 ms acceptance tone.  Without a controller it
 * prints nothing and returns HDA_ERR_NODEV.  With one it prints
 *   [hda] beep 440Hz 1000ms
 *   [hda] selftest: PASS played 48000 frames @48000 Hz (expect 1.000 s) */
int hda_selftest(void);

#endif /* SUTUREOS_HDA_H */
