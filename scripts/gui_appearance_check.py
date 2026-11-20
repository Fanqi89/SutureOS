#!/usr/bin/env python3
"""SutureOS appearance gate: a desktop must stay pixel-identical to its frozen
baseline except for values that are *live data*.

Two profiles exist since v0.59.0, when `startgui` became the Rust desktop shell:

  --profile new      the Rust desktop shell (rounded windows, acrylic, taskbar,
                     start menu) -> docs/baseline/base_gui.png
  --profile classic  the original C desktop -> docs/baseline/base_gui_classic.png

Live-data pixels can also be learned empirically: pass a SECOND screenshot of the
same build (`--live-shot`) and any pixel that differs between the two runs is by
definition live data (clock, counters, focus). That keeps the gate strict for
everything else while not failing on live text.

Usage:
  py -3 scripts/gui_appearance_check.py <shot.ppm> [--live-shot <shot2.ppm>]
                                       [--profile new|classic]
Exit code 0 only when nothing outside the dynamic set changed.
"""
import sys

try:
    from PIL import Image
except ImportError:                     # keep the message actionable
    sys.exit('Pillow is required: py -3 -m pip install pillow')

CLASSIC_DYNAMIC = [
    # The whole value column of Task Manager: PMM free / heap / uptime / FPS /
    # windows count are live numbers whose digits change width run to run.
    ('Task Manager value column (PMM/heap/uptime/FPS/windows)', (100, 84, 353, 240)),
    # The CPU-busy bar is live data AND its left edge sits at x=36.
    ('Task Manager CPU-busy bar',                           (30, 200, 360, 226)),
    ('Calculator display + status line',                    (382, 50, 622, 184)),
    ('About window version string',                         (648, 118, 970, 148)),
    ('Taskbar clock/user/FPS',                              (760, 730, 1024, 766)),
    ('Taskbar window-button strip',                          (108, 730, 700, 766)),
]

# The Rust desktop shell (v0.59.0 frozen baseline). Only the RTC clock is live
# when the desktop is untouched: window bodies sample the (static) wallpaper,
# the input hint is static, the start menu is closed.
NEW_DYNAMIC = [
    ('Taskbar RTC clock (local time, minute resolution)',    (860, 738, 1016, 762)),
    # The task manager window prints live values (uptime seconds + a CPU figure
    # derived from it, apps.rs draw_live). Their digit shapes change every
    # second, so a two-shot live mask is NOT a superset of the baseline drift;
    # the value rows are declared live here.
    ('Task manager live numbers (uptime / CPU%)',            (520, 452, 830, 522)),
]

PROFILES = {
    'new': ('docs/baseline/base_gui.png', NEW_DYNAMIC),
    'classic': ('docs/baseline/base_gui_classic.png', CLASSIC_DYNAMIC),
}


def inside(x, y, dynamic):
    return any(x0 <= x < x1 and y0 <= y < y1
               for (_, (x0, y0, x1, y1)) in dynamic)


def main(shot, live_shot=None, profile='new'):
    base, dynamic = PROFILES[profile]
    a = Image.open(base).convert('RGB')
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
            if inside(x, y, dynamic) or (observed is not None and
                                         pb[x, y] != pc[x, y]):
                live += 1
                continue
            bad += 1
            x0, y0 = min(x0, x), min(y0, y)
            x1, y1 = max(x1, x), max(y1, y)
    print('profile %s: baseline %s -> %s' % (profile, base, shot))
    if live_shot:
        print('  live mask learned from second run        : %d px' % observed)
    print('  differing px inside live-data rectangles : %d' % live)
    print('  differing px OUTSIDE                     : %d' % bad)
    if bad:
        print('  offending bbox = (%d,%d,%d,%d)' % (x0, y0, x1, y1))
        shown = 0
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if pa[x, y] != pb[x, y] and not inside(x, y, dynamic):
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
    profile = 'new'
    if '--live-shot' in args:
        i = args.index('--live-shot')
        live = args[i + 1]
        del args[i:i + 2]
    if '--profile' in args:
        i = args.index('--profile')
        profile = args[i + 1]
        del args[i:i + 2]
    if profile not in PROFILES:
        sys.exit('unknown profile %r (use new|classic)' % profile)
    sys.exit(main(args[0], live, profile))
