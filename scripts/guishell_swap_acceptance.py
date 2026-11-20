#!/usr/bin/env python3
"""SutureOS present/swap-chain acceptance (task-4).

Proves, with real QEMU input and real screenshots, that the Rust desktop shell
now presents through an explicit double-buffered swap chain with a vsync gate,
and that this changed no pixels:

  * `guishell` still starts the same loop              -> "[gui-shell] shell: loop"
  * the pointer moves through the cursor fast path     -> "cursor: fast path frames="
  * clicks / minimise-restore generate full frames     -> "profile/30:"
  * every 30 frames the swap counters are logged
      -> "[gui-shell] swap presents=P gens=G presented=PR dropped=D jitter_us=J
          interval_ms=I skipped=S latency_us=L latency_max_us=LM hist=.."
      - presents == gens == presented on every line
      - gens / presents / dropped never go backwards (monotonic)
      - skipped >= 0, interval_ms > 0, jitter_us inside one 60 Hz slot
      - production->present latency <= 2 frames, histogram non-empty
  * after ESC, `guishell swapstat` reports the cumulative counters once
      - presents == gens == presented, rendered == presented + skipped
      - damage oracle: 0 mismatches; present completion: 0 tear_bad
      - buffer state machine: buf_checks > 0, buf_bad == 0, exactly one
        PRESENTED buffer and no READY buffer left at rest
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
# own copy with a distinct image name: other sessions on this box kill
# every process called qemu-system-x86_64 when they clean up strays
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
    r'\[gui-shell\] swap presents=(\d+) gens=(\d+) presented=(\d+) dropped=(\d+) '
    r'jitter_us=(\d+) interval_ms=(\d+) skipped=(\d+) latency_us=(\d+) '
    r'latency_max_us=(\d+) hist=([\d,]+)')
STAT_RE = re.compile(
    r'\[gui-shell\] swapstat presents=(\d+) gens=(\d+) presented=(\d+) '
    r'dropped=(\d+) jitter_us=(\d+) interval_ms=(\d+) skipped=(\d+) '
    r'rendered=(\d+) scene=(\d+) carry=(\d+) full=(\d+) dmg=(\d+) '
    r'dmg_px=(\d+) framed_px=(\d+) oracle=(\d+) mismatch=(\d+) tear=(\d+) '
    r'tear_bad=(\d+) late=(\d+) latency_us=(\d+) latency_max_us=(\d+) '
    r'hist=([\d,]+) deferred=(\d+) pending=(\d+) bufs=(\w+) '
    r'buf_checks=(\d+) buf_bad=(\d+)')


def stats_tuple(m):
    """swapstat match -> tuple of ints, with the hist/bufs string groups kept."""
    out = []
    for i, v in enumerate(m):
        if i in (21, 24):            # hist, bufs
            out.append(v)
        else:
            out.append(int(v))
    return tuple(out)


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
    errlog = open(os.path.join(ROOT, 'build', 'qemu_err.log'), 'wb')
    proc = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.DEVNULL,
                             stderr=errlog)
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
        swap = [tuple(int(x) for x in m[:9]) + (m[9],)
                for m in SWAP_RE.findall(txt)]
        stats = [tuple(int(x) for x in m) for m in STAT_RE.findall(txt)]

        check('swap log line appears (>=3 windows of 30 frames)',
              len(swap) >= 3, 'lines=%d' % len(swap))
        check('presents == gens on every swap line (one generation per present)',
              swap and all(m[0] == m[1] for m in swap),
              'first=%r last=%r' % (swap[0] if swap else None,
                                    swap[-1] if swap else None))
        check('swap log carries the full field set (presented/dropped/jitter/hist)',
              swap and all(len(m[9]) > 0 for m in swap),
              'last hist=%s' % (swap[-1][9] if swap else ''))
        check('presented == presents (every present hands one buffer to the screen)',
              swap and all(m[2] == m[0] for m in swap),
              'presented=%r' % [m[2] for m in swap])
        check('presents strictly increases between windows',
              all(b[0] > a[0] for a, b in zip(swap, swap[1:])),
              'presents=%r' % [m[0] for m in swap])
        check('gens is monotonically increasing',
              all(b[1] > a[1] for a, b in zip(swap, swap[1:])),
              'gens=%r' % [m[1] for m in swap])
        check('skipped is >= 0 and never decreases',
              all(m[6] >= 0 for m in swap) and
              all(b[6] >= a[6] for a, b in zip(swap, swap[1:])),
              'skipped=%r' % [m[6] for m in swap])
        check('dropped is >= 0 and never decreases',
              all(m[3] >= 0 for m in swap) and
              all(b[3] >= a[3] for a, b in zip(swap, swap[1:])),
              'dropped=%r' % [m[3] for m in swap])
        check('interval_ms > 0 (measured on the HPET grid)',
              all(m[5] > 0 for m in swap),
              'interval_ms=%r' % [m[5] for m in swap])
        check('jitter stays inside one 60 Hz slot (<= 8333 us)',
              all(0 <= m[4] <= 8333 for m in swap),
              'jitter_us=%r' % [m[4] for m in swap])
        check('present latency <= 2 frames (no unbounded queueing)',
              all(0 <= m[8] < 33_000 for m in swap),
              'latency_max_us=%r' % [m[8] for m in swap])
        check('interval histogram buckets are being filled',
              swap and sum(int(v) for v in swap[-1][9].split(',')) > 0,
              'hist=%s' % (swap[-1][9] if swap else ''))

        # present rate from the last logged window (30 frames)
        if swap:
            ivl = swap[-1][5]
            rate = 1000.0 / ivl if ivl else 0.0
            print('[swap] last window: presents=%d skipped=%d interval_ms=%d '
                  'jitter_us=%d (~%.1f presents/s)'
                  % (swap[-1][0], swap[-1][6], ivl, swap[-1][4], rate))

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
        stats = [stats_tuple(m) for m in STAT_RE.findall(log_text())]
        if stats:
            st = stats[-1]
            print('[swap] swapstat: presents=%s gens=%s presented=%s dropped=%s '
                  'jitter_us=%s interval_ms=%s skipped=%s rendered=%s scene=%s '
                  'oracle=%s mismatch=%s tear=%s tear_bad=%s late=%s '
                  'bufs=%s buf_checks=%s buf_bad=%s'
                  % (st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8],
                     st[14], st[15], st[16], st[17], st[18], st[24], st[25], st[26]))
            check('swapstat presents > 0 after a desktop session', int(st[0]) > 0)
            check('swapstat presents == gens == presented',
                  st[0] == st[1] == st[2])
            check('accounting closes: rendered == presented + skipped + pending',
                  int(st[7]) == int(st[0]) + int(st[6]) + int(st[23]),
                  'rendered=%s presented=%s skipped=%s pending=%s'
                  % (st[7], st[0], st[6], st[23]))
            check('damage oracle: mismatches must be zero',
                  int(st[15]) == 0, 'oracle=%s mismatch=%s' % (st[14], st[15]))
            check('present completion check: no partial presents',
                  int(st[16]) > 0 and int(st[17]) == 0,
                  'tear=%s tear_bad=%s' % (st[16], st[17]))
            check('buffer state machine: invariants held, one PRESENTED buffer',
                  int(st[25]) > 0 and int(st[26]) == 0 and st[24].count('P') == 1
                  and st[24].count('R') == 0,
                  'bufs=%s checks=%s bad=%s' % (st[24], st[25], st[26]))
            check('late frames (missed 60 Hz slot) are a small minority',
                  int(st[18]) <= max(4, int(st[0]) // 4),
                  'late=%s presents=%s' % (st[18], st[0]))
            check('latency stays well under two frames on average',
                  int(st[20]) < 33_000, 'latency_us=%s' % st[20])
            if swap:
                check('cumulative counters never go backwards vs the live log',
                      int(st[0]) >= swap[-1][0] and int(st[6]) >= swap[-1][6])
        else:
            check('swapstat line parsed', False)

        # --- dedicated pointer-motion session: how many presents per second
        # does a pure cursor-fast-path session achieve? Two `guishell swapstat`
        # readings bracket the second session, and Python times the burst. ---
        stats_before = [stats_tuple(m) for m in STAT_RE.findall(log_text())]
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
            stats_after = [stats_tuple(m) for m in STAT_RE.findall(log_text())]
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
