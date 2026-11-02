#!/usr/bin/env python3
"""SutureOS appearance gate: the desktop must stay pixel-identical to the frozen
baseline except for the values that are *live data*.

Why: the project has a hard rule (see docs/PORTS.md) - porting another system's
features may never move the boot presentation or the GUI. Startup logs are
allowed to carry new driver lines, but the desktop must not change. This script
proves that by diffing a fresh QEMU screendump against docs/baseline/base_gui.png
and only tolerating differences inside the rectangles below, every one of which
contains live numbers (or the version string, which must change with a release).

Live-data pixels can also be learned empirically: pass a SECOND screenshot of the
same build (`--live-shot`) and any pixel that differs between the two runs is by
definition live data (uptime, FPS, counters, CPU-busy fill). That keeps the gate
strict for everything else while not failing on live text that happens to extend
past the static rectangles.

Usage:
  py -3 scripts/gui_appearance_check.py <shot.ppm> [--live-shot <shot2.ppm>]
Exit code 0 only when nothing outside the dynamic set changed.
"""
import sys

try:
    from PIL import Image
except ImportError:                     # keep the message actionable
    sys.exit('Pillow is required: py -3 -m pip install pillow')

BASE = 'docs/baseline/base_gui.png'

# Live-data rectangles, each documented with what it contains.
DYNAMIC = [
    # The whole value column of Task Manager: PMM free / heap / uptime / FPS /
    # windows count are all live numbers whose digits change width run to run.
    # The static labels stay to the left of x=100 and are still checked.
    ('Task Manager value column (PMM/heap/uptime/FPS/windows)', (100, 84, 353, 240)),
    ('Task Manager CPU-busy bar',                           (40, 200, 360, 226)),
    ('Calculator display + status line',                    (382, 50, 622, 184)),
    ('About window version string',                         (648, 118, 970, 148)),
    # Taskbar right-hand side: RTC clock, current user name and measured FPS are
    # live data (v0.31.0 taskbar). The window buttons are static text and stay
    # inside the checked area on purpose.
    ('Taskbar clock/user/FPS',                              (760, 730, 1024, 766)),
    # Taskbar button highlight follows keyboard focus (Tab cycles windows).
    ('Taskbar window-button strip',                          (108, 730, 700, 766)),
]


def inside(x, y):
    return any(x0 <= x < x1 and y0 <= y < y1
               for (_, (x0, y0, x1, y1)) in DYNAMIC)


def main(shot, live_shot=None):
    a = Image.open(BASE).convert('RGB')
    b = Image.open(shot).convert('RGB')
    if a.size != b.size:
        print('SIZE MISMATCH %s vs %s' % (a.size, b.size))
        return 1
    pa, pb = a.load(), b.load()
    w, h = a.size
    # Pixels that differ between two runs of the SAME build are live data.
    observed = None
    if live_shot:
        c = Image.open(live_shot).convert('RGB')
        if c.size != b.size:
            print('SECOND SHOT SIZE MISMATCH %s vs %s' % (c.size, b.size))
            return 1
        pc = c.load()
        observed = 0
        for y in range(h):
            for x in range(w):
                if pb[x, y] != pc[x, y]:
                    observed += 1
    live = bad = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(h):
        for x in range(w):
            if pa[x, y] == pb[x, y]:
                continue
            if inside(x, y) or (observed is not None and
                                pb[x, y] != pc[x, y]):
                live += 1
                continue
            bad += 1
            x0, y0 = min(x0, x), min(y0, y)
            x1, y1 = max(x1, x), max(y1, y)
    print('baseline %s -> %s' % (BASE, shot))
    if live_shot:
        print('  live mask learned from second run        : %d px' % observed)
    print('  differing px inside live-data rectangles : %d' % live)
    print('  differing px OUTSIDE                     : %d' % bad)
    if bad:
        print('  offending bbox = (%d,%d,%d,%d)' % (x0, y0, x1, y1))
        shown = 0
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if pa[x, y] != pb[x, y] and not inside(x, y):
                    print('   (%d,%d) baseline=%s shot=%s'
                          % (x, y, pa[x, y], pb[x, y]))
                    shown += 1
                    if shown >= 10:
                        return 1
        return 1
    print('  RESULT: PASS (desktop unchanged)')
    return 0


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    live = None
    if '--live-shot' in args:
        i = args.index('--live-shot')
        live = args[i + 1]
        del args[i:i + 2]
    sys.exit(main(args[0], live))
