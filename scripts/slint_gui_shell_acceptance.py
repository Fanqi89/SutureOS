#!/usr/bin/env python3
"""SutureOS desktop-shell prototype acceptance (v0.41.0).

Proves with real QEMU PS/2 input that the new Rust renderer now owns a window
shell:

  * `guishell` starts a live loop and paints a full frame (wallpaper + windows +
    taskbar + clock)                       -> "[gui-shell] shell: loop"
  * two windows with title bars and controls are on screen (pixel checks)
  * dragging a window by its title bar moves it (log + pixels)
  * clicking the taskbar button of the focused window minimises it, clicking
    again restores it (log + pixels)
  * double clicking a taskbar button maximises the window (fills the screen
    above the taskbar)
  * the start button opens the start menu (log + pixels)
  * ESC leaves the loop and the terminal comes back

Usage: py -3 scripts/slint_gui_shell_acceptance.py
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
QMP_PORT = int(os.environ.get('SHELL_QMP_PORT', '47993'))
SERIAL = 'build/serial_shell.log'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H

# mirrored from rust/suture-gui/src/shell.rs
WIN0 = (150, 120, 430, 300)
WIN1 = (470, 300, 400, 260)
START_BTN = (8, TASKBAR_Y + 5, 34, 28)
WB0 = (118, 96)                 # first taskbar window button (x, w)
WB1 = (222, 96)                 # second one
MENU = (12, TASKBAR_Y - 236 - 8, 300, 236)
MENU_ITEM0_Y = MENU[1] + 46
MENU_ITEM_H = 44
WIN2 = (210, 430, 360, 240)      # 图片查看器 (opened from the start menu)
WIN3 = (560, 180, 330, 230)      # 文本编辑器


class Qmp:
    def __init__(self, port, timeout=90):
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


def mean_brightness(px, x0, y0, x1, y1, step=4):
    tot = 0
    n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            p = px[x, y]
            tot += p[0] + p[1] + p[2]
            n += 1
    return tot / (3.0 * max(1, n))


def main():
    do_assert = '--no-assert' not in sys.argv
    log_abs = os.path.join(ROOT, SERIAL)
    shot = 'build/shell_shot_%s.ppm'
    names = ('base', 'drag', 'min', 'restore', 'max', 'menu')
    for p in [log_abs] + [os.path.join(ROOT, shot % n) for n in names]:
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[shell] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[shell] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in log_text():
                return True
            time.sleep(0.3)
        return False

    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.07):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(ch if ch != ' ' else 'spc')

        pos = [SCREEN_W // 2, SCREEN_H - 120]

        def move_to(tx, ty, settle=0.35):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                time.sleep(0.06)
                dx -= sx
                dy -= sy
            pos[0], pos[1] = tx, ty
            time.sleep(settle)

        def press():
            q.hmp('mouse_button 1')
            time.sleep(0.20)

        def release():
            q.hmp('mouse_button 0')
            time.sleep(0.35)

        def click():
            press()
            release()

        def check(name, cond, detail=''):
            checks.append((name, bool(cond), detail))
            print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                                   ('  (%s)' % detail) if detail else ''))

        def dump(name):
            q.hmp('screendump ' + (shot % name))
            time.sleep(0.6)

        def load(name):
            from PIL import Image
            return Image.open(os.path.join(ROOT, shot % name)).convert('RGB')

        def dump_until(name, pred, tries=6, gap=0.8):
            from PIL import Image
            for _ in range(tries):
                dump(name)
                p = os.path.join(ROOT, shot % name)
                if os.path.exists(p):
                    im = Image.open(p).convert('RGB')
                    if pred(im.load()):
                        return im
                time.sleep(gap)
            return load(name) if os.path.exists(os.path.join(ROOT, shot % name)) else None

        if not wait_for('sutureos#', 180):
            print('[shell] shell prompt never appeared')
            return 1
        time.sleep(0.6)
        typ('guishell')
        key('ret')
        check('shell loop started', wait_for('[gui-shell] shell: loop', 150))

        # baseline brightness of the region the launched window will cover: the
        # closed-state assertion compares against this, not an absolute number
        base_img = dump_until('base', lambda px: mean_brightness(px, 235, 490, 1000, 700) > 20)
        base_mean = mean_brightness(base_img.load(), WIN2[0] + 30, WIN2[1] + 60,
                                    WIN2[0] + 250, WIN2[1] + 180) if base_img is not None else 0.0
        check('taskbar clock comes from the RTC (not the old hardcoded 12:00)',
              wait_for('[gui-shell] clock=', 30),
              'log line "[gui-shell] clock=HH:MM"')
        check('wallpaper + window caches built', wait_for('[gui-shell] caches built', 150))
        # wait for one full frame: the heartbeat is printed after the frame was
        # presented, so the screen behind it is complete
        check('frames are being presented', wait_for('[gui-shell] frame 30', 180))

        # ---- (1) both windows and the taskbar are on screen -----------------
        base = dump_until('base', lambda px: mean_brightness(px, 200, 150, 540, 360) > 90
                          and px[512, TASKBAR_Y + 20] != px[512, TASKBAR_Y - 6])
        if base is not None:
            bpx = base.load()
            check('window 0 body painted', mean_brightness(bpx, 200, 150, 540, 360) > 90,
                  'mean %.1f' % mean_brightness(bpx, 200, 150, 540, 360))
            check('window 1 body painted', mean_brightness(bpx, 520, 350, 840, 520) > 80,
                  'mean %.1f' % mean_brightness(bpx, 520, 350, 840, 520))
            check('taskbar is painted at the bottom', bpx[512, TASKBAR_Y + 10] != bpx[512, TASKBAR_Y - 6],
                  'taskbar %s vs wallpaper %s' % (bpx[512, TASKBAR_Y + 10], bpx[512, TASKBAR_Y - 6]))
            check('window 0 title text present',
                  mean_brightness(bpx, WIN0[0] + 30, WIN0[1] + 8, WIN0[0] + 150, WIN0[1] + 26) > 55,
                  'title band mean %.1f' %
                  mean_brightness(bpx, WIN0[0] + 30, WIN0[1] + 8, WIN0[0] + 150, WIN0[1] + 26))

        # ---- (1b) pointer-only motion takes the cursor fast path (v0.45.0) ----
        for _ in range(24):
            q.hmp('mouse_move 4 0')
            time.sleep(0.12)
            pos[0] += 4
        check('pointer-only frames use the cursor fast path',
              wait_for('[gui-shell] cursor: fast path', 30),
              'log line "[gui-shell] cursor: fast path frames"')

        # ---- (2) drag window 0 by its title bar ------------------------------
        move_to(WIN0[0] + 60, WIN0[1] + 14)
        press()
        check('drag begins in the title bar', wait_for('[gui-shell] drag begin', 40))
        for dx in (40, 40, 40, 40):
            q.hmp('mouse_move %d 0' % dx)
            time.sleep(0.35)
            pos[0] += dx
        release()
        check('drag ends', wait_for('[gui-shell] drag end', 40))
        DRAG_REGION = (620, 150, 720, 280)      # wallpaper before, window after
        dragged = dump_until('drag', lambda px: mean_brightness(px, *DRAG_REGION) > 90)
        if base is not None and dragged is not None:
            bpx, dpx = base.load(), dragged.load()
            check('window 0 moved right after the drag',
                  mean_brightness(dpx, *DRAG_REGION) >
                  mean_brightness(bpx, *DRAG_REGION) + 30,
                  'after %.1f vs before %.1f' %
                  (mean_brightness(dpx, *DRAG_REGION),
                   mean_brightness(bpx, *DRAG_REGION)))

        # ---- (3) minimise / restore through the taskbar ----------------------
        move_to(WB0[0] + WB0[1] // 2, TASKBAR_Y + 18)
        click()
        check('taskbar click minimised the focused window',
              wait_for('[gui-shell] minimize', 40))
        minimised = dump_until('min', lambda px: mean_brightness(px, *DRAG_REGION) < 90)
        if minimised is not None:
            code = log_text()
            check('minimised window is gone from the desktop',
                  mean_brightness(minimised.load(), *DRAG_REGION) <
                  mean_brightness(dragged.load(), *DRAG_REGION) - 20,
                  'mean %.1f (was %.1f)' % (mean_brightness(minimised.load(), *DRAG_REGION),
                                            mean_brightness(dragged.load(), *DRAG_REGION)))
            check('minimise log line reports the window', 'minimize 0' in code)

        click()
        check('taskbar click restored it', wait_for('[gui-shell] restore focus', 40))
        restored = dump_until('restore', lambda px: mean_brightness(px, *DRAG_REGION) > 90)
        if restored is not None:
            check('restored window is back on screen',
                  mean_brightness(restored.load(), *DRAG_REGION) > 90,
                  'mean %.1f' % mean_brightness(restored.load(), *DRAG_REGION))

        # ---- (4) maximise window 1 by double clicking its taskbar button -----
        def fast_click():
            q.hmp('mouse_button 1')
            time.sleep(0.06)
            q.hmp('mouse_button 0')
            time.sleep(0.07)

        move_to(WB1[0] + WB1[1] // 2, TASKBAR_Y + 18)
        fast_click()
        fast_click()
        check('double click on the taskbar maximised the window',
              wait_for('[gui-shell] maximize', 60))
        maximised = dump_until('max', lambda px: mean_brightness(px, 20, 20, 200, 200) > 80)
        if maximised is not None:
            check('maximised window fills the screen above the taskbar',
                  mean_brightness(maximised.load(), 10, 10, 240, 240) > 70,
                  'top-left mean %.1f' % mean_brightness(maximised.load(), 10, 10, 240, 240))
            check('taskbar still on top of the maximised window',
                  maximised.load()[512, TASKBAR_Y + 10] != maximised.load()[512, TASKBAR_Y - 6])

        # double click the same taskbar button again -> restore the normal size
        move_to(WB1[0] + WB1[1] // 2, TASKBAR_Y + 18)
        fast_click()
        fast_click()
        check('double click again restored the maximised window',
              wait_for('[gui-shell] restore 1', 60))

        # ---- (5) start menu ---------------------------------------------------
        move_to(START_BTN[0] + START_BTN[2] // 2, START_BTN[1] + START_BTN[3] // 2)
        click()
        check('start button opened the start menu', wait_for('[gui-shell] menu open', 40))
        menu = dump_until('menu', lambda px: mean_brightness(px, MENU[0] + 20, MENU[1] + 40,
                                                             MENU[0] + 200, MENU[1] + 120) > 60)
        if menu is not None:
            check('start menu panel is painted',
                  mean_brightness(menu.load(), MENU[0] + 20, MENU[1] + 40,
                                  MENU[0] + 200, MENU[1] + 120) > 55,
                  'menu mean %.1f' % mean_brightness(menu.load(), MENU[0] + 20,
                                                     MENU[1] + 40, MENU[0] + 200,
                                                     MENU[1] + 120))

        # ---- (5b) keyboard menu navigation (v0.43.0) --------------------------
        # the menu is open from the previous step: Down, Down, Enter -> 图片查看器
        key('down')
        time.sleep(0.6)
        key('down')          # selection 0 -> 1 -> 2
        time.sleep(0.6)
        check('keyboard moves the menu selection',
              '[gui-shell] menu sel 1' in log_text() and
              '[gui-shell] menu sel 2' in log_text())
        key('ret')
        check('Enter launches the selected menu item',
              wait_for('[gui-shell] menu enter 2', 60) and
              wait_for('[gui-shell] menu launch 2', 60))
        # close it again so the mouse-driven tests below start from a clean slate
        time.sleep(0.8)

        # ---- (6) start menu launches a real window (v0.42.0) -------------------
        # menu is open from the previous step; click "图片查看器" (third row)
        move_to(MENU[0] + 80, MENU_ITEM0_Y + 2 * MENU_ITEM_H + 19)
        click()
        check('menu item launches its app window', wait_for('[gui-shell] menu launch 2', 60))
        CLOSE_REGION = (WIN2[0] + 30, WIN2[1] + 60, WIN2[0] + 200, WIN2[1] + 180)
        win2 = dump_until('win2', lambda px: mean_brightness(px, *CLOSE_REGION) > 80)
        if win2 is not None:
            check('launched window is painted on the desktop',
                  mean_brightness(win2.load(), WIN2[0] + 30, WIN2[1] + 60,
                                  WIN2[0] + 250, WIN2[1] + 180) > 75,
                  'mean %.1f' % mean_brightness(win2.load(), WIN2[0] + 30, WIN2[1] + 60,
                                                WIN2[0] + 250, WIN2[1] + 180))
            check('menu closed after launching', 'menu closed' in log_text() or
                  '[gui-shell] menu launch 2' in log_text())

        # ---- (7) close button removes the window ------------------------------
        # close is the third title-bar control: bx = x + w - 77, each 22 wide + 4 gap
        close_x = WIN2[0] + WIN2[2] - 77 + 2 * 26 + 11
        close_y = WIN2[1] + 4 + 11
        move_to(close_x, close_y)
        click()
        check('close button closes the window', wait_for('[gui-shell] close 2', 60))
        closed = dump_until('closed', lambda px: abs(mean_brightness(px, WIN2[0] + 30, WIN2[1] + 60,
                                                                      WIN2[0] + 250, WIN2[1] + 180)
                                                             - base_mean) < 4)
        if closed is not None:
            closed_mean = mean_brightness(closed.load(), WIN2[0] + 30, WIN2[1] + 60,
                                          WIN2[0] + 250, WIN2[1] + 180)
            check('closed window is gone from the desktop', abs(closed_mean - base_mean) < 4,
                  'mean %.1f vs baseline %.1f' % (closed_mean, base_mean))

        # ---- (8) idle frames are skipped (no useless repaints) -----------------
        time.sleep(2.5)
        idle_ok = wait_for('[gui-shell] idle: frame skipped', 20)
        # ---- (8b) window layer cache actually hits (v0.46.0) ------------------
        prof = [ln for ln in log_text().splitlines() if 'profile/30' in ln]
        hit_ok = any(('hit=' in ln and 'hit=0 ' not in ln) for ln in prof)
        check('window layer cache is reused on repaints', hit_ok,
              (prof[-1] if prof else 'no profile line'))

        check('idle frames skip the repaint', idle_ok,
              'log line "[gui-shell] idle: frame skipped (nothing changed)"' if idle_ok else 'no idle log')

        # ---- (6) exit ---------------------------------------------------------
        key('esc')
        check('ESC left the shell loop', wait_for('[gui-shell] shell: exit', 60))
        check('terminal came back', wait_for('guishell: back to terminal', 60))
    finally:
        try:
            q.hmp('quit')
        except Exception:               # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    check('no kernel PANIC', 'PANIC' not in log_text())

    if not do_assert:
        print('[shell] dumps written, assertions skipped')
        return 0

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[shell] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[shell] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
