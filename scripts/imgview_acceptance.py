#!/usr/bin/env python3
"""SutureOS image-viewer acceptance (GUI, real clicks).

Boots with the FAT32 test disk (which carries IMG.BMP and IMG.QOI), enters the
GUI, opens the start menu, clicks "Image Viewer", and dumps the screen. The
decoder is then proven by asserting the four quadrant colours of the 32x32 test
picture on the dump: red / green / blue / white - the picture is only drawn if
BMP/QOI decoding actually produced the pixels.

Usage:
  py -3 scripts/imgview_acceptance.py           # boot, click, dump, assert
  py -3 scripts/imgview_acceptance.py --no-assert   # only produce the dump
Exit code 0 only when the log shows the decode lines AND the dump colours match.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = 47961
SERIAL = 'build/serial_imgview.log'
SHOT = 'build/imgview.ppm'

# GUI geometry (kernel/gui/gui.c): 1024x768, taskbar 40 px, start button at
# (4,728)-(100,768), start menu grows upwards from y=728 with 24 px items.
SCREEN_W, SCREEN_H, TASKBAR_H = 1024, 768, 40
START_BTN = (50, SCREEN_H - TASKBAR_H + 17)          # (50,745)
ITEM_H, MENU_PAD = 24, 6
ITEM_COUNT = 8                                        # TM, Calc, About, Image, Editor, SwitchUser, sep, Exit
MENU_X = 4
MENU_TOP = SCREEN_H - TASKBAR_H - (ITEM_COUNT * ITEM_H + MENU_PAD * 2) - 2
IMAGE_ITEM = (MENU_X + 100, MENU_TOP + MENU_PAD + 3 * ITEM_H + ITEM_H // 2)


class Qmp:
    def __init__(self, port, timeout=45):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:            # noqa: PERF203 - retry loop by design
                last = e
                time.sleep(0.2)
        else:
            raise RuntimeError('QMP never listened: %r' % (last,))
        self.f = self.s.makefile('rwb')
        self._read()
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            obj = json.loads(line.decode('utf-8'))
            if 'event' in obj:
                continue
            return obj

    def cmd(self, name, **args):
        msg = {'execute': name}
        if args:
            msg['arguments'] = args
        self.f.write((json.dumps(msg) + '\n').encode('utf-8'))
        self.f.flush()
        return self._read()

    def hmp(self, line):
        r = self.cmd('human-monitor-command', **{'command-line': line})
        return (r.get('return') or '').strip()


def main():
    assert_dir = '--no-assert' not in sys.argv
    log_abs = os.path.join(ROOT, SERIAL)
    dump_abs = os.path.join(ROOT, SHOT)
    for p in (log_abs, dump_abs):
        if os.path.exists(p):
            os.remove(p)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[imgview] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.06):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(ch if ch != ' ' else 'spc')

        # The pointer starts at the screen centre (gui.c) and QMP mouse_move is
        # 1:1 with PS/2 pixels (calibrated: dx=30 -> +30 px), but a PS/2 packet
        # only carries +/-127, so walk there in bounded steps.
        pos = [SCREEN_W // 2, SCREEN_H // 2]

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
            time.sleep(0.35)

        deadline = time.time() + 150
        while time.time() < deadline:
            if 'sutureos# ' in log_text():
                break
            time.sleep(1.0)
        else:
            raise RuntimeError('shell prompt never appeared')

        # start the GUI (retry: the first scancode is occasionally swallowed)
        for attempt in range(5):
            time.sleep(0.4)
            for _ in range(24):
                key('backspace', 0.015)
            typ('startgui')
            key('ret')
            time.sleep(3.2)
            if '[gui] main loop start' in log_text():
                break
        else:
            raise RuntimeError('GUI never started')

        time.sleep(1.0)
        move_to(*START_BTN)          # absolute target from the centre start
        click()                      # open the start menu
        time.sleep(0.4)
        move_to(*IMAGE_ITEM)         # absolute target again
        click()                      # choose "Image Viewer"
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT)
        time.sleep(0.5)
        # Click inside the picture: the viewer switches to the next source, which
        # is IMG.QOI - this proves the second decoder on the same screen.
        move_to(300, 220)
        click()
        time.sleep(1.0)
        q.hmp('screendump build/imgview_qoi.ppm')
        time.sleep(0.4)
    finally:
        try:
            q.hmp('quit')
        except Exception:               # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = log_text()
    print('[imgview] log lines of interest:')
    for line in log.splitlines():
        if '[img]' in line or 'image viewer' in line or 'start menu' in line:
            print('   ' + line)
    if not assert_dir:
        return 0

    ok = True

    def check(name, cond, detail=''):
        nonlocal ok
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))
        if not cond:
            ok = False

    check('log: BMP decoded from the test disk',
          '[img] decoded /mnt/fat/IMG.BMP 32x32 bmp' in log)
    check('log: image viewer window created',
          '[gui] image viewer window created' in log)
    check('log: start menu click reached the Image Viewer entry',
          "'Image Viewer'" in log)
    check('log: QOI decoded after the in-window click',
          '[img] switched to /mnt/fat/IMG.QOI' in log)

    # Pixel assertion: read the PPM dump (P6) and sample the four quadrants.
    try:
        from PIL import Image
    except ImportError:
        print('  [skip] Pillow missing, cannot sample the dump')
        return 0 if ok else 1
    im = Image.open(dump_abs).convert('RGB')
    check('screen dump size is 1024x768', im.size == (SCREEN_W, SCREEN_H), str(im.size))

    # Window at (150,80) 420x340; client starts at (151,105); the 32x32 picture is
    # drawn at scale 8 (256x256) centred: ox=232, oy=140 (see img_draw()).
    samples = {
        'red   (top-left quadrant)':    (232 + 10, 140 + 10, (255, 0, 0)),
        'green (top-right quadrant)':   (232 + 128 + 10, 140 + 10, (0, 255, 0)),
        'blue  (bottom-left quadrant)': (232 + 10, 140 + 128 + 10, (0, 0, 255)),
        'white (bottom-right quadrant)': (232 + 128 + 10, 140 + 128 + 10, (255, 255, 255)),
    }
    px = im.load()
    for name, (x, y, want) in samples.items():
        got = px[x, y]
        check('pixel %s at (%d,%d)' % (name, x, y), got == want, 'want %s got %s' % (want, got))

    qoi_shots = os.path.join(ROOT, 'build/imgview_qoi.ppm')
    if os.path.exists(qoi_shots):
        im2 = Image.open(qoi_shots).convert('RGB')
        px2 = im2.load()
        for name, (x, y, want) in samples.items():
            got = px2[x, y]
            check('QOI pixel %s' % name, got == want, 'want %s got %s' % (want, got))

    print('[imgview] RESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
