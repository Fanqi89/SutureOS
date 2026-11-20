#!/usr/bin/env python3
"""SutureOS: the ring-3 Wayland client's committed surface must appear inside the
desktop's image-viewer window (compositor integration).

Flow: boot -> `run wlclient` (client commits a 320x200 shm surface) -> `startgui`
-> open 图片查看器 from the start menu -> the window paints the surface instead of
the logo preview (`[gui-shell] wl surface 320x200 slot 2`).

  py -3 scripts/wl_desktop_window_acceptance.py
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = int(os.environ.get('WL_WINDOW_QMP_PORT', '45931'))
LOG = 'build/serial_wl_window.log'
SHOT = 'build/wl_window.ppm'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H
START_BTN = (8, TASKBAR_Y + 5, 34, 28)
MENU = (12, TASKBAR_Y - 236 - 8, 300, 236)
MENU_ITEM_H = 44
WIN2 = (210, 430, 360, 240)
ACCENT = (0xf5, 0xa5, 0x24)


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


def main():
    log_abs = os.path.join(ROOT, LOG)
    for p in [log_abs, os.path.join(ROOT, SHOT)]:
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[wlwin] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2
    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[wlwin] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, timeout=180):
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

        def type_line(text, d=0.12):
            for ch in text:
                q.hmp('sendkey ' + ('spc' if ch == ' ' else ch))
                time.sleep(d)
            time.sleep(0.2)
            q.hmp('sendkey ret')

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
            print('[wlwin] shell prompt never appeared')
            return 1
        time.sleep(0.5)
        type_line('run wlclient')
        check('ring-3 wlclient commits a surface',
              wait_for('[wlclient] RESULT PASS', 200),
              'log line "[wlclient] RESULT PASS"')
        commit = [l for l in log_text().splitlines() if '[wl] commit ' in l]
        check('kernel Wayland server logged the commit', bool(commit),
              (commit[-1] if commit else 'no [wl] commit line'))
        time.sleep(0.5)
        type_line('startgui')
        check('startgui starts the Rust desktop',
              wait_for('[gui-shell] shell: loop', 150))
        check('desktop presents frames', wait_for('[gui-shell] frame 30', 180))

        # open the image viewer (menu row 2) with real mouse clicks
        move_to(START_BTN[0] + START_BTN[2] // 2, START_BTN[1] + START_BTN[3] // 2)
        click()
        check('start menu opens', wait_for('[gui-shell] menu open', 40))
        move_to(MENU[0] + 80, MENU[1] + 46 + 2 * MENU_ITEM_H + 19)
        click()
        check('image viewer window launched',
              wait_for('[gui-shell] menu launch 2', 60))
        check('the window paints the committed Wayland surface',
              wait_for('[gui-shell] wl surface 320x200', 90),
              'log line "[gui-shell] wl surface 320x200 slot 2"')
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT)
        time.sleep(1.0)
    finally:
        try:
            q.hmp('quit')
        except Exception:                    # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    from PIL import Image
    shot_path = os.path.join(ROOT, SHOT)
    if os.path.exists(shot_path):
        im = Image.open(shot_path).convert('RGB')
        px = im.load()
        x0, y0 = WIN2[0] + 18, WIN2[1] + 60
        x1, y1 = WIN2[0] + WIN2[2] - 18, WIN2[1] + WIN2[3] - 26
        saturated = 0
        colours = set()
        for y in range(y0, y1, 2):
            for x in range(x0, x1, 2):
                p = px[x, y]
                colours.add(p)
                if max(p) - min(p) > 80:
                    saturated += 1
        distinct = len(colours)
        # the frame is the accent colour at alpha 0x70 over the frosted body, so
        # it is a muted warm tint (r >= b) rather than the pure accent; the body
        # behind the canvas is bluish (b > r). Sample the two frame lines.
        PAD = 16
        frame_row = WIN2[1] + 30 + 8 + 24 - 1
        frame_col = WIN2[0] + PAD - 1
        warm_row = [px[x, frame_row] for x in range(WIN2[0] + PAD, WIN2[0] + PAD + 300)]
        warm_col = [px[frame_col, y] for y in range(WIN2[1] + 60, WIN2[1] + 200)]
        warm = sum(1 for p in warm_row + warm_col if p[0] > p[2])
        warm_frac = warm / float(len(warm_row) + len(warm_col))
        check('window body shows the surface colours (saturated pixels)',
              saturated > 300, '%d saturated samples' % saturated)
        check('surface has many distinct colours (a painted image, not a matte)',
              distinct > 200, '%d distinct colours' % distinct)
        check('the surface accent frame is drawn (warm tint over the body)',
              warm_frac > 0.7, '%.0f%% of the frame line is warm' % (100 * warm_frac))
    else:
        check('screendump written', False)

    check('no kernel PANIC', 'PANIC' not in log_text())
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[wlwin] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[wlwin] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
