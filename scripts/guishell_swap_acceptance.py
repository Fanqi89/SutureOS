#!/usr/bin/env python3
"""SutureOS present/swap-chain acceptance (task-4).

Proves, with real QEMU input and real screenshots, that the Rust desktop shell
now presents through an explicit double-buffered swap chain with a vsync gate,
and that this changed no pixels:

  * `guishell` still starts the same loop              -> "[gui-shell] shell: loop"
  * the pointer moves through the cursor fast path     -> "cursor: fast path frames="
  * clicks / minimise-restore generate full frames     -> "profile/30:"
  * every 30 frames the swap counters are logged       -> "[gui-shell] swap presents=N gens=G skipped=S interval_ms=M"
      - presents == gens on every line (one generation per present)
      - gens / presents never go backwards (monotonic)
      - skipped >= 0 and never decreases
      - interval_ms > 0 (the 100 Hz tick is the only clock)
  * after ESC, `guishell swapstat` reports the cumulative counters once
      - presents > 0 and rendered == scene + skipped (accounting closes)
  * two screendumps of the desktop pass the appearance gate
    (`gui_appearance_check.py --profile new`) -> 0 px outside live rects
  * no kernel PANIC

Usage: py -3 scripts/guishell_swap_acceptance.py
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
QMP_PORT = int(os.environ.get('SWAP_QMP_PORT', '45921'))
SERIAL = 'build/serial_swap.log'
SHOT_A = 'build/swap_shot1.ppm'
SHOT_B = 'build/swap_shot2.ppm'

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 38
TASKBAR_Y = SCREEN_H - TASKBAR_H
WB0 = (118, 96)                     # first taskbar window button (x, w)


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


SWAP_RE = re.compile(
    r'\[gui-shell\] swap presents=(\d+) gens=(\d+) skipped=(\d+) interval_ms=(\d+)')
STAT_RE = re.compile(
    r'\[gui-shell\] swapstat presents=(\d+) gens=(\d+) skipped=(\d+) '
    r'interval_ms=(\d+) rendered=(\d+) scene=(\d+)')


def main():
    log_abs = os.path.join(ROOT, SERIAL)
    for p in [log_abs, os.path.join(ROOT, SHOT_A), os.path.join(ROOT, SHOT_B)]:
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[swap] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[swap] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(pred, timeout=180, gap=0.3):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if pred(log_text()):
                return True
            time.sleep(gap)
        return False

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    q = None
    try:
        q = Qmp(QMP_PORT)

        def key(k, d=0.08):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key('spc' if ch == ' ' else ch)

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

        def click():
            q.hmp('mouse_button 1')
            time.sleep(0.20)
            q.hmp('mouse_button 0')
            time.sleep(0.45)

        def dump(path):
            q.hmp('screendump ' + path)
            time.sleep(0.8)

        if not wait_for(lambda t: 'sutureos# ' in t, 180):
            print('[swap] shell prompt never appeared')
            return 1
        time.sleep(0.5)
        typ('guishell')
        key('ret')
        check('guishell still starts the same shell loop',
              wait_for(lambda t: '[gui-shell] shell: loop (mouse=' in t, 150))

        # --- frozen-desktop screenshots FIRST (the baseline is the untouched
        # desktop; any interaction before the gate would legitimately move it) ---
        wait_for(lambda t: 'profile/30:' in t, 180)
        time.sleep(1.0)
        dump(SHOT_A)
        time.sleep(1.2)
        dump(SHOT_B)
        gate = subprocess.run(
            [sys.executable, os.path.join(ROOT, 'scripts', 'gui_appearance_check.py'),
             SHOT_A, '--live-shot', SHOT_B, '--profile', 'new'],
            cwd=ROOT, capture_output=True, text=True)
        out = (gate.stdout or '') + (gate.stderr or '')
        check('desktop stays pixel-identical outside live rects (appearance gate)',
              gate.returncode == 0, out.strip().splitlines()[-1] if out.strip() else '')

        # --- generate frames: pointer sweep (cursor fast path) + menu open/close
        move_to(280, 200)
        move_to(760, 520)
        move_to(430, 610)
        click()                                   # empty-desktop click
        # the start button toggles the menu on every click and has no
        # double-click semantics (unlike the taskbar window buttons, which
        # maximise on a fast second click and would cover the whole desktop).
        start = (8 + 17, TASKBAR_Y + 19)
        move_to(start[0], start[1])
        time.sleep(1.8)
        click()                                   # start menu opens (full frame)
        time.sleep(1.8)
        click()                                   # start menu closes (full frame)
        move_to(620, 300)
        move_to(320, 420)

        # wait until at least three swap windows (90 frames) were logged
        wait_for(lambda t: len(SWAP_RE.findall(t)) >= 3, 240)

        txt = log_text()
        swap = [tuple(int(x) for x in m) for m in SWAP_RE.findall(txt)]
        stats = [tuple(int(x) for x in m) for m in STAT_RE.findall(txt)]

        check('swap log line appears (>=3 windows of 30 frames)',
              len(swap) >= 3, 'lines=%d' % len(swap))
        check('presents == gens on every swap line (one generation per present)',
              swap and all(p == g for p, g, s, i in swap),
              'first=%r last=%r' % (swap[0] if swap else None,
                                    swap[-1] if swap else None))
        check('presents strictly increases between windows',
              all(b[0] > a[0] for a, b in zip(swap, swap[1:])),
              'presents=%r' % [p for p, g, s, i in swap])
        check('gens is monotonically increasing',
              all(b[1] > a[1] for a, b in zip(swap, swap[1:])),
              'gens=%r' % [g for p, g, s, i in swap])
        check('skipped is >= 0 and never decreases',
              all(s >= 0 for p, g, s, i in swap) and
              all(b[2] >= a[2] for a, b in zip(swap, swap[1:])),
              'skipped=%r' % [s for p, g, s, i in swap])
        check('interval_ms > 0 (the 100 Hz tick is the only clock)',
              all(i > 0 for p, g, s, i in swap),
              'interval_ms=%r' % [i for p, g, s, i in swap])

        # present rate from the last logged window (30 frames)
        if swap:
            ivl = swap[-1][3]
            rate = 1000.0 / ivl if ivl else 0.0
            print('[swap] last window: presents=%d skipped=%d interval_ms=%d '
                  '(~%.1f presents/s)' % (swap[-1][0], swap[-1][2], ivl, rate))

        check('cursor fast path was exercised (pointer-only updates)',
              'cursor: fast path frames=' in txt)
        check('full-frame path was exercised (profile/30 windows)',
              'profile/30:' in txt)

        # --- leave the desktop, then read the counters back -------------------
        key('esc')
        check('ESC still leaves the shell',
              wait_for(lambda t: '[gui-shell] shell: exit' in t, 90), '')
        time.sleep(1.0)
        typ('guishell swapstat')
        key('ret')
        check('guishell swapstat logs the counters once',
              wait_for(lambda t: '[gui-shell] swapstat' in t, 90))
        stats = [tuple(int(x) for x in m) for m in STAT_RE.findall(log_text())]
        if stats:
            p, g, s, ivl, rendered, scene = stats[-1]
            print('[swap] swapstat: presents=%d gens=%d skipped=%d interval_ms=%d '
                  'rendered=%d scene=%d' % (p, g, s, ivl, rendered, scene))
            check('swapstat presents > 0 after a desktop session', p > 0)
            check('swapstat presents == gens', p == g)
            check('accounting closes: rendered == scene presents + skipped',
                  rendered == scene + s)
            if swap:
                check('cumulative counters never go backwards vs the live log',
                      p >= swap[-1][0] and s >= swap[-1][2])
        else:
            check('swapstat line parsed', False)

        # --- dedicated pointer-motion session: how many presents per second
        # does a pure cursor-fast-path session achieve? Two `guishell swapstat`
        # readings bracket the second session, and Python times the burst. ---
        stats_before = [tuple(int(x) for x in m) for m in STAT_RE.findall(log_text())]
        p_before = stats_before[-1][0] if stats_before else 0
        typ('guishell')
        key('ret')
        if wait_for(lambda t: t.count('[gui-shell] shell: loop (mouse=') >= 2, 120):
            time.sleep(1.0)
            t_motion0 = time.time()
            # continuous motion: 60 small steps, no clicks (pure cursor updates)
            for i in range(60):
                q.hmp('mouse_move %d %d' % (40 if (i // 15) % 2 == 0 else -40,
                                            25 if (i // 30) % 2 == 0 else -25))
                time.sleep(0.05)
            t_motion = time.time() - t_motion0
            time.sleep(0.5)
            key('esc')
            wait_for(lambda t: t.count('[gui-shell] shell: exit') >= 2, 90)
            time.sleep(0.8)
            typ('guishell swapstat')
            key('ret')
            wait_for(lambda t: '[gui-shell] swapstat' in t, 90)
            stats_after = [tuple(int(x) for x in m) for m in STAT_RE.findall(log_text())]
            p_after = stats_after[-1][0] if stats_after else 0
            dp = p_after - p_before
            rate = dp / t_motion if t_motion > 0 else 0.0
            print('[swap] pointer-motion session: %d presents in %.2f s = %.1f '
                  'presents/s (fast-path frames are gated at 1 per 10 ms tick)'
                  % (dp, t_motion, rate))
            check('pointer motion produces presents through the fast path',
                  dp >= 10, 'presents=%d in %.2fs' % (dp, t_motion))
            check('pointer-motion present rate is bounded by the 10 ms tick',
                  0 < rate <= 100.0, '%.1f presents/s' % rate)

        check('no kernel PANIC',
              'PANIC' not in log_text().upper().replace('NO PANIC', ''))
    finally:
        try:
            if q is not None:
                q.cmd('quit')
        except Exception:                    # noqa: BLE001 - best effort
            pass
        try:
            proc.wait(timeout=20)
        except Exception:                    # noqa: BLE001
            proc.kill()

    failed = [n for n, ok, _ in checks if not ok]
    print('[swap] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    if failed:
        print('[swap] serial log: ' + SERIAL)
        for n in failed:
            print('  FAILED: ' + n)
    print('[swap] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
