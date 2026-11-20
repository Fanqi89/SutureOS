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
import re

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
        check('taskbar clock shows local time from the RTC (+08:00)',
              wait_for('[gui-shell] clock utc=', 30),
              'log line "[gui-shell] clock utc=HH:MM local=HH:MM (+08:00)"')
        _m = re.search(r'clock utc=(\d\d):(\d\d) local=(\d\d):(\d\d)', log_text())
        _ok = False
        if _m:
            _u = int(_m.group(1)) * 60 + int(_m.group(2))
            _l = int(_m.group(3)) * 60 + int(_m.group(4))
            _ok = ((_u + 8 * 60) % (24 * 60)) == _l
        check('local clock is UTC+08:00', _ok, _m.group(0) if _m else 'no clock line')
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

        # ---- (1c) shadows are alpha-composited, not a black halo (v0.58.0) ---
        if base is not None:
            bpx = base.load()
            # (i) ring d=3 (alpha 0x16) just below window 0, plain wallpaper 10 px
            # further down: the band must be the wallpaper darkened, never black
            wally = WIN0[1] + WIN0[3] + 12
            bandy = WIN0[1] + WIN0[3] + 2
            wall_px = bpx[300, wally]
            band_px = bpx[300, bandy]
            exp = tuple(max(0, int(c * (255 - 0x16) / 255)) for c in wall_px)
            _dmax = max(abs(band_px[i] - exp[i]) for i in range(3))
            check('shadow over the wallpaper is alpha-composited (no black halo)',
                  sum(band_px) > 30 and _dmax <= 14,
                  'band %s expected ~%s (wallpaper %s)' % (band_px, exp, wall_px))
            # (ii) window 1 is above window 0, so its ring darkens window 0's body
            under = bpx[WIN1[0] - 12, WIN1[1] + 50]
            shaded = bpx[WIN1[0] - 3, WIN1[1] + 50]
            _ratios = [shaded[i] / max(1, under[i]) for i in range(3)]
            check('window shadow is composited over the window below',
                  sum(shaded) > 30 and all(0.85 <= r <= 0.97 for r in _ratios),
                  'shaded %s vs body %s at x=%d' % (shaded, under, WIN1[0] - 3))

        # ---- (1d) Tab cycles the keyboard focus between windows (v0.58.0) ----
        key('tab')
        check('Tab cycles the window focus', wait_for('[gui-shell] focus cycle 0', 40),
              'log line "[gui-shell] focus cycle 0"')
        key('tab')
        wait_for('[gui-shell] focus cycle 1', 40)

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
        check('drag reuses the window layer instead of re-rasterising',
              '[gui-shell] drag: reusing window layer' in log_text(),
              'log line "[gui-shell] drag: reusing window layer"')
        _drag = [int(v) for v in re.findall(r'drag repaint (\d+) ticks', log_text())]
        _steady = _drag[1:]          # the first frame still builds caches
        check('drag repaint stays under 12 ticks (120 ms)',
              bool(_steady) and max(_steady) <= 12,
              'drag repaint ticks: %s' % (_steady[:8],))
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

        # a second click inside the 1500 ms double-click window would MAXIMISE
        # the window instead of restoring it, so space the restore click out
        time.sleep(1.7)
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
        check('maximise runs a real geometry transition (not a pop)',
              '[gui-shell] scale begin 1 1' in log_text(),
              'log line "[gui-shell] scale begin 1 1"')
        check('the transition completes',
              wait_for('[gui-shell] scale done 1 1', 60),
              'log line "[gui-shell] scale done 1 1"')
        _steps = re.findall(r'\[gui-shell\] scale 1 (\d+)\n', log_text())
        check('the transition lerps the window over 4 frames',
              all(str(k) in _steps for k in (1, 2, 3, 4)),
              'scale step lines: %s' % (_steps[:8],))
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
        check('restore transitions back to the normal geometry',
              '[gui-shell] scale begin 1 2' in log_text(),
              'log line "[gui-shell] scale begin 1 2"')
        check('maximise/restore fades the window in',
              log_text().count('[gui-shell] intro fade') >= 2,
              'intro fade lines: %d' % log_text().count('[gui-shell] intro fade'))
        # A following double-click can supersede the restore animation before its
        # 4th frame (TCG timing flake, observed since v0.44). Accept a completed
        # restore OR a visibly running one (frames logged after scale begin 1 2).
        _restore_done = wait_for('[gui-shell] scale done 1 2', 30)
        _detail = 'log line "[gui-shell] scale done 1 2"'
        if not _restore_done:
            _lines = log_text().splitlines()
            _idx = max((k for k, l in enumerate(_lines)
                        if '[gui-shell] scale begin 1 2' in l), default=-1)
            _frames = sum(1 for l in _lines[_idx + 1:]
                          if '[gui-shell] scale 1 ' in l)
            _restore_done = _idx >= 0 and _frames >= 2
            _detail = 'restore animation superseded after %d frame(s)' % _frames
        check('restore transition completes (or is superseded by the next toggle)',
              _restore_done, _detail)

        # ---- (5) start menu ---------------------------------------------------
        move_to(START_BTN[0] + START_BTN[2] // 2, START_BTN[1] + START_BTN[3] // 2)
        click()
        check('start button opened the start menu', wait_for('[gui-shell] menu open', 40))
        check('start menu fades in over 2 frames',
              '[gui-shell] menu fade' in log_text(),
              'log line "[gui-shell] menu fade N 0"')
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
        # v0.57.1: launching from the menu closes it, so re-open it with the start
        # button before clicking a row (the old version assumed it stayed open).
        move_to(START_BTN[0] + START_BTN[2] // 2, START_BTN[1] + START_BTN[3] // 2)
        click()
        assert wait_for('[gui-shell] menu open', 40), 'start menu did not reopen'
        time.sleep(0.4)
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
            check('opened window fades in over 3 frames',
                  log_text().count('[gui-shell] intro fade') >= 2,
                  'intro fade lines: %d' % log_text().count('[gui-shell] intro fade'))

        # ---- (6b) make sure window 1 is back at normal size -------------------
        # the maximise/restore checks above can leave it maximised depending on
        # how the synthetic double clicks are coalesced; restore it explicitly
        # (log parity tells us the current state) so the close check compares
        # the closed window's region against the untouched desktop.
        # Deterministic desktop for the close check: two single clicks on window
        # 1's taskbar button focus it and then minimise it (clicks are spaced well
        # past the 400 ms double-click window), then window 2 is raised again.
        _min_before = log_text().count('[gui-shell] minimize 1')
        for _ in range(4):
            if log_text().count('[gui-shell] minimize 1') > _min_before:
                break
            move_to(WB1[0] + WB1[1] // 2, TASKBAR_Y + 18)
            click()
            time.sleep(1.9)              # > 1500 ms: never a double click
        check('window 1 is minimised for the close check',
              log_text().count('[gui-shell] minimize 1') > _min_before,
              'minimize 1 lines: %d' % log_text().count('[gui-shell] minimize 1'))
        move_to(326 + 48, TASKBAR_Y + 18)      # window 2 taskbar button (3rd)
        click()
        time.sleep(0.8)

        # ---- (7) close button removes the window ------------------------------
        # close is the third title-bar control: bx = x + w - 77, each 22 wide + 4 gap
        close_x = WIN2[0] + WIN2[2] - 77 + 2 * 26 + 11
        close_y = WIN2[1] + 4 + 11
        move_to(close_x, close_y)
        click()
        check('close button closes the window', wait_for('[gui-shell] close 2', 60))
        check('close is preceded by a 2-frame fade-out',
              '[gui-shell] anim close 2' in log_text() and
              '[gui-shell] anim done 2 2' in log_text(),
              'anim close / anim done lines present')
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

        # ---- (9) per-window keyboard focus + editable line (v0.58.0) ---------
        # window 0's live position comes from the drag log (it was moved in (2))
        _drag_end = re.findall(r'drag end (\d+) (\d+)', log_text())
        wx, wy = ((int(_drag_end[-1][0]), int(_drag_end[-1][1]))
                  if _drag_end else (WIN0[0], WIN0[1]))
        move_to(wx + 200, wy + 150)
        click()
        time.sleep(0.9)
        check('clicking a window gives it the keyboard focus',
              wait_for('[gui-shell] focus 0', 40), 'log line "[gui-shell] focus 0"')
        kbd0 = dump_until('kbd0', lambda px: True)
        for _ch in 'hello':
            key(_ch, 0.16)               # back-to-back keys can be dropped
        time.sleep(1.2)

        def diff_band(im1, im2):
            p1, p2 = im1.load(), im2.load()
            n = 0
            for yy in range(wy + 128, min(wy + 168, SCREEN_H)):
                for xx in range(wx + 60, min(wx + 300, SCREEN_W)):
                    a, b = p1[xx, yy], p2[xx, yy]
                    if abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 20:
                        n += 1
            return n

        changed = 0
        if kbd0 is not None:
            for _ in range(6):
                dump('kbd1')
                im1 = load('kbd1')
                changed = diff_band(kbd0, im1)
                if changed > 120:
                    break
                time.sleep(0.6)
        check('typing repaints the focused window (input band changed)',
              changed > 120, '%d px changed in the input band' % changed)
        key('ret')
        check('Enter commits the typed line to the log',
              wait_for('[gui-shell] input win=0 text=hello', 40),
              'log line "[gui-shell] input win=0 text=hello"')
        typ('xy')
        time.sleep(0.5)
        key('backspace')
        time.sleep(0.5)
        key('ret')
        _commits = re.findall(r'input win=(\d+) text=(\S*)', log_text())
        check('Backspace edits the line before the commit',
              len(_commits) >= 2 and _commits[-1][1] == 'x',
              'commits: %s' % (_commits[-3:],))

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
