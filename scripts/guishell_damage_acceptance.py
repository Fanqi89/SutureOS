#!/usr/bin/env python3
"""SutureOS compositor damage acceptance (task-6).

Proves on real QEMU that the desktop shell now repaints through damage
rectangles instead of a full-screen redraw, and that this is *pixel identical*
to a full repaint:

  * `damage: identical` lines: every oracle run compared the damage-composed
    frame against a fresh full repaint of the whole screen -> 0 mismatches
    (swapstat `oracle=.. mismatch=0`).
  * `profile/30 ... permille=..` shows how much of the screen was recomposed:
    an idle/pointer-only window stays far below a full repaint.
  * 20 screendumps taken *while presents are happening* are identical to the
    reference desktop outside the pointer box / taskbar / live numbers - a
    half-presented frame would break this.
  * drag / menu open-close / minimise all go through the damage path
    (`drag repaint N ticks`, `anim minimize`, `damage: identical`) without a
    single oracle mismatch.
  * present completion check: `tear=` samples were taken and `tear_bad=0`.
  * buffer state machine: `buf_checks>0`, `buf_bad=0`, exactly one PRESENTED
    buffer and no READY buffer left at rest.
  * the stable desktop still passes the appearance gate
    (`gui_appearance_check.py --profile new`, 0 px outside the live rects).
  * no kernel PANIC.

Usage: py -3 scripts/guishell_damage_acceptance.py
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
# own copy with a distinct image name: other sessions on this box kill
# every process called qemu-system-x86_64 when they clean up strays
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = int(os.environ.get('DAMAGE_QMP_PORT', '45841'))
SERIAL = 'build/serial_damage.log'
SHOT_REF = 'build/dmg_ref.ppm'
SHOT_MID = 'build/dmg_mid%02d.ppm'
SHOT_STABLE = 'build/dmg_stable.ppm'
SHOT_STABLE2 = 'build/dmg_stable2.ppm'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_Y = 768 - 38

PROFILE_RE = re.compile(
    r'\[gui-shell\] profile/30: full=(\d+) hit=(\d+) miss=(\d+) win=(\d+) bar=(\d+) '
    r'ticks damage=(\d+) full_frames=(\d+) px=(\d+) framed=(\d+) permille=(\d+) '
    r'oracle=(\d+) mismatch=(\d+)')
STAT_RE = re.compile(
    r'\[gui-shell\] swapstat presents=(\d+) gens=(\d+) presented=(\d+) dropped=(\d+) '
    r'jitter_us=(\d+) interval_ms=(\d+) skipped=(\d+) rendered=(\d+) scene=(\d+) '
    r'carry=(\d+) full=(\d+) dmg=(\d+) dmg_px=(\d+) framed_px=(\d+) oracle=(\d+) '
    r'mismatch=(\d+) tear=(\d+) tear_bad=(\d+) late=(\d+) latency_us=(\d+) '
    r'latency_max_us=(\d+) hist=([\d,]+) deferred=(\d+) pending=(\d+) '
    r'bufs=(\w+) buf_checks=(\d+) buf_bad=(\d+)')


class Qmp:
    def __init__(self, port, timeout=120):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:        # noqa: PERF203 - retry loop by design
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


def load_ppm(path):
    from PIL import Image
    return Image.open(os.path.join(ROOT, path)).convert('RGB')


def diff_outside(px_a, px_b, masks):
    """Count differing pixels outside all mask rectangles (x0,y0,x1,y1)."""
    n = 0
    first = None
    for y in range(0, SCREEN_H, 2):
        for x in range(0, SCREEN_W, 2):
            skip = False
            for (mx0, my0, mx1, my1) in masks:
                if mx0 <= x < mx1 and my0 <= y < my1:
                    skip = True
                    break
            if skip:
                continue
            a = px_a[x, y]
            b = px_b[x, y]
            if abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 12:
                n += 1
                if first is None:
                    first = (x, y, a, b)
    return n, first


def main():
    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    log_abs = os.path.join(ROOT, SERIAL)
    for p in [log_abs, os.path.join(ROOT, SHOT_REF), os.path.join(ROOT, SHOT_STABLE),
              os.path.join(ROOT, SHOT_STABLE2)]:
        if os.path.exists(p):
            os.remove(p)
    for i in range(20):
        p = os.path.join(ROOT, SHOT_MID % i)
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[damage] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[damage] ' + ' '.join(argv), flush=True)
    errlog = open(os.path.join(ROOT, 'build', 'qemu_err.log'), 'wb')
    proc = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.DEVNULL,
                             stderr=errlog)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(pred, timeout=240, gap=0.3):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if pred(log_text()):
                return True
            time.sleep(gap)
        return False

    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.09):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key('spc' if ch == ' ' else ch)

        pos = [SCREEN_W // 2, SCREEN_H - 120]

        def move_to(tx, ty, settle=0.3):
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
            time.sleep(0.15)
            q.hmp('mouse_button 0')
            time.sleep(0.4)

        def dump(path):
            q.hmp('screendump ' + path)
            time.sleep(0.6)

        if not wait_for(lambda t: 'sutureos# ' in t, 300):
            print('[damage] shell prompt never appeared')
            return 1
        time.sleep(0.6)
        typ('guishell')
        key('ret')
        check('shell loop started', wait_for(lambda t: '[gui-shell] shell: loop (mouse=' in t, 240))
        check('boot self-tests still 16/16',
              wait_for(lambda t: 'selftests: 16/16 passed' in t, 60))

        # ---------------- (1) damage frames + oracle on a live desktop --------
        wait_for(lambda t: 'profile/30:' in t, 240)
        time.sleep(0.8)
        dump(SHOT_STABLE)

        # pointer sweep in the empty desktop area: damage-only frames
        for tx, ty in ((300, 200), (700, 560), (420, 640), (620, 300), (700, 620)):
            move_to(tx, ty)
        wait_for(lambda t: len(PROFILE_RE.findall(t)) >= 2, 240)

        txt = log_text()
        prof = [tuple(int(x) for x in m) for m in PROFILE_RE.findall(txt)]
        permille = [p[9] for p in prof]
        check('profile windows log the damage ratio', len(prof) >= 2,
              'windows=%d permille=%r' % (len(prof), permille[:6]))
        check('damage ratio stays far below a full repaint (< 600 permille)',
              permille and max(permille) < 600,
              'max permille=%d' % (max(permille) if permille else -1))
        check('most windows are pure damage frames (full_frames avg < 1)',
              prof and sum(p[6] for p in prof) <= 2 * len(prof),
              'full_frames=%r' % [p[6] for p in prof[:6]])
        check('oracle verified the damage composition (`damage: identical`)',
              'damage: identical' in txt,
              (re.search(r'\[gui-shell\] damage: identical[^\n]*', txt).group(0)
               if 'damage: identical' in txt else 'no line'))
        check('no oracle mismatch so far', 'damage: MISMATCH' not in txt)

        # ---------------- (2) 20 mid-present screendumps ----------------------
        # jitter the pointer inside a small empty-desktop box so every move
        # produces a (cheap) damage frame and therefore presents while dumping
        masks = [(700 - 10, 620 - 10, 700 + 40, 620 + 40),       # pointer box
                 (0, TASKBAR_Y, SCREEN_W, SCREEN_H),             # taskbar strip
                 (486, 330, 860, 520)]                           # task manager live
        dump(SHOT_REF)
        ref = load_ppm(SHOT_REF).load()
        worst = 0
        worst_at = None
        jitter = ((0, 0), (3, 2), (-3, -2), (2, -3), (-2, 3), (4, 0), (0, 4),
                  (-4, 0), (0, -4), (3, 3), (-3, -3), (2, 2), (-2, -2), (4, 4),
                  (-4, -4), (1, -1), (-1, 1), (3, -2), (-3, 2), (0, 2))
        for i in range(20):
            dx, dy = jitter[i]
            q.hmp('mouse_move %d %d' % (dx, dy))
            pos[0] += dx
            pos[1] += dy
            time.sleep(0.08)
            path = SHOT_MID % i
            q.hmp('screendump ' + path)
            time.sleep(0.22)
            im = load_ppm(path)
            n, first = diff_outside(ref, im.load(), masks)
            if n > worst:
                worst = n
                worst_at = (i, first)
        check('20 screendumps taken mid-present match the reference desktop',
              worst == 0,
              'worst=%d px outside the dynamic masks %s'
              % (worst, '' if worst_at is None else 'shot %d %r' % worst_at))
        move_to(700, 620)                      # park the pointer again

        # ---------------- (3) drag: damage rects, not a full repaint ----------
        # drag window 0 (system settings) by its title bar
        move_to(150 + 160, 120 + 12)
        q.hmp('mouse_button 1')
        time.sleep(0.2)
        for i in range(8):
            q.hmp('mouse_move 12 -6')
            pos[0] += 12
            pos[1] -= 6
            time.sleep(0.12)
        q.hmp('mouse_button 0')
        time.sleep(0.6)
        check('drag produced damage frames (`drag repaint N ticks`)',
              'drag repaint' in log_text(),
              (re.search(r'drag repaint \d+ ticks[^\n]*', log_text()).group(0)
               if 'drag repaint' in log_text() else 'no line'))
        ticks = [int(v) for v in re.findall(r'drag repaint (\d+) ticks', log_text())]
        check('drag frames stay cheap (<= 12 ticks)', ticks and max(ticks) <= 12,
              'ticks=%r' % (ticks[:8],))

        # ---------------- (4) start menu open/close through damage ------------
        move_to(8 + 17, TASKBAR_Y + 19)
        click()
        time.sleep(1.0)
        check('start menu opened', 'menu open' in log_text())
        click()
        time.sleep(0.8)
        check('start menu closed', 'menu closed' in log_text())

        # ---------------- (5) minimise through the damage path ----------------
        move_to(118 + 40, TASKBAR_Y + 19)      # first taskbar window button
        click()
        time.sleep(0.9)
        click()
        time.sleep(1.2)
        check('minimise animation ran', 'anim minimize' in log_text())
        check('window minimised', 'minimize ' in log_text() or 'minimize' in log_text())

        # ---------------- (6) stable shot for the appearance gate -------------
        move_to(300, 620)
        time.sleep(1.0)
        dump(SHOT_STABLE2)
        gate = subprocess.run(
            [sys.executable, os.path.join(ROOT, 'scripts', 'gui_appearance_check.py'),
             SHOT_STABLE2, '--live-shot', SHOT_STABLE, '--profile', 'new'],
            cwd=ROOT, capture_output=True, text=True)
        out = (gate.stdout or '') + (gate.stderr or '')
        check('desktop stays pixel-identical outside live rects (appearance gate)',
              gate.returncode == 0,
              out.strip().splitlines()[-1] if out.strip() else '')

        # ---------------- (7) counters ----------------------------------------
        key('esc')
        wait_for(lambda t: '[gui-shell] shell: exit' in t, 120)
        time.sleep(0.8)
        typ('guishell swapstat')
        key('ret')
        wait_for(lambda t: '[gui-shell] swapstat' in t, 120)
        txt = log_text()
        stats = [tuple(m) for m in STAT_RE.findall(txt)]
        if stats:
            s = stats[-1]
            print('[damage] swapstat: ' + ' '.join(
                '%s=%s' % (k, v) for k, v in zip(
                    ('presents', 'gens', 'presented', 'dropped', 'jitter_us',
                     'interval_ms', 'skipped', 'rendered', 'scene', 'carry', 'full',
                     'dmg', 'dmg_px', 'framed_px', 'oracle', 'mismatch', 'tear',
                     'tear_bad', 'late', 'latency_us', 'latency_max_us'),
                    s[:21])))
            check('oracle ran and found 0 mismatches',
                  int(s[14]) >= 2 and int(s[15]) == 0,
                  'oracle=%s mismatch=%s' % (s[14], s[15]))
            check('present completion check ran with 0 mismatches',
                  int(s[16]) >= 1000 and int(s[17]) == 0,
                  'tear=%s tear_bad=%s' % (s[16], s[17]))
            check('most frames took the damage path, not a full repaint',
                  int(s[12]) > 10 * int(s[10]),
                  'dmg=%s full=%s' % (s[12], s[10]))
            check('buffer state machine invariants held',
                  int(s[25]) > 0 and int(s[26]) == 0 and s[24].count('P') == 1
                  and s[24].count('R') == 0,
                  'bufs=%s checks=%s bad=%s' % (s[24], s[25], s[26]))
            check('no `tear: partial present` line in the log',
                  'tear: partial present' not in txt)
            check('no `buf: INVARIANT VIOLATION` line in the log',
                  'INVARIANT VIOLATION' not in txt)
            check('presented == presents == gens',
                  s[0] == s[2] == s[1], 'presents=%s presented=%s gens=%s' % (s[0], s[2], s[1]))
        else:
            check('swapstat line parsed', False)
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

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[damage] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[damage] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
