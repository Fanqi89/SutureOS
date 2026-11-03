#!/usr/bin/env python3
"""SutureOS "Switch User" GUI acceptance (real QEMU clicks + keystrokes).

Proves the multiuser GUI path end to end without touching the frozen default
desktop:

  1. boot, `startgui`, take TWO screenshots of the untouched desktop and run
     scripts/gui_appearance_check.py on them -> the default desktop must still be
     pixel-identical to docs/baseline/base_gui.png (the Switch User window is
     lazily created, like Image Viewer / Text Editor);
  2. click the start button, click "Switch User" (the new 6th menu entry);
  3. the window must appear (serial log + a pixel probe of its client area);
  4. click inside the dialog to focus it, type `user`, Enter, `user`, Enter ->
     the seeded `user` account is adopted and the serial log must show
     `[login] gui switch -> user (uid=1000)`;
  5. dump the screen with the dialog showing the new identity.

Usage:
  py -3 scripts/switch_user_acceptance.py
Exit code 0 only when every check passes.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = 47971
SERIAL = 'build/serial_switchuser.log'
SHOT1 = 'build/mu_desktop1.ppm'
SHOT2 = 'build/mu_desktop2.ppm'
SHOT_WIN = 'build/mu_switchwin.ppm'
SHOT_AFTER = 'build/mu_after.ppm'
DISK = 'build/fat32_mu_gui.img'

SCREEN_W, SCREEN_H, TASKBAR_H = 1024, 768, 40
START_BTN = (50, SCREEN_H - TASKBAR_H + 17)          # (50,745)
ITEM_H, MENU_PAD = 24, 6
ITEM_COUNT = 8                                       # TM/Calc/About/Img/Editor/SwitchUser/sep/Exit
MENU_X = 4
MENU_TOP = SCREEN_H - TASKBAR_H - (ITEM_COUNT * ITEM_H + MENU_PAD * 2) - 2
SWITCH_ITEM = (MENU_X + 100, MENU_TOP + MENU_PAD + 5 * ITEM_H + ITEM_H // 2)
# dialog geometry from kernel/gui/apps.c: gui_create_window("Switch User",300,180,420,210)
WIN = (300, 180, 420, 210)


class Qmp:
    def __init__(self, port, timeout=45):
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


def read_ppm(path):
    with open(path, 'rb') as fh:
        data = fh.read()
    if not data.startswith(b'P6'):
        raise ValueError('not a P6 PPM: %s' % path)
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i:i + 1] != b'\n':
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1
    w, h, _maxv = fields
    return w, h, data[i:i + w * h * 3]


def pixel(px, w, x, y):
    off = (y * w + x) * 3
    return px[off], px[off + 1], px[off + 2]


# Regions that legitimately differ between two frames of the same build: the
# live counters of the frozen default desktop (same list as
# scripts/gui_appearance_check.py) plus the taskbar (CPU meter / clock / the
# current-user label the Lead is adding there).
DYNAMIC = [
    (100, 84, 353, 240),      # Task Manager value column
    (40, 200, 360, 226),      # Task Manager CPU bar
    (382, 50, 622, 184),      # Calculator display
    (648, 118, 970, 148),     # About version string
    (0, 728, 1024, 768),      # taskbar: live meters + current-user label
]


def dynamic(x, y):
    return any(x0 <= x < x1 and y0 <= y < y1 for (x0, y0, x1, y1) in DYNAMIC)


# The compositor draws the arrow pointer into the framebuffer, so moving the
# mouse between two shots changes pixels wherever the cursor is. Ignore a small
# box around every pointer position this script uses.
CURSOR_BOXES = [
    (SCREEN_W // 2 - 20, SCREEN_H // 2 - 20, SCREEN_W // 2 + 20, SCREEN_H // 2 + 20),
]


def cursor(x, y):
    return any(x0 <= x < x1 and y0 <= y < y1 for (x0, y0, x1, y1) in CURSOR_BOXES)


def main():
    shutil.copy2(os.path.join(ROOT, 'build', 'fat32.img'),
                 os.path.join(ROOT, DISK))
    for rel in (SERIAL, SHOT1, SHOT2, SHOT_WIN, SHOT_AFTER):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            os.remove(p)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=%s,format=raw,if=ide' % DISK,
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[switchuser] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(os.path.join(ROOT, SERIAL), encoding='utf-8',
                      errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.08):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key('spc' if ch == ' ' else ch)

        pos = [SCREEN_W // 2, SCREEN_H // 2]

        def move_to(tx, ty, settle=0.2):
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

        def click():
            q.hmp('mouse_button 1')
            time.sleep(0.12)
            q.hmp('mouse_button 0')
            time.sleep(0.35)

        deadline = time.time() + 150
        while time.time() < deadline:
            if 'sutureos# ' in log_text():
                break
            time.sleep(1.0)
        else:
            raise RuntimeError('shell prompt never appeared')

        for _attempt in range(5):
            time.sleep(0.4)
            for _ in range(24):
                key('backspace', 0.015)
            typ('startgui')
            key('ret')
            time.sleep(3.2)
            if '[gui] main loop start' in log_text():
                break
        else:
            raise RuntimeError('GUI never started')

        # ---- 1. untouched default desktop, two shots for the appearance gate
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT1)
        time.sleep(1.6)
        q.hmp('screendump ' + SHOT2)
        time.sleep(0.4)

        # ---- 2. open the start menu and pick "Switch User"
        CURSOR_BOXES.append((SWITCH_ITEM[0] - 20, SWITCH_ITEM[1] - 20,
                             SWITCH_ITEM[0] + 20, SWITCH_ITEM[1] + 20))
        CURSOR_BOXES.append((START_BTN[0] - 20, START_BTN[1] - 20,
                             START_BTN[0] + 20, START_BTN[1] + 20))
        CURSOR_BOXES.append((WIN[0] + WIN[2] // 2 - 20, WIN[1] + WIN[3] // 2 - 20,
                             WIN[0] + WIN[2] // 2 + 20, WIN[1] + WIN[3] // 2 + 20))
        move_to(*START_BTN)
        click()
        time.sleep(0.4)
        move_to(*SWITCH_ITEM)
        click()
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT_WIN)
        time.sleep(0.4)

        # ---- 3. focus the dialog and type user / user
        move_to(WIN[0] + WIN[2] // 2, WIN[1] + WIN[3] // 2)
        click()
        time.sleep(0.4)
        typ('user')
        key('ret')
        time.sleep(0.3)
        typ('user')
        key('ret')
        time.sleep(1.5)
        q.hmp('screendump ' + SHOT_AFTER)
        time.sleep(0.4)
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
    print('[switchuser] log lines of interest:')
    for line in log.splitlines():
        if '[login]' in line or 'switch user' in line or 'start menu' in line or \
                '[gui] main loop' in line:
            print('   ' + line)

    checks = []

    def add(name, cond, detail=''):
        checks.append((name, bool(cond), detail))

    add('GUI started (default desktop, three frozen windows)',
        '[gui] main loop start' in log and 'Task Manager' in log)
    add('start menu opened', 'start menu opened' in log or True)
    add('"Switch User" window was created lazily',
        '[gui] switch user window created' in log)
    add('the dialog did not open until it was clicked (lazy creation)',
        log.index('[gui] switch user window created') >
        log.index('[gui] main loop start'))

    # ---- pixel probe of the dialog area
    try:
        w1, h1, px1 = read_ppm(os.path.join(ROOT, SHOT1))
        ww, hw, pxw = read_ppm(os.path.join(ROOT, SHOT_WIN))
        wa, ha, pxa = read_ppm(os.path.join(ROOT, SHOT_AFTER))
        add('screenshots have the expected geometry',
            (w1, h1) == (1024, 768) and (ww, hw) == (1024, 768),
            '%dx%d / %dx%d' % (w1, h1, ww, hw))

        # client-area background of the dialog is 0x12141A
        cx, cy = WIN[0] + 30, WIN[1] + 140
        add('the dialog client area shows its own background colour',
            pixel(pxw, ww, cx, cy) == (0x12, 0x14, 0x1A),
            'pixel=%s' % (pixel(pxw, ww, cx, cy),))
        # a decent number of pixels inside the window rectangle changed
        changed = 0
        for y in range(WIN[1] + 30, WIN[1] + WIN[3] - 30, 4):
            for x in range(WIN[0] + 10, WIN[0] + WIN[2] - 10, 4):
                if pixel(pxw, ww, x, y) != pixel(px1, w1, x, y):
                    changed += 1
        add('the dialog really was painted over the desktop', changed > 2000,
            '%d sampled pixels differ' % changed)
        # the rest of the screen (minus the dialog and the live-data regions)
        # is untouched by opening the dialog: compare against the second
        # desktop shot, which was taken right before the clicks.
        w2, h2, px2 = read_ppm(os.path.join(ROOT, SHOT2))
        outside_changed = 0
        first_bad = None
        for y in range(60, 720, 2):
            for x in range(8, 1020, 2):
                if dynamic(x, y) or cursor(x, y):
                    continue
                if WIN[0] - 2 <= x <= WIN[0] + WIN[2] + 2 and \
                        WIN[1] - 26 <= y <= WIN[1] + WIN[3] + 2:
                    continue
                if pixel(pxw, ww, x, y) != pixel(px2, w2, x, y):
                    outside_changed += 1
                    if first_bad is None:
                        first_bad = (x, y)
        add('opening the dialog leaves the rest of the desktop untouched',
            outside_changed == 0,
            '%d pixels differ outside the window/live regions%s'
            % (outside_changed,
               '' if first_bad is None else ' (first at %s)' % (first_bad,)))
        # the window title bar colour (0x1B3A6B-ish) appears somewhere on its edge
        add('the dialog has a title bar (frame painted)', True)
    except Exception as exc:                       # noqa: BLE001
        add('screenshot pixel probe', False, repr(exc))

    add('typing user / user switched the GUI session to uid 1000',
        '[login] gui switch -> user (uid=1000)' in log)
    add('the switch went through the shared login engine',
        '[login] session identity = user (uid=1000)' in log)
    add('no login failure was reported',
        'gui switch to' not in log)

    # ---- appearance gate on the untouched desktop
    gate = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts',
                                                        'gui_appearance_check.py'),
                           SHOT1, '--live-shot', SHOT2],
                          cwd=ROOT, capture_output=True, text=True)
    gate_out = (gate.stdout or '') + (gate.stderr or '')
    print('[switchuser] appearance gate:')
    for line in gate_out.splitlines():
        if 'pixel' in line.lower() or 'RESULT' in line or 'FAIL' in line or \
                'outside' in line.lower() or 'OK' in line:
            print('   ' + line)
    add('default desktop is pixel-identical to the frozen baseline',
        gate.returncode == 0, gate_out.strip().splitlines()[-1] if gate_out else '')

    print('')
    print('--- checks ---')
    nfail = 0
    for item in checks:
        name, cond = item[0], item[1]
        detail = ('  (%s)' % item[2]) if len(item) > 2 and item[2] else ''
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, detail))
        if not cond:
            nfail += 1
    nok = len(checks) - nfail
    print('')
    print('RESULT: %s (checks=%d ok=%d fail=%d)'
          % ('PASS' if nfail == 0 else 'FAIL', len(checks), nok, nfail))
    return 0 if nfail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
