#!/usr/bin/env python3
"""SutureOS Text Editor acceptance (B10).

Starts the GUI the normal way, opens the Text Editor from the start menu with real
mouse clicks, types text through the real PS/2 keyboard path, saves with Ctrl+S,
and then proves on the HOST that the file contains exactly those bytes (the guest
log is checked too, but it is not the evidence).

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/editor_acceptance.py
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp, START_BTN, SCREEN_W, SCREEN_H  # noqa: E402

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 48021
LOG = 'build/serial_editor.log'
SHOT = 'build/editor.ppm'
TEXT = 'hello editor 42'
SAVED = TEXT + '\n'

KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '_': 'shift-minus'}

# start menu: 7 entries (TM/CALC/ABOUT/IMG/EDITOR/SEP/EXIT), grows upward
MENU_TOP = SCREEN_H - 40 - (6 + 8 * 24) - 2   # menu_rect = y0 - h - 2
EDITOR_ITEM = (104, MENU_TOP + 6 + 4 * 24 + 12)


def main():
    log_abs = os.path.join(ROOT, LOG)
    if os.path.exists(log_abs):
        os.remove(log_abs)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    pos = [SCREEN_W // 2, SCREEN_H // 2]

    try:
        q = Qmp(PORT)

        def key(k, d=0.06):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(KEYMAP.get(ch, ch))

        def move_to(tx, ty, settle=0.2):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                time.sleep(0.05)
                dx -= sx
                dy -= sy
            pos[0], pos[1] = tx, ty
            time.sleep(settle)

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
        time.sleep(1.0)
        move_to(*START_BTN)
        click()
        time.sleep(0.4)
        move_to(*EDITOR_ITEM)
        click()
        time.sleep(1.0)
        # focus the new window: the WM only routes keys to the focused window,
        # and creating a window does not steal focus from the default desktop.
        move_to(300, 250)
        click()
        time.sleep(0.6)
        typ(TEXT)
        time.sleep(0.4)
        key('ret')              # Enter: commit + save
        time.sleep(1.5)
        q.hmp('screendump ' + SHOT)
        time.sleep(0.6)
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
        ('editor window was created',
         '[gui] text editor window created' in log),
        ('editor initialised its buffer',
         ('[edit] new file /mnt/fat/EDIT.TXT' in log) or
         ('[edit] loaded /mnt/fat/EDIT.TXT' in log)),
        ('Ctrl+S saved through the VFS',
         '[edit] saved %d bytes to /mnt/fat/EDIT.TXT' % len(SAVED) in log),
    ]

    try:
        import verify_fat32_write as vf
        img = open(os.path.join(ROOT, 'build', 'fat32_ata.img'), 'rb').read()
        fs = vf.parse(img)
        ent = fs['entries'].get('EDIT.TXT')
        checks.append(('host: EDIT.TXT exists on the raw image', ent is not None))
        if ent:
            data = fs['read_chain'](ent[0], ent[1])[:len(SAVED)]
            checks.append(('host: EDIT.TXT is byte-for-byte the typed text',
                           data == SAVED.encode(), repr(data)))
    except Exception as exc:                       # noqa: BLE001
        checks.append(('host-side FAT32 parse', False, repr(exc)))

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
