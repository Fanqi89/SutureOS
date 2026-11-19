#!/usr/bin/env python3
"""SutureOS Slint GUI prototype acceptance (Stage 0, real QEMU framebuffer).

What is proven here, and why each check exists:

  * the Rust staticlib (Slint 1.18 + software renderer) is really LINKED into
    kernel.elf      -> serial line "[gui-slint] frame rendered 1024x768 checksum=..."
  * Slint's software renderer really painted the frame
                  -> the screendump is not the terminal: a panel with rounded
                     corners exists, a translucent panel composites two
                     different backdrops differently, the emulated shadow falls
                     off with distance, the pseudo-3D icon has a bright band at
                     the top and a dark band at the bottom.
  * nothing crashed -> no "PANIC" in the log and QMP still answers after the
                       frame was drawn.

The frame is a STATIC prototype: `guislint` paints over the screen and holds it
for up to 10 s (ESC returns), which is the window this script dumps.

Usage:
  py -3 scripts/slint_gui_acceptance.py            # boot + dump + assert
  py -3 scripts/slint_gui_acceptance.py --no-assert # only produce the dump
Exit code 0 only when every check passes.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = int(os.environ.get('SLINT_QMP_PORT', '47971'))
SERIAL = 'build/serial_slint_gui.log'
SHOT = 'build/slint_gui.ppm'

SCREEN_W, SCREEN_H = 1024, 768

# ---- geometry of rust/suture-gui/ui/main.slint (keep in sync if the scene moves)
PANEL = (150, 170, 560, 320)          # x, y, w, h  (border-radius 18)
PANEL_R = 18
# fake shadow: 4 layers, dy = 8 px, outermost +10 px below the panel
SHADOW_X = 400
SHADOW_YS = (PANEL[1] + PANEL[3] + 3, PANEL[1] + PANEL[3] + 12, PANEL[1] + PANEL[3] + 24)
ICON = (178, 198, 72, 72)             # main panel icon tile (pseudo-3D)
BLOB_SAMPLE = (300, 380)              # inside the blue/purple blob, under the panel
PLAIN_SAMPLE = (680, 460)             # panel over wallpaper only (no blob)


class Qmp:
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:            # noqa: PERF203 - retry loop by design
                last = e
                time.sleep(0.2)
        else:
            raise RuntimeError('QMP never listened: %r' % (last,))
        self.f = self.s.makefile('rwb')
        self._read()
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            obj = json.loads(line.decode('utf-8'))
            if 'event' in obj:
                continue
            return obj

    def cmd(self, name, **args):
        msg = {'execute': name}
        if args:
            msg['arguments'] = args
        self.f.write((json.dumps(msg) + '\n').encode('utf-8'))
        self.f.flush()
        return self._read()

    def hmp(self, line):
        r = self.cmd('human-monitor-command', **{'command-line': line})
        return (r.get('return') or '').strip()


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def main():
    do_assert = '--no-assert' not in sys.argv
    log_abs = os.path.join(ROOT, SERIAL)
    dump_abs = os.path.join(ROOT, SHOT)
    for p in (log_abs, dump_abs):
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[slint-gui] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[slint-gui] ' + ' '.join(argv), flush=True)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    proc = subprocess.Popen(argv, cwd=ROOT)
    rendered = False
    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.05):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(ch if ch != ' ' else 'spc')

        # 1) wait for the shell prompt (the kernel is ~7 MB now, give it time)
        deadline = time.time() + 120
        while time.time() < deadline:
            if 'sutureos#' in log_text():
                break
            time.sleep(0.5)
        else:
            print('[slint-gui] shell prompt never appeared')
        time.sleep(0.5)

        # 2) run the prototype
        typ('guislint')
        key('ret')

        # 3) wait until the frame is on screen (the log line is printed after the
        #    renderer returned, so the pixels are already in the framebuffer)
        deadline = time.time() + 40
        while time.time() < deadline:
            if '[gui-slint] frame rendered' in log_text():
                rendered = True
                break
            time.sleep(0.3)
        time.sleep(1.0)

        # 4) dump while `guislint` is still holding the frame
        q.hmp('screendump ' + SHOT)
        time.sleep(0.6)

        # 5) ESC returns to the terminal (also proves the command is not stuck)
        key('esc')
        time.sleep(1.0)
    finally:
        try:
            q.hmp('quit')
        except Exception:               # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = log_text()
    print('[slint-gui] log lines of interest:')
    for line in log.splitlines():
        if '[gui-slint]' in line or '[rust]' in line:
            print('   ' + line)

    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    check('log: frame rendered line present', rendered)
    check('log: no kernel PANIC', 'PANIC' not in log)
    check('log: guislint returned to the terminal',
          'guislint: frame shown, back to terminal' in log)

    if not do_assert:
        return 0
    if not rendered:
        print('[slint-gui] RESULT: FAIL (no frame, pixel checks skipped)')
        return 1

    try:
        from PIL import Image
    except ImportError:
        print('[slint-gui] Pillow missing: cannot check pixels')
        return 1
    if not os.path.exists(dump_abs):
        print('[slint-gui] no screendump at %s' % dump_abs)
        return 1
    im = Image.open(dump_abs).convert('RGB')
    px = im.load()
    check('screendump is %dx%d' % (SCREEN_W, SCREEN_H), im.size == (SCREEN_W, SCREEN_H),
          str(im.size))

    # ---- (1) rounded corners: each corner pixel differs from the panel fill
    # 10 px inside that same corner. Comparing against an inward point instead of
    # a global reference keeps the check independent of what is behind the panel
    # (two different colour backdrops sit behind the main panel).
    corners = {
        'top-left':     (PANEL[0] + 2, PANEL[1] + 2, +10, +10),
        'top-right':    (PANEL[0] + PANEL[2] - 3, PANEL[1] + 2, -10, +10),
        'bottom-left':  (PANEL[0] + 2, PANEL[1] + PANEL[3] - 3, +10, -10),
        'bottom-right': (PANEL[0] + PANEL[2] - 3, PANEL[1] + PANEL[3] - 3, -10, -10),
    }
    # panel fill over the top edge: the reference for the later
    # 'icon highlight is brighter than the panel' check
    edge_ref = px[PANEL[0] + PANEL[2] // 2, PANEL[1] + 3]
    for name, (x, y, ix, iy) in corners.items():
        c = px[x, y]
        inside = px[x + ix, y + iy]
        d = sum(abs(a - b) for a, b in zip(c, inside))
        check('rounded corner %s differs from the panel fill' % name, d > 16,
              'corner %s vs inside %s, |d|=%d' % (c, inside, d))

    # ---- (2) translucency: the same panel over two backdrops differs ------------
    a = px[BLOB_SAMPLE]
    b = px[PLAIN_SAMPLE]
    delta = sum(abs(x - y) for x, y in zip(a, b))
    check('translucent panel composites two backdrops differently',
          delta > 30, 'blob %s vs plain %s, |d|=%d' % (a, b, delta))

    # ---- (3) emulated shadow falls off with distance ---------------------------
    lums = [luminance(px[SHADOW_X, y]) for y in SHADOW_YS]
    check('fake shadow gradient (closer to panel = darker)',
          lums[0] < lums[1] < lums[2],
          'luminance %s at y=%s' % (['%.1f' % v for v in lums], list(SHADOW_YS)))

    # ---- (4) pseudo-3D icon: bright specular band above, dark shade below ------
    def row_mean(y):
        xs = range(ICON[0] + 4, ICON[0] + ICON[2] - 4)
        return sum(luminance(px[x, y]) for x in xs) / len(list(xs))

    rows = [row_mean(y) for y in range(ICON[1], ICON[1] + ICON[3])]
    top_max = max(range(len(rows)), key=lambda i: rows[i])
    band_ok = top_max < int(ICON[3] * 0.45)
    contrast = max(rows) - min(rows)
    check('pseudo-3D icon: brightest row in the top 45%',
          band_ok, 'brightest row %d/%d (%.1f vs min %.1f)'
          % (top_max, ICON[3], max(rows), min(rows)))
    check('pseudo-3D icon has vertical contrast (specular band vs bottom shade)',
          contrast > 20, 'contrast %.1f' % contrast)
    check('pseudo-3D icon highlight is brighter than the panel behind it',
          max(rows) > luminance(edge_ref) + 20,
          'icon max %.1f vs panel %.1f' % (max(rows), luminance(edge_ref)))

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[slint-gui] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[slint-gui] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    print('[slint-gui] screenshot: %s' % SHOT)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
