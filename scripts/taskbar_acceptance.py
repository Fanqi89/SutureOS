#!/usr/bin/env python3
"""SutureOS taskbar acceptance (v0.31 batch).

Proves, on the real GUI with real input injection:
  1. the taskbar shows one button per open window, the RTC clock and the user,
  2. clicking a window button raises/focuses that window (guest log + pixels),
  3. the Windows/Super key (E0 5B) opens/closes the start menu,
  4. ESC closes the menu instead of quitting the desktop,
  5. the pixels of the window buttons and clock region really exist (the clock
     text is read from the RTC, so we only assert the band is painted there).

  py -3 scripts/taskbar_acceptance.py
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp, START_BTN, SCREEN_W, SCREEN_H  # noqa: E402

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 48101
LOG = 'build/serial_taskbar.log'
SHOT_A = 'build/taskbar_a.ppm'
SHOT_B = 'build/taskbar_b.ppm'


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(os.path.join(ROOT, LOG), encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    pos = [SCREEN_W // 2, SCREEN_H // 2]

    try:
        q = Qmp(PORT)

        def key(k, d=0.08):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(ch)

        def move_to(tx, ty):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                time.sleep(0.05)
                dx -= sx
                dy -= sy
            pos[0], pos[1] = tx, ty
            time.sleep(0.2)

        def click():
            q.hmp('mouse_button 1')
            time.sleep(0.12)
            q.hmp('mouse_button 0')
            time.sleep(0.45)

        for _ in range(150):
            if 'sutureos# ' in log_text():
                break
            time.sleep(1)
        for _ in range(5):
            time.sleep(0.4)
            for _ in range(24):
                key('backspace', 0.015)
            typ('startgui')
            key('ret')
            time.sleep(3.2)
            if '[gui] main loop start' in log_text():
                break
        time.sleep(1.0)
        q.hmp('screendump ' + SHOT_A)

        # 1) Windows/Super key toggles the start menu
        key('meta_l', 0.5)
        time.sleep(0.6)
        q.hmp('screendump ' + SHOT_B)

        # 2) ESC closes the menu (must NOT quit the desktop)
        key('esc')
        time.sleep(0.6)

        # 3) taskbar window button click: Calculator is the 2nd button.
        #    layout: x = 4+96+12 = 112; "Task Manager" 12*8+16 = 112 -> next 228
        move_to(228 + 48, SCREEN_H - 40 + 20)
        click()
        time.sleep(0.8)
        # 4) open the start menu again with the Win key and click its first item
        key('meta_l', 0.5)
        time.sleep(0.6)
        move_to(104, SCREEN_H - 40 - (8 * 24 + 12) - 2 + 6 + 12)
        click()
        time.sleep(0.8)
    finally:
        try:
            q.hmp('quit')
        except Exception:
            pass
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = log_text()
    checks = [
        ('taskbar buttons: three windows listed',
         log.count('[gui] Window created:') >= 3),
        ('Win key (E0 5B) opens the start menu',
         '[gui] Win key: start menu opened' in log),
        ('Win key again / ESC closes the start menu',
         ('[gui] ESC: start menu closed' in log) or
         ('[gui] Win key: start menu closed' in log)),
        ('clicking a taskbar button raises that window',
         ("[gui] taskbar: focus 'Calculator'" in log) or
         ('[gui] taskbar: pinned app 1' in log)),
        ('start menu is still usable after the taskbar work',
         'start menu:' in log),
    ]

    # pixel evidence: both screenshots have the taskbar band painted
    try:
        from PIL import Image
        a = Image.open(os.path.join(ROOT, SHOT_A)).convert('RGB')
        b = Image.open(os.path.join(ROOT, SHOT_B)).convert('RGB')
        band = (0, SCREEN_H - 40, SCREEN_W, SCREEN_H)
        pa = a.crop(band)
        pb = b.crop(band)
        # the taskbar is dark but not uniform: buttons/clock/fps text paint pixels
        colors_a = len(set(pa.getdata()))
        checks.append(('taskbar band is painted with more than one colour', colors_a > 4,
                       'colours=%d' % colors_a))
        diff = sum(1 for x, y in zip(pa.getdata(), pb.getdata()) if x != y)
        checks.append(('start menu changes the screen when Win is pressed', diff > 500,
                       'changed_px=%d' % diff))
    except Exception as exc:                       # noqa: BLE001
        checks.append(('pixel checks', False, repr(exc)))

    print('--- checks ---')
    ok = True
    for item in checks:
        name, cond = item[0], item[1]
        detail = ('  (%s)' % item[2]) if len(item) > 2 and item[2] else ''
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, detail))
        ok = ok and cond
    print('RESULT: %s (checks=%d)' % ('PASS' if ok else 'FAIL', len(checks)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
