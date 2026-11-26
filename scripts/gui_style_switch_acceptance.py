#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS desktop style switch acceptance (v0.71).

The desktop ships two looks and the switch lives in 系统设置 -> 外观风格:

  0. glass / Fluent (default, what the appearance baseline pins)
  1. flat (one-colour wallpaper, opaque panels, hairlines, no shadow/blur)

This gate proves the switch end to end with real clicks and pixels:

  1. default boot is glass: the wallpaper has a vertical gradient, the setting
     row says so, and the shell logs the default style;
  2. clicking the row switches to flat: `[gui-shell] style: flat` + a saved
     config line `style=1`, the wallpaper becomes one flat colour, the title strip
     is the flat title colour, and window bodies are opaque;
  3. clicking again goes back to glass (gradient returns);
  4. switching to flat and rebooting on the same volume reloads it
     (`shell cfg loaded: pinned=4 style=1`) and the desktop comes up flat.

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
PORT = int(os.environ.get('STYLE_QMP_PORT', '48225'))
SERIAL = 'build/serial_style_switch.log'
SERIAL2 = 'build/serial_style_switch2.log'
DISK = 'build/fat32_ata.img'
SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H
# 系统设置 (slot 0) and the 外观风格 row geometry (mirrors apps.rs)
WIN0 = (150, 120, 430, 300)
ROW_TOP = 30 + 20
ROW_H = 52
ROW_PITCH = 62
STYLE_ROW_Y = WIN0[1] + ROW_TOP + ROW_PITCH + ROW_H // 2
STYLE_ROW_X = WIN0[0] + WIN0[2] // 2


class Qmp:
    def __init__(self, port, timeout=120):
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), 1.0)
                self.f = self.s.makefile('rwb', buffering=0)
                self.f.readline()
                self.cmd('qmp_capabilities')
                self.cur = [512, 648]          # PS/2 pointer start
                return
            except Exception as e:             # noqa: BLE001
                last = e
                time.sleep(0.3)
        raise RuntimeError('QMP connect failed: %r' % (last,))

    def cmd(self, name, **args):
        self.f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            rep = json.loads(line)
            if 'event' not in rep:
                if 'error' in rep:
                    raise RuntimeError('QMP error: %r' % (rep['error'],))
                return rep

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})

    def shot(self, name, settle=0.5):
        p = os.path.join(ROOT, 'build', name)
        if os.path.exists(p):
            os.remove(p)
        self.cmd('screendump', filename=p)
        for _ in range(120):
            if os.path.exists(p) and os.path.getsize(p) > 1000:
                time.sleep(settle)
                return p
            time.sleep(0.05)
        raise RuntimeError('no screendump ' + name)

    def stable_shot(self, name, tries=6):
        """Dump until two consecutive frames are identical.

        A style switch rebuilds the wallpaper caches and forces a full frame; on
        TCG that frame can take a couple of seconds, so a fixed sleep sometimes
        captured the previous (or a half-updated) frame - which looked like the
        old text bleeding through the new panel."""
        prev = None
        p = None
        for _ in range(tries):
            p = self.shot(name, settle=0.6)
            data = open(p, 'rb').read()
            if prev is not None and data == prev:
                return p
            prev = data
            time.sleep(1.2)
        return p

    def key(self, qcode, settle=0.9):
        self.cmd('send-key', keys=[{'type': 'qcode', 'data': qcode}])
        time.sleep(settle)

    def keys(self, text, delay=0.06):
        for ch in text:
            self.cmd('send-key', keys=[{'type': 'qcode', 'data': ch}])
            time.sleep(delay)
        self.key('ret')

    def move(self, tx, ty):
        dx, dy = tx - self.cur[0], ty - self.cur[1]
        while dx or dy:
            sx = max(-90, min(90, dx))
            sy = max(-90, min(90, dy))
            self.hmp('mouse_move %d %d' % (sx, sy))
            dx -= sx
            dy -= sy
            time.sleep(0.05)
        self.cur = [tx, ty]
        time.sleep(0.3)

    def click(self, tx, ty, settle=1.8):
        self.move(tx, ty)
        self.hmp('mouse_button 1')
        time.sleep(0.2)
        self.hmp('mouse_button 0')
        time.sleep(settle)


def boot(serial, port, actions):
    if os.path.exists(os.path.join(ROOT, serial)):
        os.remove(os.path.join(ROOT, serial))
    log = open(os.path.join(ROOT, 'build', 'qemu_' + os.path.basename(serial) + '.out'), 'wb')
    p = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-serial', 'file:' + serial,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % port,
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    try:
        q = Qmp(port)
        actions(q)
        q.cmd('quit')
    finally:
        try:
            p.wait(timeout=30)
        except Exception:                    # noqa: BLE001
            p.kill()
        log.close()
    return open(os.path.join(ROOT, serial), 'rb').read().decode('utf-8', 'replace')


def main():
    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''), flush=True)

    subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')],
                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shutil.copyfile(os.path.join(ROOT, 'build', 'fat32.img'), os.path.join(ROOT, DISK))
    from PIL import Image

    shots = {}

    def actions1(q):
        time.sleep(50)
        q.keys('startgui')
        time.sleep(16)
        shots['glass'] = Image.open(q.stable_shot('style_glass.ppm')).convert('RGB')
        q.click(STYLE_ROW_X, STYLE_ROW_Y)          # -> flat
        shots['flat'] = Image.open(q.stable_shot('style_flat.ppm')).convert('RGB')
        q.click(STYLE_ROW_X, STYLE_ROW_Y)          # -> glass again
        shots['glass2'] = Image.open(q.stable_shot('style_glass2.ppm')).convert('RGB')
        q.click(STYLE_ROW_X, STYLE_ROW_Y)          # -> flat, persisted
        time.sleep(0.8)

    text = boot(SERIAL, PORT, actions1)

    def grad(img):
        px = img.load()
        a = px[60, 80]
        b = px[60, SCREEN_H - 120]
        return sum(abs(a[i] - b[i]) for i in range(3))

    glass, flat, glass2 = shots.get('glass'), shots.get('flat'), shots.get('glass2')
    check('the default boot is the glass style',
          '[gui-shell] shell cfg defaults' in text or 'style=0' in text,
          'cfg default log')
    check('the glass wallpaper has a vertical gradient', glass is not None and grad(glass) > 12,
          'channel sum %s' % (grad(glass) if glass else None))
    check('clicking 外观风格 switches to flat',
          '[gui-shell] style: flat' in text, 'style: flat')
    check('the switch is persisted as style=1',
          'shell cfg saved: pinned=4 style=1' in text, 'cfg saved log')
    if flat is not None:
        check('the flat wallpaper is one opaque colour', grad(flat) <= 3,
              'channel sum %d' % grad(flat))
        px = flat.load()
        wall = px[60, 80]
        check('the flat desktop backdrop is the flat wall colour',
              abs(wall[0] - 0x0e) <= 6 and abs(wall[1] - 0x17) <= 6 and abs(wall[2] - 0x26) <= 8,
              'wallpaper %s' % (wall,))
        title = px[WIN0[0] + WIN0[2] - 60, WIN0[1] + 14]
        check('flat window titles use the opaque flat title colour',
              abs(title[0] - 0x1f) <= 8 and abs(title[1] - 0x27) <= 8 and abs(title[2] - 0x34) <= 10,
              'title strip %s' % (title,))
        # the flat window must still draw its application content: count distinct
        # colours inside the settings body (rows/text) - an empty body would be a
        # single flat panel colour
        bright = sum(1 for yy in range(WIN0[1] + ROW_TOP, WIN0[1] + WIN0[3] - 10)
                     for xx in range(WIN0[0] + 20, WIN0[0] + WIN0[2] - 20)
                     if sum(px[xx, yy]) > 300)
        check('the flat window still paints its application content',
              bright > 300, '%d bright text pixels in the settings body' % bright)
        bar = px[600, TASKBAR_Y + 18]
        check('the flat taskbar is the opaque flat bar colour',
              abs(bar[0] - 0x0b) <= 6 and abs(bar[1] - 0x10) <= 8 and abs(bar[2] - 0x18) <= 10,
              'taskbar %s' % (bar,))
    check('clicking again returns to the glass style',
          '[gui-shell] style: glass (fluent)' in text, 'style: glass')
    check('the glass style really comes back', glass2 is not None and grad(glass2) > 12,
          'channel sum %s' % (grad(glass2) if glass2 else None))

    # ---- persistence: the last click left flat, reboot and check -------------
    def actions2(q):
        time.sleep(50)
        q.keys('startgui')
        time.sleep(16)
        shots['reboot'] = Image.open(q.stable_shot('style_reboot.ppm')).convert('RGB')

    text2 = boot(SERIAL2, PORT + 1, actions2)
    check('a reboot reloads the saved flat style',
          'shell cfg loaded: pinned=4 style=1' in text2, 'cfg loaded log')
    rb = shots.get('reboot')
    check('the desktop comes up flat after the reboot', rb is not None and grad(rb) <= 3,
          'channel sum %s' % (grad(rb) if rb else None))
    check('no kernel PANIC', 'PANIC' not in text and 'PANIC' not in text2)

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
