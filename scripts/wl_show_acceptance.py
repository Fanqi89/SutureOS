#!/usr/bin/env python3
"""SutureOS v0.59.0: the minimal Wayland server's committed surface must reach the
SCREEN (not just the serial log).

Flow: boot -> `run wlclient` (ring-3 client does wl_display/wl_compositor/wl_shm
handshake, commits a shared-memory surface) -> `wlshow` (kernel presents
`wl_server_latest_surface()` on the framebuffer) -> screendump.

  py -3 scripts/wl_show_acceptance.py
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
PORT = int(os.environ.get('WL_SHOW_QMP_PORT', '45903'))
LOG = 'build/serial_wl_show.log'
SHOT = 'build/wl_show.ppm'
ACCENT = (0x4f, 0x8d, 0xff)


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


def main():
    log_abs = os.path.join(ROOT, LOG)
    for p in [log_abs, os.path.join(ROOT, SHOT)]:
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[wlshow] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2
    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[wlshow] ' + ' '.join(argv), flush=True)
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

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    try:
        q = Qmp(PORT)

        def type_line(text):
            for ch in text:
                q.hmp('sendkey ' + ('spc' if ch == ' ' else ch))
                time.sleep(0.12)
            time.sleep(0.2)
            q.hmp('sendkey ret')

        if not wait_for('sutureos#', 180):
            print('[wlshow] shell prompt never appeared')
            return 1
        time.sleep(0.5)
        type_line('run wlclient')
        check('ring-3 wlclient completes the Wayland handshake',
              wait_for('[wlclient] RESULT PASS', 180),
              'log line "[wlclient] RESULT PASS"')
        commit = [l for l in log_text().splitlines() if '[wl] commit ' in l]
        check('server received a committed wl_surface', bool(commit),
              (commit[-1] if commit else 'no [wl] commit line'))
        if commit:
            import re
            m = re.search(r'commit surface=\d+ buffer=\d+ (\d+)x(\d+) stride=(\d+) hash=([0-9a-fA-F]+)', commit[-1])
            check('commit carries geometry + a pixel hash', bool(m),
                  m.group(0) if m else 'unparsable commit line')
        time.sleep(0.5)
        type_line('wlshow')
        check('wlshow presents the committed surface',
              wait_for('[wl] show ', 120),
              'log line "[wl] show WxH scale=... at x,y"')
        time.sleep(1.2)
        q.hmp('screendump ' + SHOT)
        time.sleep(1.0)
        q.hmp('sendkey esc')
        check('wlshow returns to the terminal',
              wait_for('[wl] show: back to terminal', 60))
    finally:
        try:
            q.hmp('quit')
        except Exception:                    # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    from PIL import Image
    shot_path = os.path.join(ROOT, SHOT)
    if os.path.exists(shot_path):
        im = Image.open(shot_path).convert('RGB')
        px = im.load()
        w, h = im.size
        border = sum(1 for y in range(h) for x in range(w)
                     if px[x, y] == ACCENT)
        nonblack = sum(1 for y in range(120, h - 160, 2)
                       for x in range(60, w - 60, 2) if sum(px[x, y]) > 30)
        total = len(range(120, h - 160, 2)) * len(range(60, w - 60, 2))
        colours = {}
        for y in range(200, h - 240, 3):
            for x in range(200, w - 200, 3):
                p = px[x, y]
                colours[p] = colours.get(p, 0) + 1
        top = sorted(colours.items(), key=lambda kv: -kv[1])[:4]
        check('framebuffer shows the surface (accent border painted)',
              border > 400, '%d accent-border pixels' % border)
        check('surface pixels are on screen (non-black coverage)',
              nonblack > total * 0.25,
              '%.1f%% of the sampled centre is non-black' % (100.0 * nonblack / total))
        check('surface has structure (more than one dominant colour)',
              len(top) >= 2 and top[0][1] < total * 0.995,
              'dominant colours: %s' % [(c, n) for c, n in top])
    else:
        check('screendump written', False)

    check('no kernel PANIC', 'PANIC' not in log_text())
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print('[wlshow] checks=%d ok=%d fail=%d' % (len(checks), passed, failed))
    print('[wlshow] RESULT: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
