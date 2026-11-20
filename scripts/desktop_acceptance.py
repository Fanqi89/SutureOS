#!/usr/bin/env python3
"""SutureOS desktop acceptance (v0.59.0): `startgui` must start the Rust desktop
shell, its start menu must show the pseudo-3D app icons, ESC must return to the
terminal, and `startgui-classic` must still start the original C desktop.

  py -3 scripts/desktop_acceptance.py
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
PORT = int(os.environ.get('DESKTOP_QMP_PORT', '48151'))
LOG = 'build/serial_desktop.log'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H
START_BTN = (8, TASKBAR_Y + 5, 34, 28)
MENU = (12, TASKBAR_Y - 236 - 8, 300, 236)          # MENU_X/Y/W/H mirrored
MENU_ITEM_H = 44
WIN0 = (150, 120, 430, 300)
WIN1 = (470, 300, 400, 260)
ICON_X = MENU[0] + 18
ICON_W = 22


class Qmp:
    def __init__(self, port, timeout=90):
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
    tot = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            tot += p[0] + p[1] + p[2]
            n += 1
    return tot / (3.0 * max(1, n))


def icon_stats(px, x, y0):
    """Colours + top/bottom brightness of a 22x22 menu icon."""
    colours = set()
    top = bot = 0
    for yy in range(22):
        for xx in range(22):
            p = px[x + xx, y0 + yy]
            colours.add(p)
            if yy < 4:
                top += p[0] + p[1] + p[2]
            elif yy >= 18:
                bot += p[0] + p[1] + p[2]
    top /= (3.0 * 22 * 4)
    bot /= (3.0 * 22 * 4)
    return colours, top, bot


def main():
    log_abs = os.path.join(ROOT, LOG)
    if os.path.exists(log_abs):
        os.remove(log_abs)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[desktop] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2
    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[desktop] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in log_text():
                return True
            time.sleep(0.3)
        return False

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    try:
        q = Qmp(PORT)

        def key(k, d=0.08):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key('minus' if ch == '-' else ch)

        def dump(name):
            q.hmp('screendump build/desktop_%s.ppm' % name)
            time.sleep(0.8)

        def load(name):
            from PIL import Image
            return Image.open(os.path.join(ROOT, 'build', 'desktop_%s.ppm' % name)).convert('RGB')

        pos = [SCREEN_W // 2, SCREEN_H - 120]

        def move_to(tx, ty, settle=0.35):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                time.sleep(0.06)
                dx -= sx
                dy -= sy
            pos[0], pos[1] = tx, ty
            time.sleep(settle)

        def click():
            q.hmp('mouse_button 1')
            time.sleep(0.20)
            q.hmp('mouse_button 0')
            time.sleep(0.35)

        if not wait_for('sutureos#', 180):
            print('[desktop] shell prompt never appeared')
            return 1
        time.sleep(0.5)
        typ('startgui')
        key('ret')
        check('startgui starts the Rust desktop shell',
              wait_for('[gui-shell] shell: loop', 120),
              'log line "[gui-shell] shell: loop"')
        check('desktop presents real frames', wait_for('[gui-shell] frame 30', 180))
        check('no C-desktop fallback was taken',
              '[gui] main loop start' not in log_text())

        dump('base')
        base = load('base')
        bpx = base.load()
        check('window 0 painted', mean_brightness(bpx, WIN0[0] + 40, WIN0[1] + 40,
                                                  WIN0[0] + 380, WIN0[1] + 260) > 90,
              'mean %.1f' % mean_brightness(bpx, WIN0[0] + 40, WIN0[1] + 40,
                                            WIN0[0] + 380, WIN0[1] + 260))
        check('window 1 painted', mean_brightness(bpx, WIN1[0] + 30, WIN1[1] + 40,
                                                  WIN1[0] + 360, WIN1[1] + 220) > 80,
              'mean %.1f' % mean_brightness(bpx, WIN1[0] + 30, WIN1[1] + 40,
                                            WIN1[0] + 360, WIN1[1] + 220))
        clock_bright = sum(1 for y in range(TASKBAR_Y + 8, TASKBAR_Y + 32)
                           for x in range(SCREEN_W - 100, SCREEN_W - 8)
                           if sum(bpx[x, y]) > 380)
        check('taskbar clock is drawn', clock_bright > 40,
              '%d bright clock pixels' % clock_bright)
        check('window 0 title bar shows the pseudo-3D app icon',
              len({bpx[x, y] for y in range(WIN0[1] + 8, WIN0[1] + 22)
                   for x in range(WIN0[0] + 9, WIN0[0] + 23)}) >= 4,
              'distinct colours in the title icon')

        # ---- start menu: rows + pseudo-3D icons ---------------------------
        move_to(START_BTN[0] + START_BTN[2] // 2, START_BTN[1] + START_BTN[3] // 2)
        click()
        check('start menu opens from the taskbar', wait_for('[gui-shell] menu open', 40))
        dump('menu')
        menu = load('menu')
        mpx = menu.load()
        check('start menu panel is painted',
              mean_brightness(mpx, MENU[0] + 20, MENU[1] + 40,
                              MENU[0] + 200, MENU[1] + 120) > 55,
              'menu mean %.1f' % mean_brightness(mpx, MENU[0] + 20, MENU[1] + 40,
                                                MENU[0] + 200, MENU[1] + 120))
        stats = []
        for k in range(4):
            y = MENU[1] + 46 + k * MENU_ITEM_H
            stats.append(icon_stats(mpx, ICON_X, y + 8))
        check('start-menu icons are pseudo-3D (specular top + shaded bottom)',
              all(top > bot + 4 for (_c, top, bot) in stats),
              'top/bottom brightness: ' +
              ', '.join('%.0f/%.0f' % (t, b) for (_c, t, b) in stats))
        check('start-menu icons have gradients (not flat squares)',
              all(len(c) >= 4 for (c, _t, _b) in stats),
              'distinct colours per icon: %s' % [len(c) for (c, _t, _b) in stats])
        means = []
        for (c, _t, _b) in stats:
            n = len(c)
            means.append([sum(col[i] for col in c) / n for i in range(3)])
        spread = max(max(abs(means[a][i] - means[b][i])
                         for i in range(3))
                     for a in range(4) for b in range(a + 1, 4))
        check('each app icon carries its own accent colour', spread > 25,
              'largest accent channel difference %.1f' % spread)
        # close the menu with a click on the desktop, then leave with ESC
        move_to(620, 600)
        click()
        time.sleep(0.6)

        # ---- ESC leaves the desktop ---------------------------------------
        key('esc')
        check('ESC leaves the Rust desktop',
              wait_for('[gui-shell] shell: exit', 60),
              'log line "[gui-shell] shell: exit"')
        check('terminal comes back after the desktop',
              wait_for('Desktop exited, returning to terminal', 60))

        # ---- the original C desktop is still reachable ---------------------
        time.sleep(0.5)
        typ('startgui-classic')
        key('ret')
        check('startgui-classic still starts the C desktop',
              wait_for('[gui] main loop start', 120),
              'log line "[gui] main loop start"')
        dump('classic')
        classic = load('classic')
        cpx = classic.load()
        check('C desktop is painted (legacy path alive)',
              mean_brightness(cpx, 200, 150, 540, 360) > 80,
              'classic window mean %.1f' % mean_brightness(cpx, 200, 150, 540, 360))
        key('q')
        check('C desktop exits back to the terminal',
              wait_for('GUI exited, returning to terminal', 60))

        # ---- desktop is the default entry point ----------------------------
        check('boot presentation unchanged (startgui is still an explicit command)',
              'Type \'startgui\' to launch GUI interface' in log_text())
    finally:
        try:
            q.hmp('quit')
        except Exception:                    # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    check('no kernel PANIC', 'PANIC' not in log_text())
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[desktop] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[desktop] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
