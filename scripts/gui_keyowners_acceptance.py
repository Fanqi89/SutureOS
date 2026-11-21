#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS v0.64 acceptance: per-window keyboard ownership registry.

Proves, with real QEMU keyboard input, that the shell now routes keys through the
KEY_OWNERS table instead of an if-chain plus a duplicated skip list:

  * boot log: "[gui-shell] key owners=4 mask=60"   (slots 2,3,4,5)
  * a plain window (task manager, slot 1) still owns a one-line input band:
        "input win=1 text=abc" and "input commit 1 3"
  * the editor (slot 3) consumes characters itself      -> "editor key"
  * the picture viewer (slot 2) consumes Left/Right     -> "image shift"
  * the calculator (slot 5) consumes digits             -> "calc key"
  * the Wayland view (slot 4) hosts no band and no handler: typing there
    produces no "input win=4" line at all
  * no window that owns the keyboard ever gets the input band
  * no kernel PANIC

Usage: py -3 scripts/gui_keyowners_acceptance.py
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
QMP_PORT = int(os.environ.get('KEYS_QMP_PORT', '45893'))
SERIAL = 'build/serial_keyowners.log'
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


def press_keys(q, keys, delay=0.18):
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


def boot():
    if os.path.exists(os.path.join(ROOT, SERIAL)):
        os.remove(os.path.join(ROOT, SERIAL))
    log = open(os.path.join(ROOT, 'build/qemu_keyowners.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


def row_y(idx):
    return MENU_ITEMS_Y + (idx % N_VISIBLE) * MENU_ROW_H + 19


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
        # 1. a plain window keeps the input band (focus starts on the task manager)
        press_keys(q, list('abc') + ['ret'])
        time.sleep(1.5)
        # 2. editor
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, row_y(3))          # 文本编辑器
        press_keys(q, list('hi'))
        time.sleep(1.2)
        # 3. picture viewer: Left/Right browse
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, row_y(2))          # 图片查看器
        press_keys(q, ['right'])
        time.sleep(1.2)
        # 4. calculator
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, row_y(5))          # 计算器
        press_keys(q, ['7'])
        time.sleep(1.2)
        # 5. Wayland view: no band, no handler
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, row_y(4))          # Wayland 客户端
        press_keys(q, ['x'])
        time.sleep(1.2)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')

    check('the shell started the Rust desktop', '[gui-shell]' in text)
    check('the registry is the table the shell walks (4 owners, mask 60)',
          '[gui-shell] key owners=4 mask=60' in text,
          'key owners=4 mask=60')
    check('a plain window still gets the one-line input band',
          'input win=1 text=abc' in text, 'input win=1 text=abc')
    check('...and commits it on Enter', 'input commit 1 3' in text,
          'input commit 1 3')
    check('the editor consumes characters itself', 'editor key' in text)
    check('the editor never gets the input band', 'input win=3' not in text)
    check('the picture viewer consumes Left/Right', 'image shift 1' in text,
          'image shift 1')
    check('the picture viewer never gets the input band', 'input win=2' not in text)
    check('the calculator consumes digits', 'calc key' in text)
    check('the calculator never gets the input band', 'input win=5' not in text)
    check('the Wayland view launches', 'menu launch 4' in text)
    check('the Wayland view hosts no input band either', 'input win=4' not in text)
    check('no kernel PANIC', 'PANIC' not in text)

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
