#!/usr/bin/env python3
"""SutureOS Sound Blaster 16 (ISA) + 8237 DMA channel 1 acceptance.

Proves the new ISA sound path end-to-end on QEMU 11.1 (`-device sb16`):

  Phase 1 - card present:
    * QEMU records the card's analog output with its wav backend
      (`-audiodev wav,...,out.frequency=48000`: the backend's default is
      44100 Hz, which would silently resample the evidence),
    * the shell command `sb16test 440 500` is typed through QMP sendkey (real
      keyboard path, no serial cheat), which makes the driver program the 8237
      (page 0x83 / address 0x02 / count 0x03 / mode 0x45) and issue the DSP
      0xC0 single-cycle 8-bit DMA command,
    * the serial log must show the DSP probe, the DMA programming and the
      completion lines, and the boot self-test tally must stay 16/16,
    * the recorded WAV must be 48000 Hz, carry >= 0.4 s of tone, a peak of at
      least 0.05 FS and a zero-crossing frequency of 440 Hz +/- 30 Hz.  The
      tone duration is allowed to be shorter than the requested 500 ms because
      the wav backend only records what was actually played (QEMU starts the
      ISA DMA a few milliseconds late and may stop at the last queued period).

  Phase 2 - regression without the card (same ISO, no `-device sb16`):
    * exactly one added line "[sb16] no DSP (not present)",
    * no "[sb16] dsp reset ok" / "[sb16] beep" lines,
    * AC'97 / HDA lines and the 16/16 self-test tally unchanged,
    * no PANIC.

Usage:
    py -3 scripts/sb16_acceptance.py            # both phases (~2 boots)
    py -3 scripts/sb16_acceptance.py --device   # phase 1 only
    py -3 scripts/sb16_acceptance.py --regression
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "scripts")
sys.path.insert(0, SCRIPTS)

import verify_hda_wav as hw  # read_wav / channel0 / find_segments / zero_cross_freq

ARG_FILE = os.path.join(ROOT, "build", "qemu_sb16.args")
SERIAL_DEV = os.path.join(ROOT, "build", "serial_sb16.log")
SERIAL_REG = os.path.join(ROOT, "build", "serial_sb16_nocard.log")
WAV_FILE = os.path.join(ROOT, "build", "sb16.wav")

DEVICE_ARGS = (
    "-cdrom build/sutureos.iso -boot d "
    "-device sb16,audiodev=a0 "
    "-audiodev wav,id=a0,path=build/sb16.wav,out.frequency=48000 "
    "-smp 2 -no-reboot"
)
REGRESSION_ARGS = (
    "-cdrom build/sutureos.iso -boot d "
    "-smp 2 -no-reboot"
)

EXPECT_RATE = 48000
MIN_TONE_S = 0.45
MAX_TONE_S = 0.60
MIN_PEAK_FS = 0.05
EXPECT_FREQ_HZ = 440.0
FREQ_TOL_HZ = 30.0


def keys_for(text):
    """QEMU sendkey spelling for one shell command (space -> spc, then Enter)."""
    out = []
    for ch in text:
        if ch == " ":
            out.append("spc")
        elif ch == "-":
            out.append("minus")
        elif ch == ".":
            out.append("dot")
        elif ch == "/":
            out.append("slash")
        else:
            out.append(ch)
    out.append("ret")
    return ";".join(out)


def run_qemu(args_lines, serial_log, keys, monitor_port, boot_wait=35, after=2):
    if os.path.exists(serial_log):
        os.remove(serial_log)
    rel_args = os.path.relpath(ARG_FILE, ROOT)
    with open(ARG_FILE, "w", encoding="ascii", newline="") as fh:
        fh.write(args_lines + "\n")
    subprocess.run(
        [
            "powershell", "-NoProfile", "-File",
            os.path.join(ROOT, "scripts", "qemu_boot.ps1"),
            "-SerialLog", os.path.relpath(serial_log, ROOT),
            "-ArgFile", rel_args,
            "-BootWaitSeconds", str(boot_wait),
            "-Keys", keys,
            "-AfterKeysSeconds", str(after),
            "-MonitorPort", str(monitor_port),
        ],
        cwd=ROOT,
        check=False,
    )
    if not os.path.exists(serial_log):
        return ""
    with open(serial_log, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def no_panic(log):
    return "PANIC" not in log.upper().replace("NO PANIC", "")


def analyse_wav(path):
    """Return (checks, summary) for the recorded card output."""
    checks = []
    if not os.path.exists(path):
        checks.append(("WAV recorded by the wav backend", False, "missing"))
        return checks, {}
    wav = hw.read_wav(path)
    mono = hw.channel0(wav.payload, wav.channels)
    total_s = len(wav.payload) / float(wav.channels) / float(wav.rate)

    checks.append(("sample rate %d Hz" % EXPECT_RATE, wav.rate == EXPECT_RATE,
                   "%d Hz" % wav.rate))
    checks.append(("16-bit PCM samples", wav.bits == 16, "%d bit" % wav.bits))

    gap = int(0.05 * wav.rate)
    min_len = int(0.10 * wav.rate)
    segments = hw.find_segments(mono, int(0.01 * 32768.0), gap, min_len)
    if not segments:
        checks.append(("tone segment recorded", False, "none (silent WAV)"))
        return checks, {"total_s": total_s, "segments": 0}

    a, b = max(segments, key=lambda s: s[1] - s[0])
    seg = mono[a:b]
    duration = len(seg) / float(wav.rate)
    peak = max(abs(v) for v in seg) / 32768.0
    freq, crossings = hw.zero_cross_freq(seg, wav.rate, hyst=64)

    checks.append(("tone duration %.2f..%.2f s (requested 0.50 s)"
                   % (MIN_TONE_S, MAX_TONE_S),
                   MIN_TONE_S <= duration <= MAX_TONE_S, "%.3f s" % duration))
    checks.append(("peak >= %.2f FS" % MIN_PEAK_FS,
                   peak >= MIN_PEAK_FS, "%.4f FS" % peak))
    checks.append(("%.0f Hz +/- %.0f Hz (zero crossings)"
                   % (EXPECT_FREQ_HZ, FREQ_TOL_HZ),
                   freq is not None
                   and abs(freq - EXPECT_FREQ_HZ) <= FREQ_TOL_HZ,
                   ("%.2f Hz (%d crossings)" % (freq, crossings))
                   if freq is not None else "unmeasurable"))

    return checks, {
        "rate": wav.rate,
        "channels": wav.channels,
        "bits": wav.bits,
        "total_s": total_s,
        "segments": len(segments),
        "tone_s": duration,
        "peak_fs": peak,
        "freq_hz": freq,
        "crossings": crossings,
    }


def phase_device():
    print("[sb16] phase 1: -device sb16,audiodev=a0 (WAV evidence)")
    if os.path.exists(WAV_FILE):
        os.remove(WAV_FILE)
    log = run_qemu(DEVICE_ARGS, SERIAL_DEV, keys_for("sb16test 440 500"),
                   monitor_port=45721, boot_wait=35, after=2)

    checks = []
    checks.append(("[sb16] dsp reset ok version=4.x",
                   re.search(r"\[sb16\] dsp reset ok version=4\.\d+", log)
                   is not None, ""))
    checks.append(("[  OK  ] SB16 boot line",
                   re.search(r"\[  OK  \] SB16: Sound Blaster 16 DSP", log)
                   is not None, ""))
    m = re.search(r"\[sb16\] dma buffer @(0x[0-9a-f]+) \((\d+) bytes, ([^,]+), align (\d+)\)",
                  log)
    checks.append(("DMA buffer reported, 64 KiB aligned, inside the 24-bit range",
                   m is not None
                   and int(m.group(4)) == 0
                   and "in 24-bit range" in m.group(3), m.group(0) if m else ""))
    md = re.search(r"\[sb16\] dma: base=(0x[0-9a-f]+) count=(\d+) \(page=(0x[0-9a-f]+)\) tone=(\d+)",
                   log)
    checks.append(("[sb16] dma: base=/count=/page=/tone= line",
                   md is not None, md.group(0) if md else ""))
    # 22050 Hz * 500 ms = 11025 tone samples; the driver appends a 400 ms
    # silence drain (8820 samples) so QEMU's audio backend flushes the tone
    # tail into the WAV, hence count = 11025 + 8820 - 1.
    checks.append(("DMA tone length == 22050 Hz * 500 ms",
                   md is not None and int(md.group(4)) == 11025,
                   md.group(4) if md else ""))
    checks.append(("DMA count == tone + 400 ms drain - 1",
                   md is not None and int(md.group(2)) == 11025 + 8820 - 1,
                   md.group(2) if md else ""))
    checks.append(("[sb16] beep 440Hz 500ms (22050 Hz, 8-bit mono)",
                   "[sb16] beep 440Hz 500ms (22050 Hz, 8-bit mono)" in log, ""))
    checks.append(("[sb16] beep done", "[sb16] beep done" in log, ""))
    checks.append(("boot self-tests still 16/16",
                   "[stitch] selftests: 16/16 passed" in log, ""))
    checks.append(("no kernel PANIC", no_panic(log), ""))

    wav_checks, summary = analyse_wav(WAV_FILE)
    checks.extend(wav_checks)

    print("")
    print("[sb16] serial log: %s" % os.path.relpath(SERIAL_DEV, ROOT))
    for line in log.splitlines():
        if "[sb16]" in line or "SB16" in line:
            print("       | " + line.strip())
    if summary:
        print("[sb16] wav       : %s" % os.path.relpath(WAV_FILE, ROOT))
        print("[sb16] wav info  : %d Hz, %d ch, %d bit, %.3f s audio, "
              "%d segment(s), tone=%.3f s, peak=%.4f FS, freq=%s Hz"
              % (summary.get("rate", 0), summary.get("channels", 0),
                 summary.get("bits", 0), summary.get("total_s", 0.0),
                 summary.get("segments", 0), summary.get("tone_s", 0.0),
                 summary.get("peak_fs", 0.0),
                 ("%.2f" % summary["freq_hz"])
                 if summary.get("freq_hz") is not None else "n/a"))
    return checks, log


def phase_regression():
    print("")
    print("[sb16] phase 2: same ISO without -device sb16 (regression)")
    log = run_qemu(REGRESSION_ARGS, SERIAL_REG, keys_for("sb16test 440 500"),
                   monitor_port=45722, boot_wait=30, after=1)

    checks = []
    no_dsp = log.count("[sb16] no DSP (not present)")
    checks.append(("exactly one '[sb16] no DSP (not present)' line",
                   no_dsp == 1, "%d line(s)" % no_dsp))
    checks.append(("no DSP probe success line",
                   "[sb16] dsp reset ok" not in log, ""))
    checks.append(("no beep attempted", "[sb16] beep done" not in log, ""))
    checks.append(("sb16test reports the missing card",
                   "sb16test: no Sound Blaster 16 DSP" in log, ""))
    checks.append(("AC'97 line unchanged",
                   "no codec (skipped)" in log or "[  OK  ] AC'97" in log, ""))
    checks.append(("HDA line unchanged",
                   "no controller" in log, ""))
    checks.append(("boot self-tests still 16/16",
                   "[stitch] selftests: 16/16 passed" in log, ""))
    checks.append(("no kernel PANIC", no_panic(log), ""))

    for line in log.splitlines():
        if "[sb16]" in line or "sb16test" in line or "AC'97" in line or "HDA" in line:
            print("       | " + line.strip())
    return checks, log


def main():
    ap = argparse.ArgumentParser(description="SutureOS SB16/8237 acceptance")
    ap.add_argument("--device", action="store_true", help="run phase 1 only")
    ap.add_argument("--regression", action="store_true", help="run phase 2 only")
    args = ap.parse_args()

    if not os.path.exists(os.path.join(ROOT, "build", "sutureos.iso")):
        raise SystemExit("build/sutureos.iso missing: run scripts/build.py + "
                         "scripts/build_iso.py first")

    run_both = not args.device and not args.regression
    checks = []
    if run_both or args.device:
        c, _ = phase_device()
        checks.extend(("device: " + n, ok, v) for n, ok, v in c)
    if run_both or args.regression:
        c, _ = phase_regression()
        checks.extend(("regression: " + n, ok, v) for n, ok, v in c)

    failed = [n for n, ok, _ in checks if not ok]
    print("")
    for name, ok, value in checks:
        extra = (" (%s)" % value) if value else ""
        print("  [%s] %s%s" % ("ok" if ok else "FAIL", name, extra))
    print("[sb16] checks=%d ok=%d fail=%d"
          % (len(checks), len(checks) - len(failed), len(failed)))
    print("[sb16] RESULT:", "PASS" if not failed else "FAIL")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
