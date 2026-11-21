#!/usr/bin/env python3
"""SutureOS picture-browser acceptance (v0.62): BMP and QOI decoders, both real.

The viewer used to paint the logo. It now decodes files from the FAT32 volume and
browses them with Left/Right while it owns the keyboard focus:

  * open the window from the start menu (row 3, 图片查看器),
  * it decodes /mnt/fat/IMG.BMP first  -> quadrants on screen + caption "IMG.BMP",
  * Right -> /mnt/fat/IMG.QOI          -> same quadrants (proves the QOI decoder) and
    the caption changes to "IMG.QOI",
  * Left  -> back to the BMP           -> caption is the BMP one again.

Pixel evidence: the four red / green / blue / white quadrant centres and the caption
strip diff between the two screenshots (a caption that does not follow the decoded
file would be a bug this gate must catch).

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
QMP_PORT = int(os.environ.get('IMGBROWSE_QMP_PORT', '45901'))
SERIAL = 'build/serial_image_browse.log'
SHOT_BMP = 'build/acc_img_bmp.ppm'
SHOT_QOI = 'build/acc_img_qoi.ppm'
SHOT_BACK = 'build/acc_img_back.ppm'
DISK = 'build/fat32_ata.img'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_Y = 730
MENU_X = 12
MENU_ITEMS_Y = 444            # 6-row menu: TASKBAR_Y - 324 - 8 + 46
MENU_ROW_H = 44
IMG_ROW = 2                   # 图片查看器
START_BTN = (25, 745)

# mirrored from apps.rs (window 2 = 210,430 360x240, PAD 16, TITLE_H 30)
WIN2 = (210, 430, 360, 240)
PAD, TITLE_H = 16, 30
IMG_FILE = 32                 # the fixture is 32x32
LW = 256                      # sentinel for "not computed"


def layout():
    x0 = WIN2[0] + PAD
    cy0 = WIN2[1] + TITLE_H + 30
    avail_w = WIN2[2] - 2 * PAD
    avail_h = WIN2[3] - 26 - TITLE_H - 30
    scale = max(1, min(avail_w // IMG_FILE, avail_h // IMG_FILE))
    dw = IMG_FILE * scale
    ox = x0 + (avail_w - dw) // 2
    oy = cy0 + (avail_h - dw) // 2
    return ox, oy, dw, scale, cy0, avail_h, x0


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


def press_keys(q, keys, delay=0.14):
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


def click(q, pos, tx, ty, settle=1.6):
    move_to(q, pos, tx, ty)
    q.hmp('mouse_button 1')
    time.sleep(0.20)
    q.hmp('mouse_button 0')
    time.sleep(settle)


def shot(q, path):
    full = os.path.join(ROOT, path)
    if os.path.exists(full):
        os.remove(full)
    q.cmd('screendump', filename=path)
    for _ in range(80):
        if os.path.exists(full) and os.path.getsize(full) > 1000:
            time.sleep(0.4)
            return
        time.sleep(0.1)
    raise RuntimeError('no screendump')


def load(path):
    from PIL import Image
    return Image.open(os.path.join(ROOT, path)).convert('RGB')


def boot():
    for p in (SERIAL, SHOT_BMP, SHOT_QOI, SHOT_BACK):
        full = os.path.join(ROOT, p)
        if os.path.exists(full):
            os.remove(full)
    log = open(os.path.join(ROOT, 'build/qemu_image_browse.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


def caption_sig(px, x0, cy0, avail_h):
    """Mean colour of the caption strip (the line right under the picture)."""
    y = cy0 + avail_h + 6
    r = g = b = 0
    n = 0
    for yy in range(y, y + 14):
        for xx in range(x0, x0 + 200):
            pr, pg, pb = px[xx, yy]
            r += pr
            g += pg
            b += pb
            n += 1
    return (r / n, g / n, b / n)


def quadrants(px, ox, oy, dw):
    out = []
    for (name, qx, qy, want) in [
        ('red', ox + dw // 4, oy + dw // 4, (255, 0, 0)),
        ('green', ox + 3 * dw // 4, oy + dw // 4, (0, 255, 0)),
        ('blue', ox + dw // 4, oy + 3 * dw // 4, (0, 0, 255)),
        ('white', ox + 3 * dw // 4, oy + 3 * dw // 4, (255, 255, 255)),
    ]:
        got = px[qx, qy]
        out.append((name, got, all(abs(got[i] - want[i]) <= 24 for i in range(3))))
    return out


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

    ox, oy, dw, scale, cy0, avail_h, x0 = layout()
    qemu, log = boot()
    pos = [512, 648]
    try:
        q = Qmp(QMP_PORT)
        print('waiting for the shell prompt ...', flush=True)
        time.sleep(75)
        press_keys(q, list('startgui') + ['ret'])
        time.sleep(16)
        click(q, pos, *START_BTN)
        click(q, pos, MENU_X + 100, MENU_ITEMS_Y + IMG_ROW * MENU_ROW_H + 19)
        time.sleep(2.5)
        shot(q, SHOT_BMP)
        press_keys(q, ['right'])
        time.sleep(2.5)
        shot(q, SHOT_QOI)
        press_keys(q, ['left'])
        time.sleep(2.5)
        shot(q, SHOT_BACK)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    check('the start menu launched the picture viewer', 'menu launch 2' in text,
          'menu launch 2')
    check('the first file decoded is the BMP',
          'image: loaded IMG.BMP 32x32' in text,
          'loaded IMG.BMP 32x32')
    check('Right browses to the QOI file (decoder ran)',
          'image shift 1' in text and 'image: loaded IMG.QOI 32x32' in text,
          'shift 1 -> IMG.QOI')
    check('Left browses back to the BMP',
          'image shift 2' in text and text.count('image: loaded IMG.BMP') >= 2,
          '%d BMP load(s)' % text.count('image: loaded IMG.BMP'))

    bmp = load(SHOT_BMP)
    qoi = load(SHOT_QOI)
    back = load(SHOT_BACK)
    bpx, qpx, kpx = bmp.load(), qoi.load(), back.load()

    for (label, img_px) in (('BMP', bpx), ('QOI', qpx)):
        quads = quadrants(img_px, ox, oy, dw)
        hits = sum(1 for (_, _, ok) in quads if ok)
        check('%s file shows the four colour quadrants (decode -> draw)' % label,
              hits == 4, '%d/4 %s' % (hits, [(n, px) for (n, px, ok) in quads]))

    cb = caption_sig(bpx, x0, cy0, avail_h)
    cq = caption_sig(qpx, x0, cy0, avail_h)
    ck = caption_sig(kpx, x0, cy0, avail_h)
    diff_q = max(abs(cb[i] - cq[i]) for i in range(3))
    diff_back = max(abs(cb[i] - ck[i]) for i in range(3))
    check('the caption follows the decoded file (BMP vs QOI differs)',
          diff_q > 0.8, 'channel delta %.2f' % diff_q)
    check('browsing back restores the BMP caption',
          diff_back < 1.0, 'channel delta %.2f' % diff_back)
    check('no kernel PANIC',
          'PANIC' not in text.upper().replace('NO PANIC', ''))

    ok = sum(1 for c in checks if c[1])
    print('[imgbrowse] checks=%d ok=%d fail=%d'
          % (len(checks), ok, len(checks) - ok), flush=True)
    if ok == len(checks):
        print('[imgbrowse] RESULT: PASS', flush=True)
        return 0
    print('[imgbrowse] RESULT: FAIL', flush=True)
    return 1


if __name__ == '__main__':
    sys.exit(main())
