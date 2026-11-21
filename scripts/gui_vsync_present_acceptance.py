#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS v0.64 acceptance: vertical-retrace probe + present latency histograms.

The desktop present path now asks the kernel for the next vertical retrace
(kernel/gui/vsync.c reads the VGA-compatible status port 0x3DA bit 3) and records
two histograms: the bounded retrace wait (us) and the production -> present
latency (ms).

The probe is *validated*, not assumed: `vsynctest` latches 60 rising edges against
the HPET clock, cross-checks the VGA CRTC registers and rejects any edge rate that
is not a display refresh (40..120 Hz). This gate therefore accepts exactly two
honest outcomes and fails everything else:

  A) source=vretrace working: measured 55..65 Hz, edges consumed, waits recorded.
  B) source rejected on measured evidence: the port toggles far faster than a
     display refresh (observed_hz > 1000 and edges_in_1s > 10000), so the present
     path falls back to the HPET 60 Hz grid and every wait is counted as
     "unavailable" - it never paces on the rejected bit.

In both cases:
  * "[vsync] math:" proves the rate policy / formatter / period arithmetic,
  * edges + timeouts + unavailable == presents (one bounded attempt per present),
  * wait_hist sums to the edge count, lat_hist sums to presents - 1,
  * `guishell swapstat` repeats the same counters,
  * no kernel PANIC.

Usage: py -3 scripts/gui_vsync_present_acceptance.py
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
QMP_PORT = int(os.environ.get('VSYNC_QMP_PORT', '45897'))
SERIAL = 'build/serial_vsync_present.log'
DISK = 'build/fat32_ata.img'

PROBE_RE = re.compile(r'\[vsync\] probe: latch_edges=(\d+) observed_hz=([\d.]+)')
INIT_RE = re.compile(r'\[vsync\] init: source=(\w+)')
PRESENT_RE = re.compile(r'\[vsync\] present x30: edges=(\d+) timeouts=(\d+)')
WINDOW_RE = re.compile(r'\[vsync\] edges_in_1s=(\d+) samples=(\d+) high=(\d+)')
MATH_RE = re.compile(r'\[vsync\] math: .* (PASS|FAIL)')
VSYNC_RE = re.compile(
    r'\[gui-shell\] vsync: mode=(\w+) hz=(\d+) edges=(\d+) timeouts=(\d+) '
    r'unavailable=(\d+) wait_us=(\d+) wait_max_us=(\d+) wait_hist=([\d,]+) '
    r'lat_hist=([\d,]+)')
STAT_RE = re.compile(
    r'\[gui-shell\] swapstat presents=(\d+) .*?'
    r'vsync_edges=(\d+) vsync_timeouts=(\d+) vsync_unavail=(\d+) vsync_hz=(\d+) '
    r'wait_us=(\d+) wait_max_us=(\d+) wait_hist=([\d,]+) lat_hist=([\d,]+)')


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


def move_to(q, pos, tx, ty, settle=0.3):
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


def boot():
    if os.path.exists(os.path.join(ROOT, SERIAL)):
        os.remove(os.path.join(ROOT, SERIAL))
    log = open(os.path.join(ROOT, 'build/qemu_vsync_present.log'), 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


def sums(csv):
    return sum(int(v) for v in csv.split(',') if v)


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

    qemu, log = boot()
    pos = [512, 648]
    try:
        q = Qmp(QMP_PORT)
        print('waiting for the shell prompt ...', flush=True)
        time.sleep(75)
        # 1. the probe, in the terminal (its own command, real keyboard)
        press_keys(q, list('vsynctest') + ['ret'])
        time.sleep(6)
        # 2. the desktop present path
        press_keys(q, list('startgui') + ['ret'])
        time.sleep(16)
        for (tx, ty) in [(300, 500), (700, 200), (500, 600), (820, 300), (420, 420)]:
            move_to(q, pos, tx, ty, settle=0.6)
        press_keys(q, ['esc'])
        time.sleep(4)
        press_keys(q, list('guishell') + ['spc'] + list('swapstat') + ['ret'])
        time.sleep(4)
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:                  # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    check('the shell reached its prompt', '[gui-shell]' in text)
    check('no kernel PANIC', 'PANIC' not in text)

    math = MATH_RE.search(text)
    check('the rate policy / formatter / period arithmetic self-test passes',
          math is not None and math.group(1) == 'PASS',
          math.group(0) if math else 'no math selftest line')

    probe = PROBE_RE.search(text)
    check('the probe latched edges against the HPET clock', probe is not None,
          probe.group(0) if probe else 'no probe line')
    init = INIT_RE.search(text)
    check('the probe published an explicit verdict (source=...)', init is not None,
          init.group(0) if init else 'no init line')
    source = init.group(1) if init else '?'
    window = WINDOW_RE.search(text)
    present = PRESENT_RE.search(text)

    if source == 'vretrace':
        obs = float(probe.group(2)) if probe else 0.0
        check('a real vblank source is available and measured at 55..65 Hz',
              55.0 <= obs <= 65.0, 'observed_hz=%.2f' % obs)
        check('the 30 bounded waits consumed edges',
              present is not None and int(present.group(1)) > 0,
              present.group(0) if present else 'no present x30 line')
        mode_ok = 'vblank'
    else:
        obs = float(probe.group(2)) if probe else 0.0
        edges_1s = int(window.group(1)) if window else 0
        high = int(window.group(3)) if window else 0
        samples = int(window.group(2)) if window else 0
        check('the rejected source toggles far faster than a display refresh',
              obs > 1000.0, 'observed_hz=%.2f' % obs)
        check('one second of raw sampling proves the bit really moves',
              edges_1s > 10000 and high > 0 and samples > 0,
              'edges_in_1s=%d samples=%d high=%d' % (edges_1s, samples, high))
        check('present() refuses to pace on the rejected bit',
              present is not None and int(present.group(1)) == 0,
              present.group(0) if present else 'no present x30 line')
        mode_ok = 'off'

    lines = VSYNC_RE.findall(text)
    check('the present path logs vsync lines', len(lines) > 0,
          '%d lines' % len(lines))
    if not lines:
        ok = sum(1 for _, c, _ in checks if c)
        print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
        print('RESULT: FAIL')
        return 1

    mode, hz, edges, timeouts, unavail, wavg, wmax, whist, lhist = lines[-1]
    edges, timeouts, unavail = int(edges), int(timeouts), int(unavail)
    check('the present path uses the same verdict the probe published',
          mode == mode_ok, 'mode=%s expected=%s' % (mode, mode_ok))
    check('the wait histogram sums to the edge count (per-frame line)',
          sums(whist) == edges, 'hist sum %d vs edges %d' % (sums(whist), edges))
    check('the wait never exceeded the 40 ms driver bound', int(wmax) <= 40000,
          'wait_max_us=%s' % wmax)

    stat = STAT_RE.findall(text)
    check('`guishell swapstat` repeats the vsync counters', len(stat) > 0,
          '%d stat lines' % len(stat))
    if stat:
        presents = int(stat[-1][0])
        s_edges = int(stat[-1][1])
        s_timeouts = int(stat[-1][2])
        s_unavail = int(stat[-1][3])
        s_wmax = int(stat[-1][6])
        s_whist = stat[-1][7]
        s_lhist = stat[-1][8]
        check('every present tried exactly one bounded retrace wait',
              s_edges + s_timeouts + s_unavail == presents,
              'edges %d + timeouts %d + unavail %d vs presents %d'
              % (s_edges, s_timeouts, s_unavail, presents))
        check('the wait histogram sums to the edge count in swapstat',
              sums(s_whist) == s_edges,
              'hist sum %d vs edges %d' % (sums(s_whist), s_edges))
        check('the latency histogram covers every present after the first',
              sums(s_lhist) == presents - 1,
              'lat_hist sum %d vs presents-1 %d' % (sums(s_lhist), presents - 1))
        check('the per-frame line is a snapshot of the same counters',
              edges <= s_edges and int(wmax) <= s_wmax
              and sums(whist) <= sums(s_whist),
              'line %d/%d/%d vs stat %d/%d/%d'
              % (edges, int(wmax), sums(whist), s_edges, s_wmax, sums(s_whist)))

    ok = sum(1 for _, c, _ in checks if c)
    print('\nvsync source verdict: %s (observed %s Hz)' % (source, probe.group(2) if probe else '?'))
    print('checks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
