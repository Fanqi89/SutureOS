# Real vertical retrace (vblank) on SutureOS

`kernel/gui/vsync.c` / `kernel/gui/vsync.h` add a **hardware** vblank source for the
present path, plus the `vsynctest` shell command and `scripts/vsync_acceptance.py`.
The short version: the module implements and measures a real VGA vertical-retrace
edge wait correctly, but **QEMU's emulated VGA does not provide such a signal** —
port `0x3DA` bit 3 is a per-read toggle there, so on QEMU the module honestly
reports "no source" and `vsync_present()` returns `-1` (the degraded path).
The measured evidence is in section 6; the QEMU mechanism is in section 7.

## 1. What this is

SutureOS runs on a VBE linear framebuffer (`limine_fb_addr`, QEMU `-vga std`) with
no GPU driver and no display interrupt, so both the Rust desktop shell and the
compositor pace presents on a software 60 Hz grid built from the HPET
(`kernel/gui/slint_bridge.c: suture_gui_time_ns()`). The only hardware
frame-boundary signal an x86 PC exposes without a driver model is the
VGA-compatible Input Status Register 1:

| port | bit | meaning |
|------|-----|---------|
| `0x3DA` | 3 (`0x08`) | 1 while the beam is inside vertical retrace |
| `0x3DA` | 0 (`0x01`) | 1 while display output is disabled (blanking) |

`vsync_present()` waits for the **rising edge** of bit 3 (`0 -> 1`), i.e. the
start of vertical retrace, which is the natural place to flip a framebuffer.

Reading `0x3DA` has a documented side effect: it resets the VGA attribute
controller flip-flop (`vga.c:410` in QEMU, and the same on real VGA). SutureOS
never programs the attribute controller after Limine hands over the framebuffer,
so the side effect is inert here, but it is listed as a limitation in section 11.

## 2. API

The signatures are fixed because `slint_bridge.c` is written against them.

| function | meaning |
|---|---|
| `int vsync_init(void)` | lazy probe; `0` = a usable vblank source exists, `-1` = none |
| `int vsync_available(void)` | `1`/`0`; lazily initialises |
| `int vsync_present(void)` | wait for the next rising edge: `1` = got it, `0` = bounded timeout (~40 ms), `-1` = no source |
| `int vsync_hz100(void)` | measured refresh rate x100 (`6000` = 60.00 Hz); `0` when unknown **or rejected** |
| `void vsync_stats(uint32_t *edges, uint32_t *timeouts, uint32_t *max_wait_us)` | cumulative statistics, any pointer may be NULL |
| `int vsync_selftest(void)` | prints `[vsync] selftest: hz=.. edges=.. timeouts=.. max_wait_us=.. PASS\|FAIL`, `0` = PASS |

Diagnostics used by `vsynctest` (not part of the present path): `vsync_nominal_hz100()`,
`vsync_crtc_info()`, `vsync_observed_hz100()`, `vsync_period_ns()`,
`vsync_min_period_ns()`, `vsync_max_period_ns()`, `vsync_avg_wait_us()`,
`vsync_max_wait_us()`, `vsync_edges_in_window(ms, &samples, &high)`,
`vsync_latch_edges()`, `vsync_status_text()`, `vsync_format_hz()`,
`vsync_math_selftest()`.

`vsync_hz100()` deliberately returns `0` for a source that failed validation, so a
caller can never accelerate its pacing from a bogus reading; the unvalidated
number stays visible through `vsync_observed_hz100()`.

## 3. How init measures

1. Read the CRTC registers and derive the **nominal** refresh rate (cross-check,
   section 4).
2. Latch up to **60 rising edges** of `0x3DA` bit 3, timestamping every edge with
   `time_source_now_ns()` (HPET, 100 MHz — the probe in `time_source.c` measures
   a 12 ns error over 10 ms).
3. `period_ns = (last - first) / (edges - 1)`; `min_ns`/`max_ns` are the shortest
   and longest single spacing (jitter).
4. `hz100 = round(1e11 / period_ns)`.
5. Availability policy: 60 edges must arrive **within 2 s**, and the measured rate
   must be a display rate in **40..120 Hz**. Anything else is rejected with a
   reason string; `vsync_selftest()` further requires **50..75 Hz** and a timeout
   ratio <= 20 %.

## 4. CRTC cross-check

The nominal rate mirrors QEMU's `vga_precise_update_retrace_info()`
(`hw/display/vga.c:194-248`), i.e. the standard VGA formula:

```
hz = (dot_clock / dots_per_char) / ((h_total + 5) * (v_total + 2))
```

with `dot_clock` from the misc output register bits 2..3, `dots` = 8 or 9 from
misc bit 0, the `v_total` bit 8/9 split across CRTC `0x07`, and the clocking-mode
doubling bit (`0x08` of sequencer register 1). The sequencer is unlocked exactly
as the hardware docs describe (`outb(0x3C4,0x01); outb(0x3C5, inb(0x3C5) & 0xDF)`);
the sequencer and CRTC index registers are saved and written back afterwards (only
when they look like valid indices). If the values are unusable the nominal rate is
reported as `0.00` instead of a guess.

Measured capture from Limine's framebuffer mode (QEMU 11.1, `-vga std`):

```
[vsync] crtc: htot=5f hend=7f vtot=bf ovf=5d vsync=9c vend=ff misc=67 clk=0
[vsync] init: ... nominal_hz=78.84 ...
```

`nominal_hz=78.84` is what those CRTC totals say (28.322 MHz / 8 / (100 * 449)),
and it is *not* the real 1024x768 refresh rate: QEMU keeps VGA-style CRTC totals
when it programs a VBE mode, so the cross-check is a sanity check, not a
measurement. This is exactly why the API reports the *measured* edge rate and only
prints the nominal one.

## 5. `vsynctest`

```
vsynctest
```

prints, in order:

```
[vsync] init: source=vretrace|none nominal_hz=.. measured_hz=.. period_ns=.. min_ns=.. max_ns=..
[vsync] crtc: htot=.. hend=.. vtot=.. ovf=.. vsync=.. vend=.. misc=.. clk=..
[vsync] math: period->hz conversions, formatter and rate policy PASS|FAIL
[vsync] present x30: edges=.. timeouts=.. avg_wait_us=.. max_wait_us=..
[vsync] edges_in_1s=.. samples=.. high=.. latch_edges=..
[vsync] selftest: hz=.. edges=.. timeouts=.. max_wait_us=.. PASS|FAIL
```

`edges_in_1s`/`samples`/`high` come from one second of raw port sampling without
timestamps: this is the evidence that the bit really toggles (and how fast), or
that it is stuck. `math` checks the hardware-independent half of the logic
(period -> Hz arithmetic, the hand-written fixed-point formatter, and the
accept/reject policy) against known values, so the code path a real machine would
take is proven even when the port is unusable — it caught a real truncation bug
during development (`16666667 ns` reported `59.99` instead of `60.00`; the
computation now rounds to the nearest 0.01 Hz).

## 6. Measured results (QEMU 11.1, the honest part)

Same ISO, same `vsynctest`, two display backends:

| run | display | latch edges | observed_hz | period_ns | min/max ns | samples in 1 s | edges in 1 s | high |
|-----|---------|------------:|------------:|----------:|-----------:|---------------:|-------------:|-----:|
| A | `-display none` | 60 | 265 674.81 | 3 764 | 2 700 / 33 500 | 1 857 024 | 928 512 | 928 512 |
| B | `-display gtk`  | 60 | 178 253.12 | 5 610 | 2 600 / 105 700 | 1 659 904 | 829 952 | 829 952 |

Other `-display none` runs of the same module measured 280 426.25, 274 047.68,
277 546.48, 278 629.14, 208 116.55, 168 350.16 and 111 259.45 - with
`high == samples/2` in every single run.

What these numbers mean:

* **`high == samples/2` exactly, every run** (953 088 of 1 906 176; 829 952 of
  1 659 904 ...). Exactly half of the reads see the bit set, and `edges_in_1s` is
  also half the sample count: the bit **flips on every single read** of `0x3DA`.
  A real vblank would give ~60 edges/s and `high` around 1-5 % of the samples.
* **`measured_hz` tracks the polling speed, not any display timing**: run A
  samples at 1.9 M/s and "measures" 280 kHz; run B samples at 1.66 M/s and
  "measures" 178 kHz; the latch's 60-edge burst (which also pays for an HPET read
  per edge) measures 111-280 kHz depending on loop timing. A 60 Hz source would
  report ~60 Hz at any sampling speed.
* **A real display backend does not change it** (run B), so this is not an
  artifact of `-display none`.
* `nominal_hz=78.84` (CRTC) contradicts the measured number as well, which is the
  cross-check doing its job: it flags that the measured rate cannot be the panel
  refresh.

Conclusion: on this QEMU there is **no frame-rate vblank signal** on port `0x3DA`
bit 3. The module therefore reports `source=none`, `vsync_hz100() == 0`,
`vsync_present() == -1` (30/30 calls) and `vsync_selftest() == FAIL`, and
`vsynctest` prints the measured numbers plus the rejection reason instead of a
polite zero.

## 7. Why: QEMU's VGA emulation (code evidence)

From `hw/display/vga.c` of QEMU v11.1.0:

```c
/* line 406 */ case VGA_IS1_RM:
/* line 407 */ case VGA_IS1_RC:
/* line 408 */     /* just toggle to fool polling */
/* line 409 */     val = s->st01 = s->retrace(s);
/* line 410 */     s->ar_flip_flop = 0;
```

and the retrace implementations:

```c
/* line 315 */ static uint8_t vga_dumb_retrace(VGACommonState *s)
/* line 316 */ {
/* line 317 */     return s->st01 ^ (ST01_V_RETRACE | ST01_DISP_ENABLE);
/* line 318 */ }

/* line 286 */ static uint8_t vga_precise_retrace(VGACommonState *s)   /* clock based */
```

`s->retrace` is chosen from `vga_retrace_method` (`vga.c:2269-2277`):
`VGA_RETRACE_DUMB` installs the per-read toggle, `VGA_RETRACE_PRECISE` installs
the virtual-clock implementation that would model a real vertical retrace. This
QEMU build always takes the dumb path for `-vga std` — `qemu-system-x86_64 -help`
exposes no retrace option, so there is no supported way to ask for the precise
model. That is a property of the emulator, **not** of real VGA hardware, where
`0x3DA` bit 3 is the documented live vertical-retrace flag.

## 8. What the module does with a useless port

* `vsync_init()` collects the 60 edges (they exist — the bit does move), computes
  the rate, sees 111-280 kHz instead of 40-120 Hz, and rejects the source:
  `source=none`, reason "edge rate is not a display refresh rate".
* `vsync_present()` returns `-1` immediately (never spins for 40 ms on a source it
  knows is unusable), so a caller that ignores `vsync_available()` still gets a
  fast, safe failure.
* `vsync_hz100()` returns `0`; the measured number stays visible through
  `vsync_observed_hz100()` and the log line.
* `vsync_math_selftest()` still proves the arithmetic, formatter and policy
  (`[vsync] math: ... PASS`).
* Boot is untouched: nothing calls `vsync_*` unless the user (or the Lead's
  wiring) asks, so the default presentation is byte-identical.

## 9. Acceptance

```
py -3 scripts\mkfat32_testimg.py
Copy-Item build\fat32.img build\fat32_ata.img -Force
py -3 scripts\build.py
py -3 scripts\build_iso.py
py -3 scripts\vsync_acceptance.py             # strict: expects a real 60 Hz vblank
py -3 scripts\vsync_acceptance.py --degraded  # expects the honest no-source contract
```

Both modes boot the ISO with `-display none`, QMP port 45831, 50 s boot wait,
keys typed one at a time at 75 ms, then `vsynctest` + Enter, and print
`checks=N ok=N fail=N` plus `RESULT: PASS|FAIL`.

* **strict** (15 checks): `source=vretrace`, measured 55..65 Hz, 15..19 ms period
  with <= 20 % jitter, 30 present() waits with edges and <= 6 timeouts,
  `edges_in_1s` 50..70, bit neither stuck low nor high, selftest PASS, no PANIC.
* **degraded** (11 checks): `source=none`, the measured numbers still reported,
  the rejected rate is not a display rate, 30/30 `present()` calls return `-1`,
  `vsync_hz100()` reads 0.00 through the selftest line, selftest honestly FAILs,
  the bit really toggles but far faster than any display, no PANIC.

Results on this machine (QEMU 11.1, `-display none`):

* strict: `checks=15 ok=8 fail=7`, `RESULT: FAIL` — the 7 failures are exactly the
  60 Hz assertions (`source`, `measured_hz`, `period`, `jitter`, `present edges`,
  `edges_in_1s`, `selftest`), all reported with their measured values.
* degraded: `checks=11 ok=11 fail=0`, `RESULT: PASS`.
* kernel self-tests in both runs: `[stitch] selftests: 16/16 passed`, no PANIC.

Typical serial log (this is the whole truth for this machine; verified on
`build/sutureos.iso` 23 783 424 B, sha256 `0282f76df19d…821fa7`,
`kernel.elf` 6 586 520 B):

```
[stitch] selftests: 16/16 passed
[vsync] probe: latch_edges=60 observed_hz=265674.81 nominal_hz=78.84 period_ns=3764 min_ns=2700 max_ns=33500 -> unavailable
[vsync] init: source=none nominal_hz=78.84 measured_hz=265674.81 period_ns=3764 min_ns=2700 max_ns=33500 (rejected: edge rate is not a display refresh rate)
[vsync] crtc: htot=5f hend=7f vtot=bf ovf=5d vsync=9c vend=ff misc=67 clk=0
[vsync] math: period->hz conversions, formatter and rate policy PASS
[vsync] present x30: edges=0 timeouts=0 avg_wait_us=0 max_wait_us=0
[vsync] present x30: 30 call(s) returned -1 (edge rate is not a display refresh rate)
[vsync] edges_in_1s=928512 samples=1857024 high=928512 latch_edges=60
[vsync] selftest: hz=0.00 edges=0 timeouts=0 max_wait_us=0 FAIL
```

## 10. Wiring into the present path (for the Lead)

`slint_bridge.c`, `shell.rs` and the compositor are **not** modified by this
change; the module is dormant until something calls it. The intended wiring:

```c
#include "vsync.h"

/* once per desktop session, not per frame */
static int g_vsync_ready = -1;
if (g_vsync_ready < 0) g_vsync_ready = (vsync_init() == 0);   /* 0 = usable */

/* once per presented frame, right before the flip */
if (g_vsync_ready) {
    const int r = vsync_present();      /* 1 edge, 0 timeout, -1 no source */
    if (r == 0) { /* keep the existing HPET grid for this frame */ }
}
```

Rules that matter:

1. **Never pace on `vsync_hz100() == 0`.** It returns 0 both for "unknown" and for
   "rejected"; keep the existing 16.67 ms HPET grid in that case. On today's QEMU
   that is the live path: `vsync_init()` returns `-1`, `present()` returns `-1`.
2. `present()` is a **bounded spin** (~40 ms worst case, usually one frame). Call
   it once per presented frame, not per damage rect, and not inside the rasteriser.
3. The call is cheap to fail: when `vsync_available() == 0` it returns immediately
   (`-1`), so no extra guard is strictly needed beyond the `g_vsync_ready` flag.
4. Do not treat `present()` as a frame-accurate trigger: its resolution is one
   port I/O round trip (0.7-9 us in QEMU TCG) plus HPET read latency; it is a
   *pacing hint* that removes tearing, not a timestamped event.
5. If a future machine reports `source=vretrace`, prefer its measured rate
   (`vsync_hz100()`) over the hard-coded 60 Hz grid, and fall back to the grid
   after two consecutive timeouts.

## 11. Known limitations

1. **Register polling, not a scanline IRQ.** There is no display interrupt in
   SutureOS, and the VGA vertical-retrace IRQ ("IRQ9 clamp") is not emulated by
   QEMU and not wired here. Every wait is an `inb` + `pause` spin, so
   `vsync_present()` burns CPU instead of sleeping.
2. **Port read side effect.** Reading `0x3DA` resets the attribute controller
   flip-flop. Nothing in SutureOS programs `0x3C0`/`0x3C1` after Limine, so this
   is inert today, but a future driver that touches the attribute controller must
   not interleave with vsync sampling.
3. **QEMU gives no vblank.** As measured in section 6, `-vga std` under QEMU 11.1
   toggles bit 3 per read (`vga_dumb_retrace`, `vga.c:315`). The module therefore
   rejects the source; the strict acceptance fails on this machine by design, and
   the degraded acceptance proves the `-1` contract instead.
4. **Real hardware / other hypervisors are unverified.** Real VGA documents bit 3
   as vertical retrace, and the implementation follows that, but **nothing here was
   measured on real hardware, VMware or VirtualBox** — do not extrapolate these
   numbers. On real hardware, expect `vsync_init()` to succeed with the panel's
   rate (typically 60-75 Hz); verify with `vsynctest` before trusting it.
5. **Precision.** Edge spacing is timestamped with the HPET through
   `time_source_now_ns()`, which costs a port/MMIO read; in QEMU a sampling
   iteration takes ~0.5-9 us, so min/max jitter includes the measurement overhead,
   not just display jitter. `present()` resolution is likewise one I/O round trip.
6. **Single source, single display.** The module assumes the VGA-compatible ports
   are decoded (`misc` bit 0 = 1, i.e. `0x3D4`/`0x3DA` rather than `0x3B4`/`0x3BA`)
   and knows nothing about multi-monitor, DPMS or mode changes. A KMS driver would
   replace it with a real vblank event.
7. **No swap-chain integration yet.** The compositor's 60 Hz HPET grid remains the
   pacing authority until the wiring in section 10 lands; `vsync_present()` alone
   does not tear-proof anything.
8. **The measured rate of a bogus source depends on the poll speed** (111-280 kHz
   above). That is why `vsync_init()` validates the rate instead of trusting it —
   without that check, `present()` would "sync" to the port read itself.
9. **Only the registers we touch are restored.** The sequencer and CRTC *index*
   registers are written back (when the read looks like a valid index); the CRTC
   timing registers are only read. A mode set racing with `vsynctest` is not
   handled.
10. **Two branches are implemented but unexercised here.** The "bit stuck at 0 or
    1" path (fewer than 60 edges inside the 2 s budget -> unavailable) and the
    `vsync_present()` timeout path (a source that was usable but then stops
    toggling -> `0`) could not be triggered on this machine: the port always
    toggles, so every run takes either the "usable" or the "rejected rate" branch.
    Both paths are bounded by the same wall clock + pause-spin caps that the
    rejected-rate path already exercises (30/30 `-1` calls return immediately).

## 12. Files

* `kernel/gui/vsync.c`, `kernel/gui/vsync.h` — new module (measurement, policy,
  bounded present, statistics, `vsync_math_selftest`).
* `kernel/shell/shell.c`, `kernel/shell/shell.h` — the `vsynctest` command only
  (include, prototype, one command-table row, one function after
  `shell_cmd_timesrc`).
* `scripts/vsync_acceptance.py` — strict + `--degraded` gates.
* `docs/vsync.md` — this document.

Nothing else is touched: `kernel/gui/slint_bridge.c`, `kernel/main/*`, `rust/**`
and `scripts/build.py` are unchanged. `scripts/build.py` globs `kernel/**/*.c`, so
`vsync.c` is compiled automatically (verified: `build/obj/kernel__gui__vsync.c.o`).
