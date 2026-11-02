#!/usr/bin/env python3
"""Verify the AC'97 PCM-output acceptance evidence (a WAV written by QEMU).

QEMU's wav audio backend records everything the emulated AC'97 codec plays from
machine start until QEMU quits, so the file normally contains boot-time silence,
one 440 Hz / 1.00 s tone from SutureOS' boot self-test (and, in a run where the
shell `beep` command was typed, a second tone).  This script locates the tone
segment(s) and checks the acceptance numbers on the longest one:

    * sample rate        == 48000 Hz
    * tone duration      == 1.00 s +/- 0.05 s
    * peak amplitude     >= 0.1 FS
    * zero-crossing freq == 440 Hz +/- 22 Hz

Usage:
    py -3 scripts/verify_ac97_wav.py build/audio.wav [--segment N]

Exit code 0 = every check passed (the numbers are printed either way).
"""

import argparse
import array
import os
import sys

EXPECT_RATE = 48000
EXPECT_DURATION_S = 1.00
DURATION_TOL_S = 0.05
MIN_PEAK_FS = 0.10
EXPECT_FREQ_HZ = 440.0
FREQ_TOL_HZ = 22.0

FULL_SCALE = 32768.0
# A tone sample is "active" above 1% of full scale; the self-test tone is at
# 0.5 FS, so this is far away from both silence and the noise floor.
ACTIVE_THRESHOLD = int(0.01 * FULL_SCALE)
# Silence shorter than this does not split a tone into two segments.
SEGMENT_GAP_S = 0.05
# Anything shorter than this is treated as a click, not a tone.
MIN_SEGMENT_S = 0.10


class Wav:
    def __init__(self, path, rate, channels, bits, fmt, payload):
        self.path = path
        self.rate = rate
        self.channels = channels
        self.bits = bits
        self.format = fmt
        self.payload = payload


def read_wav(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    if len(raw) < 44 or raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise SystemExit("%s: not a RIFF/WAVE file" % path)

    pos = 12
    fmt_chunk = None
    payload = None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        csize = int.from_bytes(raw[pos + 4:pos + 8], "little")
        body = raw[pos + 8:pos + 8 + csize]
        if cid == b"fmt ":
            fmt_chunk = body
        elif cid == b"data":
            # QEMU writes the sample data but does not always update the RIFF
            # and data sizes on close (they stay 0 / 0xFFFFFFFF).  In that case
            # everything after the data chunk header is audio.
            if csize == 0 or csize == 0xFFFFFFFF or csize > len(raw) - (pos + 8):
                payload = raw[pos + 8:]
            else:
                payload = body
            break
        pos += 8 + csize + (csize & 1)

    if fmt_chunk is None or payload is None:
        raise SystemExit("%s: missing fmt/data chunk" % path)
    if len(fmt_chunk) < 16:
        raise SystemExit("%s: short fmt chunk" % path)

    fmt = int.from_bytes(fmt_chunk[0:2], "little")
    channels = int.from_bytes(fmt_chunk[2:4], "little")
    rate = int.from_bytes(fmt_chunk[4:8], "little")
    bits = int.from_bytes(fmt_chunk[14:16], "little")

    if fmt != 1:
        raise SystemExit("%s: expected PCM (format 1), got format %d" % (path, fmt))
    if bits != 16:
        raise SystemExit("%s: expected 16-bit samples, got %d-bit" % (path, bits))
    if channels < 1:
        raise SystemExit("%s: bad channel count %d" % (path, channels))

    samples = array.array("h")
    samples.frombytes(payload[:len(payload) & ~1])
    if sys.byteorder == "big":
        samples.byteswap()
    return Wav(path, rate, channels, bits, fmt, samples)


def channel0(samples, channels):
    if channels == 1:
        return samples
    return samples[0::channels]


def find_segments(samples, thr, gap, min_len):
    """Return [(start, end)] sample ranges whose |value| exceeds `thr`."""
    segments = []
    run_start = None
    last_active = None
    for i, v in enumerate(samples):
        if v > thr or v < -thr:
            if run_start is None:
                run_start = i
            elif i - last_active > gap:
                segments.append((run_start, last_active + 1))
                run_start = i
            last_active = i
    if run_start is not None:
        segments.append((run_start, last_active + 1))
    return [s for s in segments if s[1] - s[0] >= min_len]


def zero_cross_freq(seg, rate, hyst):
    """Frequency from interpolated positive-going zero crossings."""
    positions = []
    state = -1
    prev_i = None
    prev_v = None
    for i, v in enumerate(seg):
        if state < 0:
            if v >= hyst:
                if prev_v is not None and v != prev_v:
                    frac = (0.0 - prev_v) / float(v - prev_v)
                    positions.append(prev_i + frac)
                else:
                    positions.append(float(i))
                state = 1
        elif v <= -hyst:
            state = -1
        prev_i = i
        prev_v = v
    if len(positions) < 3:
        return None, len(positions)
    span = (positions[-1] - positions[0]) / float(rate)
    if span <= 0:
        return None, len(positions)
    return (len(positions) - 1) / span, len(positions)


def main():
    ap = argparse.ArgumentParser(description="Verify the AC'97 WAV evidence")
    ap.add_argument("wav", help="WAV file recorded by QEMU (-audiodev wav)")
    ap.add_argument("--segment", type=int, default=0,
                    help="1-based tone segment to check (default: longest)")
    ap.add_argument("--expect-duration", type=float, default=EXPECT_DURATION_S,
                    help="expected tone duration in seconds (default %.2f; a run "
                         "that also typed the shell `beep` command records two "
                         "back-to-back tones, so pass 2.0 there)" % EXPECT_DURATION_S)
    ap.add_argument("--duration-tol", type=float, default=DURATION_TOL_S,
                    help="duration tolerance in seconds (default %.2f)" % DURATION_TOL_S)
    args = ap.parse_args()

    if not os.path.exists(args.wav):
        raise SystemExit("%s: no such file" % args.wav)

    wav = read_wav(args.wav)
    mono = channel0(wav.payload, wav.channels)
    total_s = len(wav.payload) / float(wav.channels) / float(wav.rate)

    print("[ac97-wav] file       : %s (%d bytes)" % (wav.path, os.path.getsize(wav.path)))
    print("[ac97-wav] format     : PCM %d-bit, %d ch, %d Hz, %.3f s of audio"
          % (wav.bits, wav.channels, wav.rate, total_s))

    gap = int(SEGMENT_GAP_S * wav.rate)
    min_len = int(MIN_SEGMENT_S * wav.rate)
    segments = find_segments(mono, ACTIVE_THRESHOLD, gap, min_len)
    if not segments:
        print("[ac97-wav] FAIL: no tone segment found in %s" % wav.path)
        print("           (is the file from a run with -device AC97,audiodev=a0, "
              "and did the boot self-test / `beep` actually play?)")
        return 1

    print("[ac97-wav] segments   : %d" % len(segments))
    for idx, (a, b) in enumerate(segments, 1):
        print("           #%d start=%.3f s duration=%.3f s"
              % (idx, a / float(wav.rate), (b - a) / float(wav.rate)))

    if args.segment:
        if args.segment > len(segments):
            print("[ac97-wav] FAIL: --segment %d out of range" % args.segment)
            return 1
        a, b = segments[args.segment - 1]
    else:
        a, b = max(segments, key=lambda s: s[1] - s[0])

    seg = mono[a:b]
    duration = len(seg) / float(wav.rate)
    peak = max(abs(v) for v in seg) / FULL_SCALE
    freq, crossings = zero_cross_freq(seg, wav.rate, hyst=64)

    print("[ac97-wav] checking   : segment start=%.3f s duration=%.3f s peak=%.4f FS "
          "crossings=%d freq=%s Hz"
          % (a / float(wav.rate), duration, peak, crossings,
             ("%.2f" % freq) if freq is not None else "n/a"))

    checks = []
    checks.append(("sample rate 48000 Hz", wav.rate == EXPECT_RATE,
                   "%d Hz" % wav.rate))
    checks.append(("duration %.3f s +/- %.3f s" % (args.expect_duration, args.duration_tol),
                   abs(duration - args.expect_duration) <= args.duration_tol,
                   "%.3f s" % duration))
    checks.append(("peak >= 0.10 FS", peak >= MIN_PEAK_FS, "%.4f FS" % peak))
    checks.append(("frequency 440 Hz +/- 22 Hz",
                   freq is not None and abs(freq - EXPECT_FREQ_HZ) <= FREQ_TOL_HZ,
                   ("%.2f Hz" % freq) if freq is not None else "unmeasurable"))

    failed = 0
    for name, ok, value in checks:
        print("[ac97-wav] %s %s (%s)" % ("PASS" if ok else "FAIL", name, value))
        if not ok:
            failed += 1

    if failed:
        print("[ac97-wav] RESULT: FAIL (%d check(s) failed)" % failed)
        if wav.rate != EXPECT_RATE:
            print("[ac97-wav] hint: QEMU's wav backend defaults to 44100 Hz; record "
                  "with -audiodev wav,id=a0,path=...,out.frequency=48000")
        return 1

    print("[ac97-wav] RESULT: PASS (rate=%d Hz, tone=%.3f s, peak=%.4f FS, %.2f Hz, "
          "%d crossings)" % (wav.rate, duration, peak, freq, crossings))
    return 0


if __name__ == "__main__":
    sys.exit(main())
