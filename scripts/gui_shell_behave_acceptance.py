#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS desktop-shell behaviour gate (v0.68):

  * the taskbar is complete: a startup banner reports the pinned section
    (`[gui-shell] taskbar: 4 pinned buttons from x=50, window buttons from
    x=250`), a pinned icon launches its application (`pin launch 5 2`) and the pin
    disappears once the window exists, and the window button minimises/restores.
  * maximise/restore uses ONE animation path for every window: the interpolated
    rectangle is logged per frame (`scale rect idx=N x,y w,h`) and both the
    startup window (slot 0) and the pinned-launched calculator (slot 5) walk the
    same strictly monotonic width sequence.
  * the acrylic is LIVE while dragging: the shell re-anchors the glass every moved
    frame (`drag: live acrylic`), must NOT reuse the home-anchored layer
    (`drag: reusing window layer` must be absent), and two mid-drag frames really
    differ in the window's glass pixels.

Exit code 0 only when every check passes.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = int(os.environ.get('SHELLB_QMP_PORT', '45971'))
SERIAL = 'build/serial_shell_behave.log'
SERIAL2 = 'build/serial_shell_behave2.log'
DISK = 'build/fat32_ata.img'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H
START_BTN = (8, TASKBAR_Y + 5, 34, 28)
PIN_X0, PIN_PITCH, PIN_W = 50, 48, 44
WB_X0 = 250
WB_W, WB_PITCH = 96, 104
# window 0 (System Settings) title bar centre, and its size
W0_TITLE = (350, 135)
W0_SIZE = (430, 300)
# v0.69 context menu geometry: width 138, rows 30 px, 4 px pad, opened above the
# clicked button (ctx_y - h - 6)
CTX_W = 138
CTX_ROW_H = 30
CTX_PAD = 4


def ctx_geometry(click_x, click_y, rows):
    h = rows * CTX_ROW_H + 2 * CTX_PAD
    x = max(4, min(click_x - 40, SCREEN_W - CTX_W - 4))
    y = max(4, min(click_y - h - 6, TASKBAR_Y - h - 4))
    return x, y, CTX_W, h


def ctx_row_center(click_x, click_y, rows, row):
    x, y, w, _h = ctx_geometry(click_x, click_y, rows)
    return x + w // 2, y + CTX_PAD + row * CTX_ROW_H + CTX_ROW_H // 2


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

    def key(self, qcode, settle=0.6):
        self.cmd('send-key', keys=[{'type': 'qcode', 'data': qcode}])
        time.sleep(settle)

    def keys(self, text, delay=0.04):
        for ch in list(text) + ['ret']:
            self.cmd('send-key', keys=[{'type': 'qcode', 'data': ch}])
            time.sleep(delay)

    def shot(self, name, settle=0.35):
        p = os.path.join(ROOT, 'build', name)
        if os.path.exists(p):
            os.remove(p)
        self.cmd('screendump', filename=p)
        for _ in range(80):
            if os.path.exists(p) and os.path.getsize(p) > 1000:
                time.sleep(settle)
                return p
            time.sleep(0.05)
        raise RuntimeError('no screendump ' + name)


def title_controls(wx, wy, ww, wh=0):
    """(min, max, close) centres of a window's title-bar controls (control_rect)."""
    bx = wx + ww - 3 - 22 * 3 - 2 * 4
    by = wy + (30 - 22) // 2
    return ([(bx + k * 26 + 11, by + 11) for k in range(3)])


W0 = (150, 120, 430, 300)          # System Settings (open at startup)
W5 = (600, 210, 320, 360)          # Calculator  (opened by the pinned button)


def strip_colours(img, x0, y0, x1, y1):
    px = img.load()
    cols = set()
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            cols.add(px[x, y])
    return cols


def patch_diff(ia, ib, ax, ay, bx, by, u, v, n=36):
    """Sum of |channel differences| for the same window-relative patch."""
    pa, pb = ia.load(), ib.load()
    d = 0
    for dy in range(0, n, 2):
        for dx in range(0, n, 2):
            qa = pa[ax + u + dx, ay + v + dy]
            qb = pb[bx + u + dx, by + v + dy]
            d += abs(qa[0] - qb[0]) + abs(qa[1] - qb[1]) + abs(qa[2] - qb[2])
    return d


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
    for p in (SERIAL,):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))
    log = open(os.path.join(ROOT, 'build', 'qemu_shell_behave.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    pos = [512, 648]
    try:
        q = Qmp(QMP_PORT)
        print('waiting for the shell prompt ...', flush=True)
        time.sleep(75)
        q.keys('startgui')
        time.sleep(16)

        from PIL import Image

        def move_to(tx, ty):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                dx -= sx
                dy -= sy
                time.sleep(0.06)
            pos[0], pos[1] = tx, ty
            time.sleep(0.3)

        def click(tx, ty, settle=1.4, button=1):
            move_to(tx, ty)
            q.hmp('mouse_button %d' % button)
            time.sleep(0.2)
            q.hmp('mouse_button 0')
            time.sleep(settle)

        # ---- 1. taskbar: pinned icon visible at startup -----------------
        start = Image.open(q.shot('sb_start.ppm')).convert('RGB')
        pin_calc = (PIN_X0 + 2 * PIN_PITCH, PIN_W)
        before = strip_colours(start, pin_calc[0], TASKBAR_Y + 6,
                               pin_calc[0] + pin_calc[1], TASKBAR_Y + TASKBAR_H - 6)
        # ---- 2. click the pinned calculator -----------------------------
        click(pin_calc[0] + PIN_W // 2, TASKBAR_Y + 18, settle=2.5)
        after = Image.open(q.shot('sb_pin_after.ppm')).convert('RGB')
        after_cols = strip_colours(after, pin_calc[0], TASKBAR_Y + 6,
                                   pin_calc[0] + pin_calc[1],
                                   TASKBAR_Y + TASKBAR_H - 6)

        # ---- 3. window button for the calculator: minimise + restore -----
        calc_wb = (WB_X0 + 2 * WB_PITCH, WB_W)     # win0, win1 open, win5 open
        click(calc_wb[0] + WB_W // 2, TASKBAR_Y + 18, settle=1.8)
        click(calc_wb[0] + WB_W // 2, TASKBAR_Y + 18, settle=1.8)

        # ---- 3b. v0.69: right-click the calculator's window button --------
        calc_wb = (WB_X0 + 2 * WB_PITCH, WB_W)
        wb_click = (calc_wb[0] + WB_W // 2, TASKBAR_Y + 18)
        click(wb_click[0], wb_click[1], settle=1.2, button=2)
        shot_ctx = Image.open(q.shot('sb_ctx.ppm', settle=0.3)).convert('RGB')
        # the popup is painted above the taskbar (3 rows for a pinned window)
        pop_at = (wb_click[0] - 40, wb_click[1] - (3 * CTX_ROW_H + 2 * CTX_PAD) - 6)
        pop_now = strip_colours(shot_ctx, pop_at[0] + 6, pop_at[1] + 6,
                                pop_at[0] + CTX_W - 6, pop_at[1] + 3 * CTX_ROW_H)
        pop_before = strip_colours(start, pop_at[0] + 6, pop_at[1] + 6,
                                   pop_at[0] + CTX_W - 6, pop_at[1] + 3 * CTX_ROW_H)
        # row 1 = 最小化
        r1 = ctx_row_center(wb_click[0], wb_click[1], 3, 1)
        click(r1[0], r1[1], settle=1.8)
        # restore it again so the rest of the gate sees the same layout
        click(wb_click[0], wb_click[1], settle=1.8)

        # ---- 3c. v0.69: right-click the editor pin and unpin it -----------
        pin_editor = (PIN_X0 + 3 * PIN_PITCH + PIN_W // 2, TASKBAR_Y + 18)
        click(pin_editor[0], pin_editor[1], settle=1.2, button=2)
        r1 = ctx_row_center(pin_editor[0], pin_editor[1], 2, 1)
        click(r1[0], r1[1], settle=1.6)

        # ---- 4. maximise/restore the calculator via its title-bar button --
        calc_min, calc_max, calc_close = title_controls(*W5)
        click(calc_max[0], calc_max[1], settle=2.0)
        # maximised: the control moved to the screen's top-right corner
        _, full_max, _ = title_controls(0, 0, SCREEN_W)
        click(full_max[0], full_max[1], settle=2.0)

        # ---- 5. the same for window 0 (startup window) ---------------------
        _, w0_max, _ = title_controls(*W0)
        click(W0_TITLE[0], W0_TITLE[1], settle=1.2)     # focus window 0
        click(w0_max[0], w0_max[1], settle=2.0)
        _, w0_full_max, _ = title_controls(0, 0, SCREEN_W)
        click(w0_full_max[0], w0_full_max[1], settle=2.0)

        # ---- 6. live acrylic: drag window 0 over the wallpaper -----------
        move_to(W0_TITLE[0], W0_TITLE[1])
        q.hmp('mouse_button 1')
        time.sleep(0.3)
        # first position: window moves by (+250, +300)
        move_to(W0_TITLE[0] + 250, W0_TITLE[1] + 300)
        time.sleep(0.8)
        a = Image.open(q.shot('sb_drag_a.ppm', settle=0.3)).convert('RGB')
        ax, ay = W0_TITLE[0] + 250 - 200, W0_TITLE[1] + 300 - 15
        # second position: +120, +30 further
        move_to(W0_TITLE[0] + 370, W0_TITLE[1] + 330)
        time.sleep(0.8)
        b = Image.open(q.shot('sb_drag_b.ppm', settle=0.3)).convert('RGB')
        bx, by = ax + 120, ay + 30
        q.hmp('mouse_button 0')
        time.sleep(1.2)

        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:               # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    check('the shell loop started', '[gui-shell] shell: loop' in text)
    check('the taskbar reports its complete layout',
          'taskbar: 4 pinned buttons from x=50, window buttons from x=250' in text,
          'taskbar banner')
    check('the pinned calculator button is painted at startup',
          len(before) > 12, '%d colours' % len(before))
    check('clicking a pinned button launches its app',
          'pin launch 5 2' in text, 'pin launch 5 2')
    check('the pin disappears once the app is open',
          len(after_cols) < len(before),
          '%d -> %d colours' % (len(before), len(after_cols)))
    check('the calculator window button really minimises it (slot 5 bug)',
          'anim minimize 5' in text and 'anim done 1 5' in text
          and 'minimize 5' in text,
          'anim/minimize lines for slot 5')
    check('a second taskbar click restores the calculator',
          'restore focus 5' in text, 'restore focus 5')

    # ---- animation: same interpolation law for both windows -----------------
    rects = {}
    for line in text.splitlines():
        m = re.search(r'scale rect idx=(\d+) (-?\d+),(-?\d+) (\d+),(\d+)', line)
        if m:
            idx = int(m.group(1))
            rects.setdefault(idx, []).append((int(m.group(2)), int(m.group(3)),
                                              int(m.group(4)), int(m.group(5))))
    check('the calculator logged an interpolated rectangle per animation frame',
          len(rects.get(5, [])) >= 3, '%d frames' % len(rects.get(5, [])))
    check('window 0 logged the same kind of interpolated frames',
          len(rects.get(0, [])) >= 3, '%d frames' % len(rects.get(0, [])))
    seq5 = [r[2] for r in rects.get(5, [])][:4]
    seq0 = [r[2] for r in rects.get(0, [])][:4]
    check('both windows log four animation frames each',
          len(rects.get(5, [])) >= 4 and len(rects.get(0, [])) >= 4,
          'slot5 %d / slot0 %d' % (len(rects.get(5, [])), len(rects.get(0, []))))
    check('the calculator maximise walks a growing width sequence',
          len(seq5) >= 2 and all(seq5[i] < seq5[i + 1] for i in range(len(seq5) - 1)),
          'widths %s' % seq5)
    check('window 0 uses the same growing-width interpolation',
          len(seq0) >= 2 and all(seq0[i] < seq0[i + 1] for i in range(len(seq0) - 1)),
          'widths %s' % seq0)

    # ---- live acrylic -------------------------------------------------------
    check('dragging re-anchors the acrylic every moved frame',
          'drag: live acrylic' in text, 'drag: live acrylic')
    check('the home-anchored layer is not reused once the drag moves',
          text.count('drag: reusing window layer') <= 1,
          'reuse line count %d (one pre-movement frame is expected)'
          % text.count('drag: reusing window layer'))
    changed = 0
    probes = [(6, 40), (24, 150), (200, 250), (380, 60), (40, 260)]
    for (u, v) in probes:
        if (ax + u + 36 < SCREEN_W and ay + v + 36 < TASKBAR_Y - 4
                and bx + u + 36 < SCREEN_W and by + v + 36 < TASKBAR_Y - 4):
            if patch_diff(a, b, ax, ay, bx, by, u, v) > 60:
                changed += 1
    check('the glass pixels follow the wallpaper while dragging',
          changed >= 2, '%d/%d window-relative patches changed' % (changed, len(probes)))
    # ---- v0.69: context menu + pin persistence ------------------------------
    check('right-clicking a window button opens its context menu',
          'ctx menu open window 5' in text, 'ctx menu open window 5')
    check('the context menu is really painted above the taskbar',
          len(pop_now) > 0 and pop_now != pop_before,
          'popup colours %d vs wallpaper %d' % (len(pop_now), len(pop_before)))
    check('a menu row performs its action (minimise)',
          'ctx act minimize 5' in text and 'minimize 5' in text,
          'ctx act minimize 5')
    check('right-clicking a pinned button opens the pin menu',
          'ctx menu open pin 3 3' in text, 'ctx menu open pin 3 3')
    check('取消固定 removes the pin and saves the list',
          'pin unpin 3 3' in text and 'shell cfg saved: pinned=3 style=' in text,
          'pin unpin + saved')
    check('the pinned section shrinks after unpinning',
          'shell cfg saved: pinned=3 style=' in text,
          'saved 3 items')

    # host side: the FAT32 volume really carries SHELL.CFG
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import verify_fat32_write as vf
    img = open(os.path.join(ROOT, DISK), 'rb').read()
    fs = vf.parse(img)
    ent = fs['entries'].get('SHELL.CFG')
    cfg = b''
    if ent:
        cfg = fs['read_chain'](ent[0], ent[1])
    check('SHELL.CFG exists on the volume the guest booted',
          ent is not None, 'entries: %s' % ','.join(sorted(fs['entries'])))
    check('SHELL.CFG lists the remaining pinned slots (and the style line)',
          cfg == b'pinned=0,1,5\nstyle=glass\n', repr(cfg))

    check('no kernel PANIC', 'PANIC' not in text)

    # ---- v0.69: second boot on the same volume reloads the pinned list -----
    for p in (SERIAL2,):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))
    log2f = open(os.path.join(ROOT, 'build', 'qemu_shell_behave2.log'), 'wb')
    qemu2 = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL2,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % (QMP_PORT + 1),
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log2f, stderr=subprocess.STDOUT)
    try:
        q2 = Qmp(QMP_PORT + 1)
        time.sleep(75)
        q2.keys('startgui')
        time.sleep(16)
        q2.cmd('quit')
    finally:
        try:
            qemu2.wait(timeout=30)
        except Exception:               # noqa: BLE001
            qemu2.kill()
        log2f.close()
    text2 = open(os.path.join(ROOT, SERIAL2), 'rb').read().decode('utf-8', 'replace')
    check('a second boot reloads the pinned list from SHELL.CFG',
          'shell cfg loaded: pinned=3 style=0' in text2,
          'shell cfg loaded: pinned=3 style=0')
    check('the reloaded layout starts the window buttons at x=202',
          'window buttons from x=202' in text2,
          'window buttons from x=202')

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
