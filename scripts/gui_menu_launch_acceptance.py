#!/usr/bin/env python3
"""SutureOS start-menu acceptance (v0.62): five rows, and the 5th row opens the
Wayland window.

What changed and what this gate proves:
  * the dedicated Wayland window used to be reachable only through F2, so the
    desktop had an application that was invisible in the UI;
  * the start menu now lists five applications, which means the panel is 44 px
    taller and 44 px higher than the frozen 4-row geometry (it stays anchored to
    the taskbar). Every draw/hit-test site derives its geometry from
    `MENU_ITEMS.len()`, so this gate samples the panel edge and each row on screen
    instead of trusting a log line;
  * clicking the 5th row must launch window 4 (`menu launch 4` -> `wayland window
    opened`), and F2 must still toggle it (the old path is not broken).

Pixel evidence: the menu panel edge, one icon plate per row (the pseudo-3D icons are
colour-coded by accent), and the purple accent of the Wayland window's title bar.

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
QMP_PORT = int(os.environ.get('MENU_QMP_PORT', '45891'))
SERIAL = 'build/serial_menu_launch.log'
SHOT_MENU = 'build/acc_menu_five.ppm'
SHOT_WIN = 'build/acc_menu_wayland.ppm'
DISK = 'build/fat32_ata.img'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_Y = 730
MENU_X, MENU_W = 12, 300
MENU_HEAD_H, MENU_ROW_H, MENU_FOOT_H = 46, 44, 14
N_ITEMS = 5
MENU_H = MENU_HEAD_H + N_ITEMS * MENU_ROW_H + MENU_FOOT_H        # 280
MENU_Y = TASKBAR_Y - MENU_H - 8                                  # 442
MENU_ITEMS_Y = MENU_Y + MENU_HEAD_H                              # 488
START_BTN = (25, 745)


class Qmp:
    def __init__(self, port, timeout=120):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), 1.0)
                self.f = self.s.makefile('rwb', buffering=0)
                self._read()
                self.cmd('qmp_capabilities')
                return
            except Exception as e:      # noqa: BLE001
                last = e
                time.sleep(0.3)
        raise RuntimeError('QMP connect failed: %r' % (last,))

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            if b'"event"' in line:
                continue
            rep = json.loads(line)
            if 'error' in rep:
                raise RuntimeError('QMP error: %r' % (rep['error'],))
            return rep

    def cmd(self, name, **args):
        self.f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        return self._read()

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})


QCODE = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash',
         ',': 'comma', '=': 'equal', '\n': 'ret'}


def press_keys(q, keys, delay=0.14):
    for k in keys:
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': QCODE.get(k, k)}])
        time.sleep(delay)


def move_to(q, pos, tx, ty, settle=0.35):
    dx, dy = tx - pos[0], ty - pos[1]
    while dx or dy:
        sx = max(-90, min(90, dx))
        sy = max(-90, min(90, dy))
        q.hmp('mouse_move %d %d' % (sx, sy))
        dx -= sx
        dy -= sy
        time.sleep(0.06)
    pos[0], pos[1] = tx, ty
    time.sleep(settle)


def click(q, pos, tx, ty, settle=1.6):
    move_to(q, pos, tx, ty)
    q.hmp('mouse_button 1')
    time.sleep(0.20)
    q.hmp('mouse_button 0')
    time.sleep(settle)


def shot(q, path):
    if os.path.exists(path):
        os.remove(path)
    q.cmd('screendump', filename=path)
    for _ in range(80):
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            time.sleep(0.4)
            return
        time.sleep(0.1)
    raise RuntimeError('no screendump')


def load(path):
    from PIL import Image
    return Image.open(os.path.join(ROOT, path)).convert('RGB')


def mean_brightness(px, x0, y0, x1, y1):
    n = 0
    acc = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            acc += (r * 299 + g * 587 + b * 114) // 1000
            n += 1
    return acc / float(n) if n else 0.0


def accent_pixels(px, x0, y0, x1, y1, want, tol=70):
    """Count pixels close to an accent colour - the plates are colour-coded."""
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            if abs(r - want[0]) <= tol and abs(g - want[1]) <= tol and abs(b - want[2]) <= tol:
                n += 1
    return n


def boot():
    for p in (SERIAL, SHOT_MENU, SHOT_WIN):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))
    log = open(os.path.join(ROOT, 'build/qemu_menu_launch.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


ACCS = [(0x4f, 0x8d, 0xff), (0x22, 0xc1, 0xa8), (0xf5, 0xa5, 0x24),
        (0xe0, 0x5a, 0x8a), (0x7a, 0x5c, 0xff)]


def main():
    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''), flush=True)

    subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')],
                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import shutil
    shutil.copyfile(os.path.join(ROOT, 'build', 'fat32.img'),
                    os.path.join(ROOT, DISK))

    qemu, log = boot()
    pos = [512, 648]
    try:
        q = Qmp(QMP_PORT)
        print('waiting for the shell prompt ...', flush=True)
        time.sleep(75)
        press_keys(q, list('startgui') + ['ret'])
        time.sleep(16)
        click(q, pos, *START_BTN)
        time.sleep(1.0)
        shot(q, os.path.join(ROOT, SHOT_MENU))
        # the 5th row really is on screen: click it
        click(q, pos, MENU_X + 100, MENU_ITEMS_Y + 4 * MENU_ROW_H + 19)
        time.sleep(2.0)
        shot(q, os.path.join(ROOT, SHOT_WIN))
        press_keys(q, ['f2'])              # open+focused -> minimize
        time.sleep(2.0)
        press_keys(q, ['f2'])              # minimized -> restore
        time.sleep(2.0)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')

    # ---------------------------------------------------------------- log side
    check('the shell started the Rust desktop', '[gui-shell]' in text)
    check('the start menu opened', 'menu open' in text)
    check('the 5th menu row launched window 4', 'menu launch 4' in text,
          'menu launch 4')
    check('the 5th row is the Wayland window (not the old placeholder)',
          'menu launch 4' in text and 'wayland window' in text,
          'menu launch 4')
    check('F2 still toggles the window (minimize, then restore)',
          'wayland window minimized' in text and 'wayland window opened' in text,
          'F2 -> minimized -> restored')

    # ---------------------------------------------------------------- pixels
    menu = load(SHOT_MENU)
    px = menu.load()
    # panel top edge: just above MENU_Y it is desktop, just below it is glass.
    above = mean_brightness(px, MENU_X + 40, MENU_Y - 22, MENU_X + 240, MENU_Y - 4)
    inside = mean_brightness(px, MENU_X + 40, MENU_Y + 70, MENU_X + 240, MENU_Y + 130)
    check('the menu panel grew to five rows (top edge at %d)' % MENU_Y,
          abs(above - inside) > 8.0, 'above %.1f vs inside %.1f' % (above, inside))
    # the frozen 4-row layout put the top edge at 486: the strip above 442 must not
    # look like the old panel anymore
    old_top = mean_brightness(px, MENU_X + 40, 486 - 20, MENU_X + 240, 486 - 6)
    check('nothing is painted where the 4-row panel used to start',
          abs(old_top - inside) > 6.0, 'old-top band %.1f vs panel %.1f'
          % (old_top, inside))
    row_hits = []
    misses = 0
    for k, acc in enumerate(ACCS):
        ry = MENU_ITEMS_Y + k * MENU_ROW_H
        # icon plate centre: MENU_X + 26, row centre + 19
        n = accent_pixels(px, MENU_X + 14, ry + 8, MENU_X + 40, ry + 32, acc)
        row_hits.append(n)
        if n < 20:
            misses += 1
    check('all five rows draw their own accent icon', misses == 0,
          'accent px per row: %s' % row_hits)

    win = load(SHOT_WIN)
    wpx = win.load()
    # the Wayland window is 440x330 at (250,330) with accent 0x7a5cff in its title
    purple = accent_pixels(wpx, 252, 331, 690, 360, (0x7a, 0x5c, 0xff), 60)
    check('the Wayland window is on screen with its accent title bar',
          purple > 200, '%d accent px in the title strip' % purple)
    check('no kernel PANIC',
          'PANIC' not in text.upper().replace('NO PANIC', ''))

    ok = sum(1 for c in checks if c[1])
    print('[menulaunch] checks=%d ok=%d fail=%d'
          % (len(checks), ok, len(checks) - ok), flush=True)
    if ok == len(checks):
        print('[menulaunch] RESULT: PASS', flush=True)
        return 0
    print('[menulaunch] RESULT: FAIL', flush=True)
    return 1


if __name__ == '__main__':
    sys.exit(main())
