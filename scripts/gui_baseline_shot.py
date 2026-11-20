#!/usr/bin/env python3
"""SutureOS desktop appearance gate (v0.59.0): `startgui` is now the Rust desktop
shell, so this script boots, enters the desktop, takes two screendumps of the
untouched desktop and runs the new-shell appearance gate against
docs/baseline/base_gui.png.

  py -3 scripts/gui_baseline_shot.py
Exit code 0 means the frozen Rust desktop did not move (outside live data).
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = int(os.environ.get('DESKTOP_QMP_PORT', '48141'))
LOG = 'build/serial_desktop_shot.log'
SHOT_A = 'build/acc_desktop1.ppm'
SHOT_B = 'build/acc_desktop2.ppm'


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
    for p in [log_abs, os.path.join(ROOT, SHOT_A), os.path.join(ROOT, SHOT_B)]:
        if os.path.exists(p):
            os.remove(p)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[desktop] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[desktop] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    q = None
    try:
        q = Qmp(PORT)
        for _ in range(180):
            if 'sutureos# ' in log_text():
                break
            time.sleep(1)
        # type startgui (the C loop keeps keyboard focus in the terminal)
        for ch in 'startgui':
            q.hmp('sendkey ' + ch)
            time.sleep(0.08)
        q.hmp('sendkey ret')
        ok = False
        for _ in range(120):
            txt = log_text()
            if '[gui-shell] shell: exit' in txt:
                break
            if '[gui-shell] frame 30' in txt:
                ok = True
                break
            time.sleep(1)
        if not ok:
            print('[desktop] the Rust desktop never reached frame 30')
            return 1
        time.sleep(1.0)
        q.hmp('screendump ' + SHOT_A)
        time.sleep(2.0)
        q.hmp('screendump ' + SHOT_B)
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

    if '--save-baseline' in sys.argv:
        # capture-only mode: freeze the current desktop as the new baseline
        from PIL import Image
        Image.open(os.path.join(ROOT, SHOT_A)).convert('RGB').save(
            os.path.join(ROOT, 'docs', 'baseline', 'base_gui.png'))
        print('[desktop] baseline updated: docs/baseline/base_gui.png')
        return 0
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, 'scripts', 'gui_appearance_check.py'),
                        SHOT_A, '--live-shot', SHOT_B, '--profile', 'new'],
                       cwd=ROOT)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
