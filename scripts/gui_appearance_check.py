#!/usr/bin/env python3
"""SutureOS appearance gate: the desktop must stay pixel-identical to the frozen
baseline except for the values that are *live data*.

Why: the project has a hard rule (see docs/PORTS.md) - porting another system's
features may never move the boot presentation or the GUI. Startup logs are
allowed to carry new driver lines, but the desktop must not change. This script
proves that by diffing a fresh QEMU screendump against docs/baseline/base_gui.png
and only tolerating differences inside the rectangles below, every one of which
contains live numbers (or the version string, which must change with a release).

Usage:
  py -3 scripts/gui_appearance_check.py <shot.ppm>
Exit code 0 only when nothing outside the dynamic rectangles changed.
"""
import sys

try:
    from PIL import Image
except ImportError:                     # keep the message actionable
    sys.exit('Pillow is required: py -3 -m pip install pillow')

BASE = 'docs/baseline/base_gui.png'

# Live-data rectangles, each documented with what it contains.
DYNAMIC = [
    ('Task Manager counters (PMM/heap/uptime/FPS/windows)', (140, 88, 212, 178)),
    ('Task Manager CPU-busy bar',                           (40, 202, 360, 224)),
    ('Calculator display + status line',                    (382, 50, 622, 184)),
    ('About window version string',                         (648, 118, 970, 148)),
]


def inside(x, y):
    return any(x0 <= x < x1 and y0 <= y < y1
               for (_, (x0, y0, x1, y1)) in DYNAMIC)


def main(shot):
    a = Image.open(BASE).convert('RGB')
    b = Image.open(shot).convert('RGB')
    if a.size != b.size:
        print('SIZE MISMATCH %s vs %s' % (a.size, b.size))
        return 1
    pa, pb = a.load(), b.load()
    w, h = a.size
    live = bad = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(h):
        for x in range(w):
            if pa[x, y] == pb[x, y]:
                continue
            if inside(x, y):
                live += 1
                continue
            bad += 1
            x0, y0 = min(x0, x), min(y0, y)
            x1, y1 = max(x1, x), max(y1, y)
    print('baseline %s -> %s' % (BASE, shot))
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
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
