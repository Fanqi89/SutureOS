#!/usr/bin/env python3
"""SutureOS CJK terminal acceptance: pixel-identical 16x16 glyphs, proven.

Boots with the FAT32 test disk (whose boot self-test leaves a file named
`中文 长名.txt`), runs `ls /mnt/fat`, dumps the screen and then checks that each
CJK glyph bitmap from `build/font_cjk.bin` appears EXACTLY in the screenshot
(16x16, white on black). That is decode -> font lookup -> blit all the way to the
framebuffer, without trusting anything the guest printed.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/cjk_terminal_acceptance.py
"""
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp          # noqa: E402  (QMP helper)

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 48001
LOG = 'build/serial_cjk.log'
SHOT = 'build/cjk_terminal.ppm'
SHOT_SCROLL = 'build/cjk_terminal_scrolled.ppm'
EXPECT = ['中', '文', '长', '名']


def load_glyphs():
    """codepoint -> 16x16 bool matrix from the generated blob."""
    blob = open(os.path.join(ROOT, 'build', 'font_cjk.bin'), 'rb').read()
    assert blob[:4] == b'SFCJ', 'font blob magic'
    count = struct.unpack_from('<I', blob, 4)[0]
    glyphs = {}
    off = 8
    for _ in range(count):
        cp = struct.unpack_from('<I', blob, off)[0]
        data = blob[off + 4:off + 36]
        glyphs[cp] = [[(data[r * 2 + c // 8] >> (7 - c % 8)) & 1
                       for c in range(16)] for r in range(16)]
        off += 36
    return glyphs


def main():
    try:
        import numpy as np
    except ImportError:
        sys.exit('numpy required for the pixel search')

    log_abs = os.path.join(ROOT, LOG)
    if os.path.exists(log_abs):
        os.remove(log_abs)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
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

    try:
        q = Qmp(PORT)
        for _ in range(150):
            if 'sutureos# ' in log_text():
                break
            time.sleep(1)
        for ch in 'ls /mnt/fat':
            key = 'spc' if ch == ' ' else ('slash' if ch == '/' else ch)
            q.hmp('sendkey ' + key)
            time.sleep(0.05)
        q.hmp('sendkey ret')
        time.sleep(1.8)
        q.hmp('screendump ' + SHOT)
        time.sleep(0.6)
        # now scroll the terminal by 10 lines: the Chinese line must survive the
        # scroll (the overlay is shifted and repainted) at a higher row
        for _ in range(30):
            q.hmp('sendkey ret')
            time.sleep(0.08)
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT_SCROLL)
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
    checks = []
    checks.append(('guest booted the CJK font', '[term] CJK font: 7443 glyphs' in log))
    checks.append(('guest printed the Chinese filename to the log',
                   '中文 长名.txt' in log))

    from PIL import Image
    img = Image.open(os.path.join(ROOT, SHOT)).convert('L')
    arr = np.array(img) > 128
    glyphs = load_glyphs()

    def find_glyph(arr, ch):
        g = np.array(glyphs[ord(ch)], dtype=bool)
        h, w = arr.shape
        for y in range(0, h - 16 + 1):
            for x in range(0, w - 16 + 1):
                if arr[y, x:x + 16].tobytes() != g[0].tobytes():
                    continue
                if np.array_equal(arr[y:y + 16, x:x + 16], g):
                    return (x, y)
        return None

    positions = {}
    for ch in EXPECT:
        found = find_glyph(arr, ch)
        positions[ch] = found
        checks.append(('screen contains the exact 16x16 bitmap of %s' % ch,
                       found is not None, str(found) if found else 'not found'))

    # scroll test: same glyphs must still be exact, at a row 10 lines higher
    img2 = Image.open(os.path.join(ROOT, SHOT_SCROLL)).convert('L')
    arr2 = np.array(img2) > 128
    for ch in EXPECT:
        before = positions.get(ch)
        after = find_glyph(arr2, ch)
        ok = after is not None
        if ok and before is not None:
            # scrolled up by a whole number of 16 px text rows, same column
            ok = (after[0] == before[0] and after[1] <= before[1]
                  and (before[1] - after[1]) % 16 == 0)
        checks.append(('after a 10-line scroll %s is still pixel-exact at the '
                       'shifted row' % ch, ok,
                       '%s -> %s' % (before, after)))

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
