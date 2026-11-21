#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS v0.64 acceptance: the start menu scrolls past six rows.

The registry grew to 7 entries (6 windows + "退出到终端") while the panel keeps the
324 px height every existing gate has calibrated coordinates against, so the
seventh row has to be *scrolled to*:

  * the panel top edge stays at MENU_Y=398 and the 7th row is not drawn
  * Down x6 walks the selection to row 6 and scrolls the list  ("menu sel 6",
    "menu scroll 1 1"); the 7th row (grey accent) is then painted in the last
    visible slot and the first slot now shows row 1 (teal)
  * Home / End jump to the first / last row and keep it in view
  * the scrollbar strip is clickable: upper half scrolls up, lower half down
  * Enter on the 7th row leaves the desktop ("menu: exit to terminal" ->
    "exit to terminal (start menu)") and the terminal prompt comes back
  * no kernel PANIC

Usage: py -3 scripts/gui_menu_scroll_acceptance.py
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
QMP_PORT = int(os.environ.get('SCROLL_QMP_PORT', '45895'))
SERIAL = 'build/serial_menu_scroll.log'
SHOT_TOP = 'build/acc_menu_scroll_top.ppm'
SHOT_SCROLLED = 'build/acc_menu_scroll_down.ppm'
DISK = 'build/fat32_ata.img'

TASKBAR_Y = 730
MENU_X, MENU_W = 12, 300
MENU_HEAD_H, MENU_ROW_H, MENU_FOOT_H = 46, 44, 14
N_ITEMS = 7
N_VISIBLE = 6
MENU_H = MENU_HEAD_H + N_VISIBLE * MENU_ROW_H + MENU_FOOT_H        # 324
MENU_Y = TASKBAR_Y - MENU_H - 8                                    # 398
MENU_ITEMS_Y = MENU_Y + MENU_HEAD_H                                # 444
START_BTN = (25, 745)
SCROLL_STRIP_X = 303
ACC_BLUE = (0x4f, 0x8d, 0xff)      # row 0 系统设置
ACC_TEAL = (0x22, 0xc1, 0xa8)      # row 1 任务管理器
ACC_GREY = (0x8b, 0x94, 0xa6)      # row 6 退出到终端


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


def press_keys(q, keys, delay=0.22):
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


def mean_brightness(px, x0, y0, x1, y1):
    n = 0
    acc = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            acc += (r * 299 + g * 587 + b * 114) // 1000
            n += 1
    return acc / float(n) if n else 0.0


def accent_pixels(px, x0, y0, x1, y1, want, tol=64):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            if abs(r - want[0]) <= tol and abs(g - want[1]) <= tol and abs(b - want[2]) <= tol:
                n += 1
    return n


def boot():
    for p in (SERIAL, SHOT_TOP, SHOT_SCROLLED):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))
    log = open(os.path.join(ROOT, 'build/qemu_menu_scroll.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


def row_band(slot):
    y = MENU_ITEMS_Y + slot * MENU_ROW_H
    return (MENU_X + 16, y + 6, MENU_X + 60, y + 34)


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
        click(q, pos, *START_BTN)                 # open the menu
        shot(q, SHOT_TOP)
        # walk to the 7th row: selection has to pull the list along
        press_keys(q, ['down'] * 6)
        time.sleep(1.0)
        shot(q, SHOT_SCROLLED)
        # Home / End
        press_keys(q, ['home'])
        time.sleep(0.8)
        press_keys(q, ['end'])
        time.sleep(0.8)
        # scrollbar strip: upper half = up, lower half = down
        click(q, pos, SCROLL_STRIP_X, MENU_ITEMS_Y + 40, settle=1.2)
        click(q, pos, SCROLL_STRIP_X, MENU_ITEMS_Y + 200, settle=1.2)
        # Enter on the last row = exit to terminal
        press_keys(q, ['ret'])
        time.sleep(3.0)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')

    top = load(SHOT_TOP)
    pxt = top.load()
    above = mean_brightness(pxt, MENU_X + 40, MENU_Y - 22, MENU_X + 240, MENU_Y - 4)
    inside = mean_brightness(pxt, MENU_X + 40, MENU_Y + 70, MENU_X + 240, MENU_Y + 130)
    check('the panel is 6 rows tall (top edge at %d)' % MENU_Y,
          abs(above - inside) > 8.0, 'above %.1f vs inside %.1f' % (above, inside))
    grey_top = accent_pixels(pxt, MENU_X + 16, MENU_ITEMS_Y + 6 * MENU_ROW_H,
                             MENU_X + 60, MENU_Y + MENU_H - 2, ACC_GREY)
    check('the 7th row is not painted before scrolling', grey_top < 40,
          'grey px %d' % grey_top)
    blue_top = accent_pixels(pxt, *row_band(0), ACC_BLUE)
    check('row 0 (blue) starts in the first slot', blue_top > 60, 'blue px %d' % blue_top)

    down = load(SHOT_SCROLLED)
    pxd = down.load()
    grey = accent_pixels(pxd, *row_band(5), ACC_GREY)
    check('after 6x Down the 7th row fills the last slot', grey > 60,
          'grey px %d' % grey)
    teal = accent_pixels(pxd, *row_band(0), ACC_TEAL)
    blue_now = accent_pixels(pxd, *row_band(0), ACC_BLUE)
    check('...and the list really scrolled (slot 0 is row 1, not row 0)',
          teal > 60 and blue_now < teal, 'teal %d blue %d' % (teal, blue_now))

    check('Down x6 selected the 7th row', 'menu sel 6' in text)
    check('the list scrolled once the selection left the panel',
          text.count('menu scroll 1 1') >= 3,
          'menu scroll 1 1 x%d' % text.count('menu scroll 1 1'))
    check('Home jumps to the first row and scrolls back',
          'menu sel 0' in text and text.count('menu scroll 0 1') >= 2,
          'menu scroll 0 1 x%d' % text.count('menu scroll 0 1'))
    check('End jumps to the last row', text.count('menu sel 6') >= 2)
    check('the exit row asks the runner to leave the desktop',
          'menu: exit to terminal' in text)
    check('the C runner honoured it (one frame later)',
          'exit to terminal (start menu)' in text)
    check('the terminal prompt is back after the desktop exits',
          text.count('sutureos#') >= 2, 'sutureos# x%d' % text.count('sutureos#'))
    check('no kernel PANIC', 'PANIC' not in text)

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
