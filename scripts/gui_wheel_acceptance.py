#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS v0.65 acceptance: the PS/2 wheel, end to end.

The mouse driver used to hard-code 3-byte packets, so a wheel was ignored. It now
does the IntelliMouse handshake (magic sample rates 200/100/80), switches to
4-byte packets when the device reports id 3/4, and the Rust shell consumes the Z
nibble each frame:

  * `mouseinfo` reports device id=0x3 (wheel) and packet=4 bytes,
  * `mouseinfo wheel` feeds synthetic 4-byte packets through the real decoder and
    proves BOTH directions (z=+1, z=-1, accumulation) - needed because QEMU's
    PS/2 model only injects wheel-up, never wheel-down,
  * with the start menu open a notch scrolls it one row
        (wheel-up -> "wheel 1 menu=1" -> "menu scroll 0 1"),
  * with the picture viewer focused a notch browses the previous/next file
        (wheel-up -> "image shift 2" -> "image: loaded IMG.QOI"),
  * no kernel PANIC.

Usage: py -3 scripts/gui_wheel_acceptance.py
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
QMP_PORT = int(os.environ.get('WHEEL_QMP_PORT', '45931'))
SERIAL = 'build/serial_wheel.log'
SHOT = 'build/acc_wheel_menu.ppm'
DISK = 'build/fat32_ata.img'

TASKBAR_Y = 730
MENU_X, MENU_W = 12, 300
MENU_HEAD_H, MENU_ROW_H, MENU_FOOT_H = 46, 44, 14
N_VISIBLE = 6
MENU_H = MENU_HEAD_H + N_VISIBLE * MENU_ROW_H + MENU_FOOT_H        # 324
MENU_Y = TASKBAR_Y - MENU_H - 8                                    # 398
MENU_ITEMS_Y = MENU_Y + MENU_HEAD_H                                # 444
START_BTN = (25, 745)
IMAGE_ROW = 2
SCROLL_STRIP_X = 303


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

    def wheel(self, button, settle=1.1):
        self.cmd('input-send-event',
                 events=[{'type': 'btn', 'data': {'down': True, 'button': button}}])
        time.sleep(0.08)
        self.cmd('input-send-event',
                 events=[{'type': 'btn', 'data': {'down': False, 'button': button}}])
        time.sleep(settle)


QCODE = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash',
         ',': 'comma', '=': 'equal', '\n': 'ret'}


def press_keys(q, keys, delay=0.16):
    for k in keys:
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': QCODE.get(k, k)}])
        time.sleep(delay)


def move_to(q, pos, tx, ty, settle=0.3):
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


def click(q, pos, tx, ty, settle=1.8):
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


def accent_pixels(px, x0, y0, x1, y1, want, tol=64):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            if abs(r - want[0]) <= tol and abs(g - want[1]) <= tol and abs(b - want[2]) <= tol:
                n += 1
    return n


def boot():
    for p in (SERIAL, SHOT):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))
    log = open(os.path.join(ROOT, 'build/qemu_wheel.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


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
        # 1. driver mode + synthetic both-direction decoder test
        press_keys(q, list('mouseinfo wheel') + ['ret'])
        time.sleep(2)
        press_keys(q, list('mouseinfo') + ['ret'])
        time.sleep(2)
        press_keys(q, list('startgui') + ['ret'])
        time.sleep(16)
        # 2. open the menu, page it down with the scrollbar, scroll back with the wheel
        click(q, pos, *START_BTN)
        click(q, pos, SCROLL_STRIP_X, MENU_ITEMS_Y + 200, settle=1.4)
        shot(q, os.path.join(ROOT, SHOT))
        q.wheel('wheel-up')
        q.wheel('wheel-up')
        time.sleep(0.8)
        # 3. picture viewer: wheel-up browses to the previous file (wraps to QOI)
        click(q, pos, MENU_X + 100, MENU_ITEMS_Y + IMAGE_ROW * MENU_ROW_H + 19, settle=2.2)
        q.wheel('wheel-up')
        time.sleep(1.4)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    check('the shell started the Rust desktop', '[gui-shell]' in text)
    check('the PS/2 mouse came up as an IntelliMouse (id 3, 4-byte packets)',
          'device id=0x3 (wheel)' in text and 'packet=4 bytes' in text,
          'device id=0x3 / packet=4 bytes')
    check('both wheel directions decode (synthetic 4-byte packets)',
          '[mouse] wheel selftest: z=+1, z=-1, accumulate -> PASS' in text,
          'wheel selftest PASS')
    check('the shell consumes wheel notches', 'wheel -1 menu=1' in text,
          'wheel lines: %d' % text.count('[gui-shell] wheel'))
    check('a wheel notch scrolls the open menu',
          'menu scroll 1 1' in text and 'menu scroll 0 1' in text,
          'scroll 1 x%d / scroll 0 x%d'
          % (text.count('menu scroll 1 1'), text.count('menu scroll 0 1')))
    check('with the picture viewer focused the wheel browses files',
          'wheel -1 menu=0' in text and 'image shift 2' in text,
          'image shift 2 x%d' % text.count('image shift 2'))
    check('the wheel really changed the displayed file',
          'image: loaded IMG.BMP' in text and 'image: loaded IMG.QOI' in text,
          'IMG.BMP x%d / IMG.QOI x%d'
          % (text.count('image: loaded IMG.BMP'), text.count('image: loaded IMG.QOI')))
    check('no kernel PANIC', 'PANIC' not in text)

    # the paged menu really shows the 7th row in the last visible slot
    img = load(SHOT)
    px = img.load()
    grey = accent_pixels(px, MENU_X + 16, MENU_ITEMS_Y + 5 * MENU_ROW_H,
                         MENU_X + 60, MENU_ITEMS_Y + 6 * MENU_ROW_H,
                         (0x8b, 0x94, 0xa6))
    check('the paged scroll was on the screen (7th row in the last slot)',
          grey > 60, 'grey px %d' % grey)

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
