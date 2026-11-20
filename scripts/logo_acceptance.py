#!/usr/bin/env python3
"""SutureOS logo acceptance: start-button icon, start-menu header and shutdown.

Boots the real ISO, opens the Rust shell prototype (`guishell`), takes a
screendump and compares the on-screen 24x24 start-button icon and the 48x48
start-menu header pixel-by-pixel against the PNG ladder in `logo/`. Then it
sends `s` and checks that the reverse logo animation runs and the machine powers
off (the logo animation must finish without waiting for user input).

  py -3 scripts/logo_acceptance.py
"""
import json
import os
import socket
import subprocess
import sys
import time

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = 45815
SERIAL = 'build/serial_logo.log'
LOG_ABS = os.path.join(ROOT, SERIAL)
SHOT_SHELL = 'build/logo_shell.ppm'
SHOT_MENU = 'build/logo_menu.ppm'

TASKBAR_Y = 768 - 38
START_BTN = (8, TASKBAR_Y + 5, 34, 28)
MENU = (12, TASKBAR_Y - 236 - 8, 300, 236)   # MENU_X, MENU_Y, MENU_W, MENU_H


class Qmp:
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:
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


KEY = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus'}


def type_text(q, text, delay=0.08):
    for ch in text:
        q.hmp('sendkey ' + KEY.get(ch, ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(0.3)


def compare_region(shot_path, box, png_path, bg_x0, bg_x1):
    """Alpha-aware comparison of a 1:1 logo blit.

    Returns opaque_count / opaque_bad / translucent / clear_count / clear_white /
    clear_mean / bg_mean. Only opaque pixels are compared exactly; transparent
    pixels are judged by "not white" plus a mean-tracking test, because the
    blurred-wallpaper background legitimately varies across a 48 px icon.
    """
    shot = Image.open(shot_path).convert('RGB')
    want = Image.open(png_path).convert('RGBA')
    x, y, w, h = box
    got = shot.crop((x, y, x + w, y + h))
    if got.size != want.size:
        return {'opaque_count': 0, 'opaque_bad': 10 ** 6, 'translucent': 0,
                'clear_count': 0, 'clear_white': 0,
                'clear_mean': (0, 0, 0), 'bg_mean': (0, 0, 0)}
    gp = got.load()
    wp = want.load()
    opaque = opaque_bad = translucent = clear = clear_white = 0
    clear_sum = [0, 0, 0]
    bg_sum = [0, 0, 0]
    bg_n = 0
    for row in range(h):
        strip = [shot.getpixel((sx, y + row)) for sx in range(bg_x0, bg_x1)]
        bg = tuple(sum(c[i] for c in strip) / len(strip) for i in range(3))
        for i in range(3):
            bg_sum[i] += bg[i]
        bg_n += 1
        for col in range(w):
            r, g, b, a = wp[col, row]
            gotp = gp[col, row]
            if a >= 250:
                opaque += 1
                if max(abs(gotp[i] - (r, g, b)[i]) for i in range(3)) > 4:
                    opaque_bad += 1
            elif a == 0:
                clear += 1
                for i in range(3):
                    clear_sum[i] += gotp[i]
                if min(gotp) > 235:          # the old white square
                    clear_white += 1
            else:
                translucent += 1
    clear_mean = tuple(clear_sum[i] / max(1, clear) for i in range(3))
    bg_mean = tuple(bg_sum[i] / max(1, bg_n) for i in range(3))
    return {'opaque_count': opaque, 'opaque_bad': opaque_bad,
            'translucent': translucent, 'clear_count': clear,
            'clear_white': clear_white,
            'clear_mean': clear_mean, 'bg_mean': bg_mean}


def main():
    for p in (LOG_ABS, os.path.join(ROOT, SHOT_SHELL), os.path.join(ROOT, SHOT_MENU)):
        if os.path.exists(p):
            os.remove(p)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-vga', 'std', '-display', 'none',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-smp', '2', '-no-reboot']
    print('[logo] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def logtext():
        try:
            with open(LOG_ABS, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if needle in logtext():
                return True
            time.sleep(0.3)
        return False

    checks = []
    try:
        ok_prompt = False
        deadline = time.time() + 90
        while time.time() < deadline:
            if 'sutureos# ' in logtext():
                ok_prompt = True
                break
            time.sleep(0.4)
        checks.append(('boot reaches the shell', ok_prompt))
        q = Qmp(QMP_PORT)
        type_text(q, 'guishell')
        checks.append(('guishell starts', wait_for('[gui-shell] shell: loop started', 60)))
        # let a few desktop frames render
        time.sleep(2.5)
        q.hmp('screendump ' + SHOT_SHELL)
        time.sleep(1.5)
        r24 = compare_region(
            os.path.join(ROOT, SHOT_SHELL),
            (START_BTN[0] + 5, START_BTN[1] + 2, 24, 24),
            os.path.join(ROOT, 'logo', 'SutureOS_logo_24.png'),
            START_BTN[0] + 2, START_BTN[0] + 5)
        checks.append(('start-button logo: opaque pixels match the 24px PNG '
                       '(%d bad / %d opaque, %d blended)'
                       % (r24['opaque_bad'], r24['opaque_count'],
                          r24['translucent']),
                       r24['opaque_bad'] <= max(2, r24['opaque_count'] // 50)
                       and r24['opaque_count'] > 80
                       and r24['translucent'] > 20))
        checks.append(('start-button logo: transparent pixels show the taskbar '
                       '(%s vs bg %s, %d white)'
                       % (tuple(round(v) for v in r24['clear_mean']),
                          tuple(round(v) for v in r24['bg_mean']),
                          r24['clear_white']),
                       r24['clear_count'] > 20
                       and r24['clear_white'] <= r24['clear_count'] // 50
                       and max(abs(r24['clear_mean'][i] - r24['bg_mean'][i])
                               for i in range(3)) <= 26))
        # open the start menu and check its 48px header icon
        # guishell puts the cursor at (w/2, h-120) = (512,648); HMP mouse_move is
        # relative, so move by the delta to the start button centre.
        _dx = (START_BTN[0] + START_BTN[2] // 2) - 512
        _dy = (START_BTN[1] + START_BTN[3] // 2) - 648
        q.hmp('mouse_move %d %d' % (_dx, _dy))
        time.sleep(0.3)
        q.hmp('mouse_button 1')
        time.sleep(0.12)
        q.hmp('mouse_button 0')
        checks.append(('start menu opens', wait_for('[gui-shell] menu open', 40)))
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT_MENU)
        time.sleep(1.5)
        r48 = compare_region(
            os.path.join(ROOT, SHOT_MENU),
            (MENU[0] + 10, MENU[1] + 2, 48, 48),
            os.path.join(ROOT, 'logo', 'SutureOS_logo_48.png'),
            MENU[0] + 2, MENU[0] + 8)
        checks.append(('start-menu logo: opaque pixels match the 48px PNG '
                       '(%d bad / %d opaque, %d blended)'
                       % (r48['opaque_bad'], r48['opaque_count'],
                          r48['translucent']),
                       r48['opaque_bad'] <= max(4, r48['opaque_count'] // 50)
                       and r48['opaque_count'] > 300))
        checks.append(('start-menu logo: transparent pixels show the panel '
                       '(%s vs bg %s, %d white)'
                       % (tuple(round(v) for v in r48['clear_mean']),
                          tuple(round(v) for v in r48['bg_mean']),
                          r48['clear_white']),
                       r48['clear_count'] > 40
                       and r48['clear_white'] <= r48['clear_count'] // 50
                       and max(abs(r48['clear_mean'][i] - r48['bg_mean'][i])
                               for i in range(3)) <= 26))
        # close the menu first (the shell only handles F10 when the menu is closed)
        q.hmp('mouse_move 200 100')
        time.sleep(0.3)
        q.hmp('mouse_button 1')
        time.sleep(0.12)
        q.hmp('mouse_button 0')
        checks.append(('start menu closes', wait_for('[gui-shell] menu closed', 30)))
        time.sleep(0.5)
        # shutdown: reverse logo animation then ACPI power off
        q.hmp('sendkey f10')
        checks.append(('shutdown animation starts', wait_for('[gui-shell] logo shutdown start', 40)))
        time.sleep(0.5)
        checks.append(('shutdown animation frames logged',
                       wait_for('[gui-shell] logo shutdown 3', 60)))
        checks.append(('shutdown animation finishes', wait_for('[gui-shell] logo shutdown done', 90)))
        checks.append(('ACPI power off called', wait_for('[gui-shell] power off', 30)))
        try:
            proc.wait(timeout=30)
            checks.append(('QEMU exited after power off', True))
        except subprocess.TimeoutExpired:
            checks.append(('QEMU exited after power off', False))
            proc.kill()
    finally:
        if proc.poll() is None:
            try:
                proc.kill()
            except OSError:
                pass
        try:
            os.remove(LOG_ABS + '.tmp')
        except OSError:
            pass

    log = logtext()
    checks.append(('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')))
    failed = [n for n, ok in checks if not ok]
    for name, ok in checks:
        print('  [%s] %s' % ('ok' if ok else 'FAIL', name))
    print('[logo] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[logo] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
