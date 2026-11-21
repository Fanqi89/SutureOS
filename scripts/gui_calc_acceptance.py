#!/usr/bin/env python3
"""SutureOS calculator acceptance (v0.63): a real app with real arithmetic.

The desktop's sixth application is a calculator (window 5, start-menu row 6). This
gate proves it end to end:

  * open it from the start menu (`menu launch 5`),
  * type `12+30=` with the real PS/2 keyboard and require `calc result 42`,
  * `9*9=` -> 81, `7-10=` -> -3 (negative results are part of the state machine),
  * `8/0=` -> `calc error (division by zero)`,
  * `c` -> `calc clear`,
  * the display strip and the 4x4 keypad plates are on screen (pixels, not logs),
  * no kernel PANIC.

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
QMP_PORT = int(os.environ.get('CALC_QMP_PORT', '45911'))
SERIAL = 'build/serial_calc.log'
SHOT = 'build/acc_calc.ppm'
SHOT_42 = 'build/acc_calc_42.ppm'      # the state right after 12+30=
DISK = 'build/fat32_ata.img'

TASKBAR_Y = 730
MENU_X = 12
MENU_ITEMS_Y = 444            # 6-row menu: TASKBAR_Y - 324 - 8 + 46
MENU_ROW_H = 44
CALC_ROW = 5
START_BTN = (25, 745)

# window 5 geometry mirrored from shell.rs::win5 + apps.rs layout constants
WIN5 = (600, 250, 300, 260)
PAD, TITLE_H = 16, 30

# QEMU's QKeyCode has physical keys only: shifted symbols are pressed as
# "shift held + base key" (this also exercises the kernel's shift map).
QCODE = {
    ' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash', ',': 'comma',
    '=': 'equal', '\n': 'ret', '+': ['shift', 'equal'], '*': ['shift', '8'],
}


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


def press(q, keys, delay=0.16):
    for k in keys:
        seq = QCODE.get(k, k)
        if isinstance(seq, str):
            seq = [seq]
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': d} for d in seq])
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


def boot():
    for p in (SERIAL, SHOT, SHOT_42):
        full = os.path.join(ROOT, p)
        if os.path.exists(full):
            os.remove(full)
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


def count_where(px, x0, y0, x1, y1, pred):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(px[x, y]):
                n += 1
    return n


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
        press(q, list('startgui') + ['\n'])
        time.sleep(16)
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, MENU_ITEMS_Y + CALC_ROW * MENU_ROW_H + 19)
        time.sleep(1.5)
        press(q, list('12+30='))
        time.sleep(1.0)
        q.cmd('screendump', filename=SHOT_42)   # display shows 42 here
        time.sleep(1.0)
        press(q, ['c'])
        press(q, list('9*9='))
        time.sleep(1.0)
        press(q, ['c'])
        press(q, list('7-10='))
        time.sleep(1.0)
        press(q, ['c'])
        press(q, list('8/0='))
        time.sleep(1.5)
        q.cmd('screendump', filename=SHOT)
        time.sleep(1.0)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    check('the start menu launched the calculator', 'menu launch 5' in text,
          'menu launch 5')
    check('12+30= computes 42', 'calc result 42' in text, 'calc result 42')
    check('9*9= computes 81', 'calc result 81' in text, 'calc result 81')
    check('7-10= computes -3 (negative result)', 'calc result -3' in text,
          'calc result -3')
    check('8/0= reports a division-by-zero error',
          'calc error (division by zero)' in text, 'calc error')
    check('c clears the calculator', text.count('calc clear') >= 3,
          '%d clear line(s)' % text.count('calc clear'))
    check('every key reached the calculator window',
          text.count('[gui-shell] calc key') >= len('12+30=') + len('9*9=')
          + len('7-10=') + len('8/0='),
          '%d key line(s)' % text.count('[gui-shell] calc key'))

    from PIL import Image
    img = Image.open(os.path.join(ROOT, SHOT)).convert('RGB')
    px = img.load()
    x0 = WIN5[0] + PAD
    dy0 = WIN5[1] + TITLE_H + 8
    dy1 = dy0 + 44
    # the display strip is very dark; its text is warm white (0xffe6c8)
    strip = count_where(px, x0, dy0, x0 + 260, dy1,
                        lambda c: c[0] > 200 and c[1] > 170 and c[2] > 120)
    check('the display strip draws the current value', strip > 60,
          '%d text px in the display strip' % strip)
    # keypad plates (0x2a3140) below the display
    plate = count_where(px, x0, dy1 + 14, x0 + 268, dy1 + 170,
                        lambda c: abs(c[0] - 0x2a) < 8 and abs(c[1] - 0x31) < 8
                        and abs(c[2] - 0x40) < 8)
    check('the 4x4 keypad plates are painted', plate > 3000,
          '%d plate px' % plate)
    check('no kernel PANIC', 'PANIC' not in text.upper().replace('NO PANIC', ''))

    ok = sum(1 for c in checks if c[1])
    print('[calc] checks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok),
          flush=True)
    if ok == len(checks):
        print('[calc] RESULT: PASS', flush=True)
        return 0
    print('[calc] RESULT: FAIL', flush=True)
    return 1


if __name__ == '__main__':
    sys.exit(main())
