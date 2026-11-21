#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS calculator acceptance (v0.64): decimals, percent, memory, mouse.

The desktop's sixth application is a calculator (window 5, start-menu row 6). This
gate proves it end to end with real PS/2 keyboard and real mouse clicks:

  * open it from the start menu ("menu launch 5"),
  * "[gui-shell] calc layout rows=6 cols=5 slots=30 grid=.. cell=.." is logged,
  * keyboard / fixed point (4 decimals):
        12.5+30.25=  -> calc value 427500
        9*9=         -> calc value 810000
        7-10=        -> calc value -30000        (negative path)
        1/4=         -> calc value 2500
        8/0=         -> calc error (division by zero)
        50+10%=      -> calc value 550000        (percent of the accumulator)
        200*10%=     -> calc value 200000        (percent of the operand)
  * mouse: clicking 7 + 8 = on the pad emits "calc tap" lines and computes 15,
  * memory: M+ / AC / MR / MC with "calc mem plus/recall/clear" lines,
  * the display strip and the keypad really are painted (pixels, not just logs),
  * the calculator owns the keyboard (no "input win=5" band),
  * no kernel PANIC.

Exit code 0 only when every check passes.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = int(os.environ.get('CALC_QMP_PORT', '45911'))
SERIAL = 'build/serial_calc.log'
SHOT = 'build/acc_calc.ppm'
DISK = 'build/fat32_ata.img'

TASKBAR_Y = 730
MENU_X = 12
MENU_HEAD_H, MENU_ROW_H, MENU_FOOT_H = 46, 44, 14
N_VISIBLE = 6
MENU_H = MENU_HEAD_H + N_VISIBLE * MENU_ROW_H + MENU_FOOT_H        # 324
MENU_Y = TASKBAR_Y - MENU_H - 8                                    # 398
MENU_ITEMS_Y = MENU_Y + MENU_HEAD_H                                # 444
CALC_ROW = 5                                                       # 计算器
START_BTN = (25, 745)

CALC_X, CALC_Y = 600, 210
CAL_ROWS, CAL_COLS = 6, 5
# window-relative pad geometry (apps.rs: PAD=16, TITLE_H=30, display 44, gap 10)
CAL_GRID = (16, 92, 57, 37)
CAL_DISP_Y = 38                                                    # TITLE_H + 8
CAL_DISP_H = 44

# CAL_KEYS in apps.rs, row-major: slot = row * 5 + col
LABELS = [['MC', 'MR', 'M+', 'M-', 'C'],
          ['7', '8', '9', '/', 'DEL'],
          ['4', '5', '6', '*', '±'],
          ['1', '2', '3', '-', '%'],
          ['0', '.', '=', '+', '00'],
          ['CE', 'AC', '√', 'x2', '1/x']]

LAYOUT_RE = re.compile(
    r'\[gui-shell\] calc layout rows=(\d+) cols=(\d+) slots=(\d+) '
    r'grid=(-?\d+),(-?\d+) cell=(\d+),(\d+)')


def slot_of(label):
    for r in range(CAL_ROWS):
        for c in range(CAL_COLS):
            if LABELS[r][c] == label:
                return r * CAL_COLS + c
    raise KeyError(label)


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
SHIFTED = {'+': ['shift', 'equal'], '*': ['shift', '8'], '%': ['shift', '5']}


def press(q, keys, delay=0.16):
    """Press a sequence of characters; shifted symbols become a chord."""
    for k in keys:
        data = SHIFTED.get(k, [QCODE.get(k, k)])
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': d} for d in data])
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


def click(q, pos, tx, ty, settle=1.2):
    move_to(q, pos, tx, ty)
    q.hmp('mouse_button 1')
    time.sleep(0.18)
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


def slot_screen(label):
    slot = slot_of(label)
    gx, gy, cw, ch = CAL_GRID
    return (CALC_X + gx + (slot % CAL_COLS) * cw + cw // 2,
            CALC_Y + gy + (slot // CAL_COLS) * ch + ch // 2)


def boot():
    for p in (SERIAL, SHOT):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))
    log = open(os.path.join(ROOT, 'build/qemu_calc.log'), 'wb')
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
        press(q, list('startgui') + ['ret'])
        time.sleep(16)
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, MENU_ITEMS_Y + CALC_ROW * MENU_ROW_H + 19, settle=2.2)

        press(q, list('12.5+30.25='))
        time.sleep(0.8)
        press(q, list('9*9='))
        time.sleep(0.8)
        press(q, list('7-10='))
        time.sleep(0.8)
        press(q, list('1/4='))
        time.sleep(0.8)
        press(q, list('8/0='))
        time.sleep(0.8)
        press(q, list('c'))
        time.sleep(0.8)
        press(q, list('50+10%='))
        time.sleep(0.8)
        press(q, list('c'))
        time.sleep(0.8)
        press(q, list('200*10%='))
        time.sleep(0.8)
        press(q, list('c'))
        time.sleep(0.8)

        # ---------------------------------------------------- mouse keypad path
        for label in ('7', '+', '8', '='):
            click(q, pos, *slot_screen(label), settle=1.0)
        time.sleep(1.0)
        shot(q, os.path.join(ROOT, SHOT))
        # ------------------------------------------------------------- memory
        click(q, pos, *slot_screen('M+'), settle=1.0)
        click(q, pos, *slot_screen('C'), settle=1.0)
        click(q, pos, *slot_screen('MR'), settle=1.0)
        click(q, pos, *slot_screen('MC'), settle=1.0)
        click(q, pos, *slot_screen('MR'), settle=1.0)
        time.sleep(1.0)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')

    m = LAYOUT_RE.search(text)
    check('the calculator logged its keypad layout', m is not None,
          m.group(0) if m else 'no calc layout line')
    if m:
        rows, cols, slots = int(m.group(1)), int(m.group(2)), int(m.group(3))
        grid = (int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)))
        check('the pad is the 6x5 / 30-slot one this gate clicks',
              (rows, cols, slots) == (CAL_ROWS, CAL_COLS, CAL_ROWS * CAL_COLS),
              'rows=%d cols=%d slots=%d' % (rows, cols, slots))
        check('the pad geometry matches the coordinates used for the clicks',
              grid == CAL_GRID, 'grid=%d,%d cell=%d,%d' % grid)

    check('the menu row opened the calculator window', 'menu launch 5' in text)
    check('decimal arithmetic keeps 4 fixed-point digits (12.5+30.25)',
          'calc value 427500' in text, 'calc value 427500')
    check('multiplication (9*9)', 'calc value 810000' in text)
    check('a negative result prints its sign (7-10)', 'calc value -30000' in text)
    check('division keeps the fraction (1/4)', 'calc value 2500' in text)
    check('division by zero is an error state',
          'calc error (division by zero)' in text)
    check('percent of the accumulator (50+10%)', 'calc value 550000' in text)
    check('percent of the operand (200*10%)', 'calc value 200000' in text)
    check('clear resets the display', text.count('calc clear') >= 3,
          'calc clear x%d' % text.count('calc clear'))
    check('mouse taps reach the keypad handlers',
          text.count('calc tap') >= 8, 'calc tap x%d' % text.count('calc tap'))
    check('mouse taps computed 7 + 8 = 15', 'calc value 150000' in text)
    check('M+ stored the value', 'calc mem plus 150000' in text,
          'calc mem plus 150000')
    check('C keeps the memory (only AC clears it)',
          'calc mem recall 150000' in text, 'calc mem recall 150000')
    check('MC cleared the memory',
          'calc mem clear' in text and 'calc mem recall 0' in text)
    check('the calculator owns the keyboard (no input band)', 'input win=5' not in text)
    check('no kernel PANIC', 'PANIC' not in text)

    from PIL import Image
    img = Image.open(os.path.join(ROOT, SHOT)).convert('RGB')
    px = img.load()
    bright = 0
    for y in range(CALC_Y + CAL_DISP_Y, CALC_Y + CAL_DISP_Y + CAL_DISP_H):
        for x in range(CALC_X + 20, CALC_X + 300):
            r, g, b = px[x, y]
            if (r * 299 + g * 587 + b * 114) // 1000 > 150:
                bright += 1
    check('the display strip shows the computed digits (bright pixels)', bright > 60,
          'bright px %d' % bright)
    colors = set()
    for y in range(CALC_Y + 100, CALC_Y + 320, 3):
        for x in range(CALC_X + 20, CALC_X + 300, 3):
            colors.add(px[x, y])
    check('the keypad plates are painted (many distinct colours)',
          len(colors) > 24, '%d colours' % len(colors))

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
