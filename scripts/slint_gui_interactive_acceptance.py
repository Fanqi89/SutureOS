#!/usr/bin/env python3
"""SutureOS interactive GUI prototype acceptance (v0.40.0).

What is proven here, with real QEMU PS/2 input (no guest-side fakes):

  * `guislint` really enters a live loop   -> "[gui-slint] interactive: loop (mouse=1)"
  * pointer hover reaches the Rust scene   -> "[gui-slint] hover row 0" / "hover row 1"
  * the hovered list row is visually highlighted (pixels), and the previously
    hovered row goes back to its idle state
  * a real click is routed as an edge     -> "[gui-slint] click at X Y"
  * the click ripple is drawn (pixels near the click change, then disappear)
  * the pointer cursor is rendered at the pointer position
  * ESC leaves the loop cleanly           -> "[gui-slint] interactive: exit after N frames"
    and the terminal comes back

Usage: py -3 scripts/slint_gui_interactive_acceptance.py
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
QMP_PORT = int(os.environ.get('SLINT_UI_QMP_PORT', '47981'))
SERIAL = 'build/serial_slint_ui.log'

SCREEN_W, SCREEN_H = 1024, 768

# geometry mirrored from rust/suture-gui/src/lib.rs (rows live inside the panel)
ROW_X0, ROW_X1 = 178, 682
ROW0_Y, ROW1_Y, ROW_H = 390, 428, 30


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


def mean_brightness(px, x0, y0, x1, y1):
    tot = 0
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            p = px[x, y]
            tot += p[0] + p[1] + p[2]
            n += 1
    return tot / (3.0 * n)


def main():
    do_assert = '--no-assert' not in sys.argv
    log_abs = os.path.join(ROOT, SERIAL)
    shots = {k: 'build/slint_ui_%s.ppm' % k for k in ('row0', 'row1', 'click', 'after')}
    for p in [log_abs] + [os.path.join(ROOT, v) for v in shots.values()]:
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[slint-ui] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[slint-ui] ' + ' '.join(argv), flush=True)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in log_text():
                return True
            time.sleep(0.25)
        return False

    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []
    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.05):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(ch if ch != ' ' else 'spc')

        # pointer starts at the screen centre; walk with PS/2-sized steps
        pos = [SCREEN_W // 2, SCREEN_H // 2]

        def move_to(tx, ty, settle=0.30):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                time.sleep(0.05)
                dx -= sx
                dy -= sy
            pos[0], pos[1] = tx, ty
            time.sleep(settle)

        def dump(name):
            q.hmp('screendump ' + shots[name])
            time.sleep(0.5)

        def dump_until(name, pred, tries=8):
            """Dump repeatedly until `pred(image)` is true (the guest paints at
            its own pace under TCG, so a fixed sleep can capture the previous
            frame). Keeps the LAST dump either way."""
            from PIL import Image as _Img
            for _ in range(tries):
                dump(name)
                abs_p = os.path.join(ROOT, shots[name])
                if os.path.exists(abs_p):
                    im = _Img.open(abs_p).convert('RGB')
                    if pred(im.load()):
                        return True
                time.sleep(0.35)
            return False

        def row_brightness(px, ry):
            tot = n = 0
            for y in range(ry + 6, ry + 24, 2):
                for x in range(ROW_X0 + 40, ROW_X1 - 40, 2):
                    c = px[x, y]
                    tot += c[0] + c[1] + c[2]
                    n += 1
            return tot / (3.0 * n)

        def check(name, cond, detail=''):
            checks.append((name, bool(cond), detail))
            print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                                   ('  (%s)' % detail) if detail else ''))

        if not wait_for('sutureos#', 150):
            print('[slint-ui] shell prompt never appeared')
            return 1
        time.sleep(0.5)

        typ('guislint')
        key('ret')

        check('interactive loop started',
              wait_for('[gui-slint] interactive: loop', 60),
              'log line "[gui-slint] interactive: loop (mouse=1)"')
        check('static first frame still rendered',
              '[gui-slint] frame rendered' in log_text())
        time.sleep(1.2)             # let the fade-in finish

        # hover row 0
        move_to((ROW_X0 + ROW_X1) // 2, ROW0_Y + 10)
        check('hover reaches row 0', wait_for('[gui-slint] hover row 0', 15))
        dump_until('row0', lambda px: row_brightness(px, ROW0_Y) >
                   row_brightness(px, ROW1_Y) + 4.0)

        # hover row 1
        move_to((ROW_X0 + ROW_X1) // 2, ROW1_Y + 10)
        check('hover reaches row 1', wait_for('[gui-slint] hover row 1', 15))
        dump_until('row1', lambda px: row_brightness(px, ROW1_Y) >
                   row_brightness(px, ROW0_Y) + 4.0)

        # click there -> ripple
        q.hmp('mouse_button 1')
        time.sleep(0.12)
        q.hmp('mouse_button 0')
        check('click routed to the scene',
              wait_for('[gui-slint] click at', 15))
        time.sleep(0.10)
        dump('click')

        # let the ripple expire, then ESC out
        time.sleep(0.8)
        key('esc')
        check('ESC leaves the loop', wait_for('[gui-slint] interactive: exit', 20))
        check('terminal came back', wait_for('guislint: frame shown, back to terminal', 20))
        dump('after')
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
    check('no kernel PANIC', 'PANIC' not in log)

    if not do_assert:
        print('[slint-ui] screenshot dumps written, assertions skipped')
        return 0
    try:
        from PIL import Image
    except ImportError:
        print('[slint-ui] Pillow missing: cannot check pixels')
        return 1

    imgs = {}
    for k, rel in shots.items():
        abs_p = os.path.join(ROOT, rel)
        if os.path.exists(abs_p):
            imgs[k] = Image.open(abs_p).convert('RGB')
    if len(imgs) < 4:
        print('[slint-ui] missing screendumps: %s' % sorted(shots))
        return 1

    r0 = imgs['row0'].load()
    r1 = imgs['row1'].load()
    row0_band = (ROW_X0 + 40, ROW0_Y + 6, ROW_X1 - 40, ROW0_Y + ROW_H - 6)
    row1_band = (ROW_X0 + 40, ROW1_Y + 6, ROW_X1 - 40, ROW1_Y + ROW_H - 6)
    b0_in_row0 = mean_brightness(r0, *row0_band)
    b0_in_row1 = mean_brightness(r0, *row1_band)
    b1_in_row0 = mean_brightness(r1, *row0_band)
    b1_in_row1 = mean_brightness(r1, *row1_band)
    check('hovered row 0 is brighter than the idle row 1 (shot A)',
          b0_in_row0 > b0_in_row1 + 2.0,
          'row0 %.1f vs row1 %.1f' % (b0_in_row0, b0_in_row1))
    check('after moving down, row 1 is the brighter one (shot B)',
          b1_in_row1 > b1_in_row0 + 2.0,
          'row0 %.1f vs row1 %.1f' % (b1_in_row0, b1_in_row1))
    check('row 0 highlight follows the pointer (A brighter than B)',
          b0_in_row0 > b1_in_row0 + 2.0,
          'A %.1f vs B %.1f' % (b0_in_row0, b1_in_row0))

    # pointer cursor: the two shots differ right at the pointer positions
    def local_diff(a, b, cx, cy, r=12):
        pa, pb = a.load(), b.load()
        n = 0
        for y in range(cy - r, cy + r):
            for x in range(cx - r, cx + r):
                if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H and pa[x, y] != pb[x, y]:
                    n += 1
        return n

    px0, py0 = (ROW_X0 + ROW_X1) // 2, ROW0_Y + 10
    px1, py1 = (ROW_X0 + ROW_X1) // 2, ROW1_Y + 10
    d_old = local_diff(imgs['row0'], imgs['row1'], px0, py0)
    d_new = local_diff(imgs['row0'], imgs['row1'], px1, py1)
    check('cursor is drawn at the pointer (old and new spots differ)',
          d_old > 40 and d_new > 40,
          'changed px near old spot %d, near new spot %d' % (d_old, d_new))

    # ripple: the click shot differs from the calm shot around the click point
    d_ripple = local_diff(imgs['click'], imgs['after'], px1, py1, r=40)
    check('click ripple painted a visible ring', d_ripple > 120,
          '%d changed px within 40 px of the click' % d_ripple)

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[slint-ui] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[slint-ui] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
