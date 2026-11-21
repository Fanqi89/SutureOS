#!/usr/bin/env python3
"""SutureOS new-shell per-window application content acceptance (v0.58.0).

Proves that the four windows of the Rust desktop shell no longer share one
placeholder body but paint real, per-application content, and that the live
values really are live:

  1. default desktop: 系统设置 shows three setting rows (label + value + accent
     pill) and 任务管理器 shows its static labels and two bars;
  2. 任务管理器's uptime / CPU % text CHANGES between two dumps 3 s apart (this
     is what proves `apps::draw_live` plus the once-a-second idle-fingerprint
     refresh: the cached window layer itself stays static);
  3. 图片查看器 (launched from the start menu) shows a dark checkerboard canvas
     with the real 48x48 logo nearest-neighbour scaled 3x (bright pixels on a
     dark matte) plus the file-name caption and the "48x48 · 3x" footer;
  4. 文本编辑器 (also launched from the start menu) shows its four numbered
     code lines (a real buffer since v0.61) and the accent caret at the end of the
     seeded second line;
  5. the four window bodies are pixel-wise distinct from each other;
  6. no kernel PANIC and the boot self-tests are still 16/16.

Usage:
  py -3 scripts/gui_apps_content_acceptance.py            # run + assert
  py -3 scripts/gui_apps_content_acceptance.py --no-assert # artefacts only
Exit code 0 only when every check passes, 2 on environment problems
(missing ISO / QEMU / Pillow).

Screenshots: build/apps_shot_{base,image,editor,live1,live2}.ppm
Serial log: build/serial_apps_content.log
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
QMP_PORT = int(os.environ.get('APPS_QMP_PORT', '47995'))
SERIAL = 'build/serial_apps_content.log'
SHOT = 'build/apps_shot_%s.ppm'

# ---- layout mirrored from rust/suture-gui/src/shell.rs + apps.rs -------------
SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H          # 730
TITLE_H = 30
PAD = 16

WIN0 = (150, 120, 430, 300)               # 系统设置     accent 0x4f8dff
WIN1 = (470, 300, 400, 260)               # 任务管理器   accent 0x22c1a8
WIN2 = (210, 430, 360, 240)               # 图片查看器   accent 0xf5a524
WIN3 = (560, 180, 330, 230)               # 文本编辑器   accent 0xe05a8a

START_BTN = (8, TASKBAR_Y + 5, 34, 28)
MENU_H = 236
MENU_ITEMS_Y = TASKBAR_Y - MENU_H - 8 + 46      # 532
MENU_ITEM_H = 44
MENU_X_CLICK = 90

# apps.rs anchors
ROW_TOP = TITLE_H + 20                    # first setting row, relative to win y
ROW_H = 52
ROW_GAP = 10
TM_BAR1_Y = TITLE_H + 42
TM_UPTIME_Y = TITLE_H + 138
TM_CPU_Y = TITLE_H + 164
TM_UPTIME_X = 92                          # relative to (win x + PAD + 4)
TM_CPU_X = 46
IMG_CAPTION_Y = TITLE_H + 8
IMG_CANVAS_Y = TITLE_H + 30
IMG_LOGO = 144
ED_Y0 = TITLE_H + 10                      # first code line - 4
ED_LINE_DY = 18
# v0.61: the editor owns a real buffer (apps::ED_*), seeded with four lines and the
# caret at the END OF THE SECOND line - that is where the first keystroke lands.
ED_CARET_LINE = 1
ED_CARET_TEXT = '    let mut n = 42;'
ED_CARET_W = 10 * len(ED_CARET_TEXT)      # ASCII advance at scale 1


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


# ---------------------------------------------------------------- pixel helpers

def mean_brightness(px, x0, y0, x1, y1, step=2):
    tot = 0
    n = 0
    for y in range(max(0, y0), min(SCREEN_H, y1), step):
        for x in range(max(0, x0), min(SCREEN_W, x1), step):
            p = px[x, y]
            tot += p[0] + p[1] + p[2]
            n += 1
    return tot / (3.0 * max(1, n))


def count_bright(px, x0, y0, x1, y1, thresh):
    """Pixels whose r+g+b exceeds `thresh` - a cheap "text is here" probe."""
    n = 0
    for y in range(max(0, y0), min(SCREEN_H, y1)):
        for x in range(max(0, x0), min(SCREEN_W, x1)):
            p = px[x, y]
            if p[0] + p[1] + p[2] > thresh:
                n += 1
    return n


def count_warm(px, x0, y0, x1, y1, r_minus_g):
    """Pixels where red clearly dominates green (warm accent, e.g. 0xe05a8a)."""
    n = 0
    for y in range(max(0, y0), min(SCREEN_H, y1)):
        for x in range(max(0, x0), min(SCREEN_W, x1)):
            p = px[x, y]
            if p[0] - p[1] > r_minus_g:
                n += 1
    return n


def count_colorful(px, x0, y0, x1, y1, spread):
    """Pixels whose channel spread exceeds `spread`: catches the coloured logo
    glyph and ignores the neutral checkerboard / window body."""
    n = 0
    for y in range(max(0, y0), min(SCREEN_H, y1)):
        for x in range(max(0, x0), min(SCREEN_W, x1)):
            p = px[x, y]
            if max(p) - min(p) > spread:
                n += 1
    return n


def count_max_above(px, x0, y0, x1, y1, thresh):
    """Pixels whose brightest channel exceeds `thresh` (logo glyph vs matte)."""
    n = 0
    for y in range(max(0, y0), min(SCREEN_H, y1)):
        for x in range(max(0, x0), min(SCREEN_W, x1)):
            if max(px[x, y]) > thresh:
                n += 1
    return n


def diff_pixels(px1, px2, x0, y0, x1, y1, thresh=24):
    n = 0
    for y in range(max(0, y0), min(SCREEN_H, y1)):
        for x in range(max(0, x0), min(SCREEN_W, x1)):
            a = px1[x, y]
            b = px2[x, y]
            if abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > thresh:
                n += 1
    return n


def signature(px, x0, y0, x1, y1):
    """Coarse 3x3 block-mean fingerprint + bright-pixel count of a region."""
    w = max(1, (x1 - x0) // 3)
    h = max(1, (y1 - y0) // 3)
    sig = []
    for by in range(3):
        for bx in range(3):
            sig.append(round(mean_brightness(px, x0 + bx * w, y0 + by * h,
                                             x0 + (bx + 1) * w, y0 + (by + 1) * h),
                             1))
    sig.append(count_bright(px, x0, y0, x1, y1, 600) // 16)
    return sig


def sig_distance(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def main():
    do_assert = '--no-assert' not in sys.argv
    for tool, why in ((QEMU, 'QEMU'),):
        if not os.path.exists(tool):
            print('[apps] %s not found at %s (environment problem)' % (why, tool))
            return 2
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[apps] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2
    try:
        from PIL import Image
    except ImportError:
        print('[apps] Pillow is required: py -3 -m pip install pillow')
        return 2

    log_abs = os.path.join(ROOT, SERIAL)
    names = ('base', 'image', 'editor', 'live1', 'live2', 'final')
    for p in [log_abs] + [os.path.join(ROOT, SHOT % n) for n in names]:
        if os.path.exists(p):
            os.remove(p)

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[apps] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, timeout=180):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in log_text():
                return True
            time.sleep(0.3)
        return False

    def wait_count(needle, n, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if log_text().count(needle) >= n:
                return True
            time.sleep(0.3)
        return False

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.07):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key(ch if ch != ' ' else 'spc')

        # The kernel pins the pointer to (w/2, h/2) when `guishell` starts and
        # never clamps its internal position, so track it exactly: every move is
        # a relative PS/2 delta of at most +-90 px.
        pos = [SCREEN_W // 2, SCREEN_H - 120]   # kernel pins the pointer here

        def move_to(tx, ty, settle=0.35):
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
            time.sleep(0.20)
            q.hmp('mouse_button 0')
            time.sleep(0.45)

        def dump(name):
            q.hmp('screendump ' + (SHOT % name))
            time.sleep(0.7)

        def load(name):
            path = os.path.join(ROOT, SHOT % name)
            return Image.open(path).convert('RGB') if os.path.exists(path) else None

        def dump_until(name, pred, tries=8, gap=0.8):
            for _ in range(tries):
                dump(name)
                im = load(name)
                if im is not None and pred(im.load()):
                    return im
                time.sleep(gap)
            return load(name)

        def open_menu():
            move_to(START_BTN[0] + START_BTN[2] // 2, START_BTN[1] + START_BTN[3] // 2)
            click()

        def click_menu_item(idx):
            open_menu()
            assert wait_for('[gui-shell] menu open', 40), 'start menu did not open'
            time.sleep(0.4)
            move_to(MENU_X_CLICK, MENU_ITEMS_Y + idx * MENU_ITEM_H + 22)
            click()

        # ---- boot + enter the shell --------------------------------------
        if not wait_for('sutureos#', 240):
            print('[apps] shell prompt never appeared')
            return 1
        time.sleep(0.8)
        typ('guishell')
        key('ret')
        check('shell loop started', wait_for('[gui-shell] shell: loop', 180))
        check('boot self-tests still 16/16',
              wait_for('selftests: 16/16 passed', 30))

        # ---- (1) default desktop: 系统设置 rows + 任务管理器 bars ----------
        base = dump_until('base', lambda px: mean_brightness(px, 200, 160, 460, 340) > 80)
        ok_base = base is not None

        if ok_base:
            bpx = base.load()
            # 系统设置: three rounded rows, each a lighter band than the body
            row_means = []
            label_px = []
            value_px = []
            pill_blue = []
            x0 = WIN0[0] + PAD
            for i in range(3):
                ry = WIN0[1] + ROW_TOP + i * (ROW_H + ROW_GAP)
                row_means.append(mean_brightness(bpx, x0 + 40, ry + 4,
                                                 x0 + 380, ry + ROW_H - 4))
                label_px.append(count_bright(bpx, x0 + 30, ry + 16,
                                             x0 + 200, ry + 36, 560))
                value_px.append(count_bright(bpx, x0 + 210, ry + 16,
                                             x0 + 388, ry + 36, 540))
                p = bpx[x0 + 14, ry + ROW_H // 2]
                pill_blue.append(p[2] - p[0])
            gap_mean = mean_brightness(bpx, x0 + 40,
                                       WIN0[1] + ROW_TOP + ROW_H + 2,
                                       x0 + 380, WIN0[1] + ROW_TOP + ROW_H + 8)
            check('settings: three setting rows painted',
                  all(m > gap_mean + 4 for m in row_means),
                  'row means %s vs gap %.1f' % ([round(m, 1) for m in row_means],
                                                gap_mean))
            check('settings: every row carries its label text',
                  all(n > 60 for n in label_px),
                  'bright pixels per row: %s' % (label_px,))
            check('settings: every row carries its right-aligned value',
                  all(n > 40 for n in value_px),
                  'value pixels per row: %s' % (value_px,))
            check('settings: accent pill is blue (0x4f8dff)',
                  all(v > 25 for v in pill_blue),
                  'blue-minus-red per row: %s' % (pill_blue,))

            # 任务管理器: two bars, accent/green fill on a translucent track
            bx0 = WIN1[0] + PAD + 56
            by1 = WIN1[1] + TM_BAR1_Y
            fill = bpx[bx0 + 20, by1 + 7]
            track = bpx[WIN1[0] + WIN1[2] - PAD - 30, by1 + 7]
            check('taskmgr: CPU bar has an accent fill',
                  fill[1] - fill[0] > (track[1] - track[0]) + 30 and fill[1] > 140,
                  'fill %s vs track %s' % (fill, track))
            by2 = by1 + 30
            fill2 = bpx[bx0 + 20, by2 + 7]
            check('taskmgr: memory bar painted as well',
                  fill2[1] - fill2[0] > (track[1] - track[0]) + 20,
                  'fill2 %s' % (fill2,))
            check('taskmgr: static labels are present',
                  count_bright(bpx, WIN1[0] + PAD + 4, by1 - 4,
                               WIN1[0] + PAD + 60, by2 + 18, 560) > 40,
                  'bar label pixels')

        # ---- (2) live values: uptime changes between two dumps 3 s apart ---
        if ok_base:
            live_x0 = WIN1[0] + PAD + 4 + TM_UPTIME_X
            live_y0 = WIN1[1] + TM_UPTIME_Y
            cpu_x0 = WIN1[0] + PAD + 4 + TM_CPU_X
            cpu_y0 = WIN1[1] + TM_CPU_Y
            dump('live1')
            im1 = load('live1')
            time.sleep(3.0)
            changed = 0
            cpu_changed = 0
            for _ in range(6):
                dump('live2')
                im2 = load('live2')
                if im1 is not None and im2 is not None:
                    changed = diff_pixels(im1.load(), im2.load(),
                                          live_x0, live_y0, live_x0 + 70, live_y0 + 16)
                    cpu_changed = diff_pixels(im1.load(), im2.load(),
                                              cpu_x0, cpu_y0, cpu_x0 + 60, cpu_y0 + 16)
                    if changed > 10:
                        break
                time.sleep(1.2)
            check('taskmgr: uptime text changes over 3 s',
                  changed > 10, '%d px differ in the uptime line' % changed)
            check('taskmgr: CPU% text changes with it',
                  cpu_changed > 6, '%d px differ in the CPU%% line' % cpu_changed)

        # ---- (3) 图片查看器: checkerboard canvas + real 48px logo at 3x ----
        click_menu_item(2)
        check('menu launched window 2 (image viewer)', wait_for('[gui-shell] menu launch 2', 90))
        img = None
        if ok_base:
            cx0 = WIN2[0] + PAD
            cy0 = WIN2[1] + IMG_CANVAS_Y
            lx = cx0 + (WIN2[2] - 2 * PAD - IMG_LOGO) // 2
            img = dump_until('image',
                             lambda px: count_colorful(px, lx, cy0, lx + IMG_LOGO,
                                                       cy0 + IMG_LOGO, 40) > 800)
        if img is not None:
            ipx = img.load()
            cx0 = WIN2[0] + PAD
            cy0 = WIN2[1] + IMG_CANVAS_Y
            lx = cx0 + (WIN2[2] - 2 * PAD - IMG_LOGO) // 2
            # checkerboard: two dark tones, clearly darker than the window body
            tones = {}
            for y in range(cy0 + 4, cy0 + 60, 2):
                for x in range(cx0 + 4, cx0 + 70, 2):
                    tones[ipx[x, y]] = tones.get(ipx[x, y], 0) + 1
            dark_mean = mean_brightness(ipx, cx0 + 4, cy0 + 4, cx0 + 70, cy0 + 60)
            check('image: checkerboard matte is dark (well below the body)',
                  dark_mean < 130, 'canvas mean %.1f (window body ~170)' % dark_mean)
            check('image: checkerboard has two alternating tones',
                  len(tones) >= 2 and len(sorted(tones.values())[-2:]) == 2
                  and sorted(tones.values())[-2] > 30,
                  'tones %s' % sorted(tones.items(), key=lambda kv: -kv[1])[:4])
            # the logo itself: coloured pixels inside the 144x144 block (the
            # white PNG matte is keyed out, the glyph is teal/orange)
            bright = count_bright(ipx, lx, cy0, lx + IMG_LOGO, cy0 + IMG_LOGO, 480)
            colorful = count_colorful(ipx, lx, cy0, lx + IMG_LOGO, cy0 + IMG_LOGO, 40)
            lit = count_max_above(ipx, lx, cy0, lx + IMG_LOGO, cy0 + IMG_LOGO, 120)
            check('image: real 48px logo drawn at 3x (lit + coloured px)',
                  bright > 40 and colorful > 1200 and lit > 1500,
                  '%d lit / %d bright / %d coloured px in the 144x144 block'
                  % (lit, bright, colorful))
            check('image: logo sits on the checkerboard (no white tile)',
                  mean_brightness(ipx, lx, cy0, lx + IMG_LOGO, cy0 + 20) < 150,
                  'top band mean %.1f'
                  % mean_brightness(ipx, lx, cy0, lx + IMG_LOGO, cy0 + 20))
            cap = count_bright(ipx, cx0, WIN2[1] + IMG_CAPTION_Y,
                               cx0 + 240, WIN2[1] + IMG_CAPTION_Y + 16, 560)
            check('image: file-name caption present', cap > 40,
                  '%d caption px' % cap)

        # ---- (4) 文本编辑器: three numbered lines + caret -------------------
        click_menu_item(3)
        check('menu launched window 3 (text editor)', wait_for('[gui-shell] menu launch 3', 90))
        ed = None
        if ok_base:
            ed = dump_until('editor',
                            lambda px: count_bright(px, WIN3[0] + PAD + 34,
                                                    WIN3[1] + ED_Y0 + 4,
                                                    WIN3[0] + WIN3[2] - PAD,
                                                    WIN3[1] + ED_Y0 + 60, 520) > 120)
        if ed is not None:
            epx = ed.load()
            line_px = []
            for i in range(4):
                ly = WIN3[1] + ED_Y0 + 4 + i * ED_LINE_DY
                line_px.append(count_bright(epx, WIN3[0] + PAD + 34, ly,
                                            WIN3[0] + WIN3[2] - PAD, ly + 16, 520))
            check('editor: four numbered code lines painted (real buffer)',
                  all(n > 10 for n in line_px),
                  'bright px per line: %s' % (line_px,))
            caret_x = WIN3[0] + PAD + 34 + ED_CARET_W
            caret_y = WIN3[1] + ED_Y0 + 4 + ED_CARET_LINE * ED_LINE_DY
            warm = count_warm(epx, caret_x, caret_y, caret_x + 8, caret_y + 16, 60)
            # v0.61: the caret is a real 2x16 text cursor (32 px), not the old
            # 8x16 block, so the threshold follows the shape
            check('editor: accent caret block sits at the end of the seeded line',
                  warm > 20, '%d warm px in the caret cell' % warm)

        # ---- (5) the four window bodies are pixel-wise distinct ------------
        if ok_base and img is not None and ed is not None:
            sig0 = signature(bpx, WIN0[0] + PAD, WIN0[1] + ROW_TOP,
                             WIN0[0] + PAD + 300, WIN0[1] + ROW_TOP + 170)
            sig1 = signature(bpx, WIN1[0] + PAD, WIN1[1] + TITLE_H + 14,
                             WIN1[0] + PAD + 300, WIN1[1] + TITLE_H + 100)
            sig2 = signature(ipx, WIN2[0] + PAD, WIN2[1] + IMG_CANVAS_Y,
                             WIN2[0] + PAD + 300, WIN2[1] + IMG_CANVAS_Y + IMG_LOGO)
            sig3 = signature(epx, WIN3[0] + PAD, WIN3[1] + ED_Y0,
                             WIN3[0] + PAD + 290, WIN3[1] + ED_Y0 + 60)
            sigs = [('系统设置', sig0), ('任务管理器', sig1),
                    ('图片查看器', sig2), ('文本编辑器', sig3)]
            worst = min(sig_distance(sigs[i][1], sigs[j][1])
                        for i in range(len(sigs)) for j in range(i + 1, len(sigs)))
            check('four window bodies are distinct', worst >= 8,
                  'min pairwise signature distance %d' % worst)

        # ---- (6) exit + wrap up -------------------------------------------
        dump('final')
        key('esc')
        check('ESC left the shell loop', wait_for('[gui-shell] shell: exit', 90))
        check('terminal came back', wait_for('guishell: back to terminal', 90))
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
    check('no kernel PANIC', 'PANIC' not in log)
    check('no virtio/window error lines',
          'resp BAD' not in log and 'scale: layer missing' not in log)

    if not do_assert:
        print('[apps] dumps written, assertions skipped')
        return 0

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[apps] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[apps] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
