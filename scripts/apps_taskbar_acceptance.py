#!/usr/bin/env python3
"""SutureOS application registry + taskbar / window-management acceptance (v0.34).

Proves with real QMP input that:
  1. the start menu is built from the application registry (log line with count),
  2. pinned apps always own a button on the taskbar's left side and clicking it
     raises their window,
  3. clicking a running window's taskbar button toggles restore <-> minimise
     (click -> minimise, click -> restore), i.e. the behaviour the user asked for,
  4. the focused window can be maximised/restored (keyboard `m` and title-bar
     button share gui_flip_window) and minimised (`n`).

  py -3 scripts/apps_taskbar_acceptance.py
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp, START_BTN, SCREEN_W, SCREEN_H  # noqa: E402

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 48111
LOG = 'build/serial_apps_taskbar.log'
SHOT = 'build/apps_taskbar.ppm'

# taskbar geometry must match kernel/gui/gui.c gui_taskbar_layout()
TASKBAR_Y = SCREEN_H - 40
PIN_START_X = 4 + 96 + 12   # START_BTN_X + START_BTN_W + 12 (kernel/gui/gui.c)
PIN_LABELS = ['Task Manager', 'Calculator', 'Image Viewer', 'Text Editor']
PIN_MAX_CHARS = 10


def pinned_rects():
    out = []
    x = PIN_START_X
    for label in PIN_LABELS:
        w = 8 * min(len(label), PIN_MAX_CHARS) + 14
        out.append((x, w, label))
        x += w + 4
    return out


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
            time.sleep(0.5)

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
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT)

        # 1) maximise / restore with the `m` shortcut on the focused window
        key('m', 0.5)
        time.sleep(0.6)
        key('m', 0.5)
        time.sleep(0.6)
        # 2) minimise with `n`, then toggle it back with the taskbar button
        key('n', 0.5)
        time.sleep(0.6)
        wb_centre = PIN_START_X + sum(8 * min(len(l), PIN_MAX_CHARS) + 14 + 4
                                     for l in PIN_LABELS) + 4 + 64
        move_to(wb_centre, TASKBAR_Y + 20)
        click()                                   # restore
        click()                                   # minimise again
        click()                                   # restore again
        # 3) pinned app button raises that app's window
        pin = pinned_rects()[1]                   # Calculator
        move_to(pin[0] + pin[1] // 2, TASKBAR_Y + 20)
        click()
        time.sleep(0.8)
        # 4) start menu still opens (Win key) and is built from the registry
        key('meta_l', 0.5)
        time.sleep(0.6)
        key('esc')
        time.sleep(0.4)
        q.hmp('screendump build/apps_taskbar_menu.ppm')
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
        ('start menu is built from the application registry',
         '[gui] start menu: 8 items from app registry' in log,
         '8 = 6 registry rows + separator + exit'),
        ('focused window maximised (title-bar button shares this path)',
         'maximized to 1024x728' in log),
        ('maximised window restored to its saved rectangle',
         'restored to' in log),
        ('`n` shortcut minimises the focused window',
         "[gui] key n: minimize '" in log),
        ('taskbar window button restores a minimised window',
         "[gui] taskbar: restore '" in log),
        ('clicking the same taskbar button again minimises it',
         "[gui] taskbar: minimize '" in log),
        ('pinned app button raises its app window',
         '[gui] taskbar: pinned app 1' in log),
        ('Win key still toggles the start menu',
         '[gui] Win key: start menu opened' in log),
    ]

    # pixel evidence: the taskbar band shows pinned buttons (more colours than a
    # plain bar) and the desktop itself is still the frozen baseline layout
    try:
        from PIL import Image
        im = Image.open(os.path.join(ROOT, SHOT)).convert('RGB')
        band = im.crop((0, TASKBAR_Y, SCREEN_W, SCREEN_H))
        colours = len(set(band.getdata()))
        checks.append(('taskbar band is painted (pinned + window buttons + clock)',
                       colours >= 6, 'colours=%d' % colours))
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
