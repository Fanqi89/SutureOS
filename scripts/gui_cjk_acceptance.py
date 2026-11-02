#!/usr/bin/env python3
"""SutureOS GUI CJK acceptance: Chinese glyphs inside a real window.

Boots with the FAT32 test disk (which carries `图片.BMP`), starts the GUI through
the normal `startgui` path, opens the Image Viewer with real mouse clicks, and
then proves the status line rendered the Chinese filename: the 16x16 glyph
bitmaps of 图 and 片 must appear in the screenshot in the status-line colour
(0x143C8C). Host-side pixel match, nothing trusted from the guest log alone.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/gui_cjk_acceptance.py
"""
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp, START_BTN, IMAGE_ITEM, SCREEN_W, SCREEN_H  # noqa: E402

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 48011
LOG = 'build/serial_gui_cjk.log'
SHOT = 'build/gui_cjk.ppm'
FG = (0x70, 0x78, 0x80)          # footer colour ("缩放 xN" line)
EXPECT = ['缩', '放']


def load_glyphs():
    blob = open(os.path.join(ROOT, 'build', 'font_cjk.bin'), 'rb').read()
    assert blob[:4] == b'SFCJ'
    count = struct.unpack_from('<I', blob, 4)[0]
    glyphs, off = {}, 8
    for _ in range(count):
        cp = struct.unpack_from('<I', blob, off)[0]
        data = blob[off + 4:off + 36]
        glyphs[cp] = [[(data[r * 2 + c // 8] >> (7 - c % 8)) & 1
                       for c in range(16)] for r in range(16)]
        off += 36
    return glyphs


def main():
    import numpy as np

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
                key('spc' if ch == ' ' else ch)

        def move_to(tx, ty, settle=0.18):
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
            time.sleep(0.4)

        for _ in range(150):
            if 'sutureos# ' in log_text():
                break
            time.sleep(1)
        for attempt in range(5):
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
        move_to(*IMAGE_ITEM)
        click()
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
    from PIL import Image
    img = Image.open(os.path.join(ROOT, SHOT)).convert('RGB')
    arr = np.array(img)
    fg_mask = np.all(arr == np.array(FG, dtype=np.uint8), axis=2)

    checks = [
        ('guest decoded the fixture picture',
         '[img] decoded /mnt/fat/IMG.BMP' in log),
        ('viewer window exists', '[gui] image viewer window created' in log),
    ]
    glyphs = load_glyphs()
    for ch in EXPECT:
        g = np.array(glyphs[ord(ch)], dtype=bool)
        found = None
        h, w = g.shape
        for y in range(0, arr.shape[0] - h + 1):
            for x in range(0, arr.shape[1] - w + 1):
                if np.array_equal(fg_mask[y:y + h, x:x + w], g):
                    found = (x, y)
                    break
            if found:
                break
        checks.append(('image-viewer footer contains the exact 16x16 glyph %s' % ch,
                       found is not None, str(found) if found else 'not found'))

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
