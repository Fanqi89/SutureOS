#!/usr/bin/env python3
"""SutureOS C-desktop appearance gate (legacy path).

Boots the current ISO, enters the ORIGINAL C desktop with `startgui-classic`,
takes two screendumps and runs scripts/gui_appearance_check.py --profile classic
against docs/baseline/base_gui_classic.png.

  py -3 scripts/gui_baseline_shot_classic.py
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp  # noqa: E402

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = int(os.environ.get('CLASSIC_QMP_PORT', '48131'))
LOG = 'build/serial_gui_shot.log'
SHOT_A = 'build/acc_gui1.ppm'
SHOT_B = 'build/acc_gui2.ppm'


def main():
    log_abs = os.path.join(ROOT, LOG)
    for p in [log_abs, os.path.join(ROOT, SHOT_A), os.path.join(ROOT, SHOT_B)]:
        if os.path.exists(p):
            os.remove(p)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
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

        def key(k, d=0.08):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        for _ in range(150):
            if 'sutureos# ' in log_text():
                break
            time.sleep(1)
        for _ in range(5):
            time.sleep(0.4)
            for _ in range(24):
                key('backspace', 0.015)
            for ch in 'startgui-classic':
                key('minus' if ch == '-' else ch, 0.12)
            key('ret')
            time.sleep(3.2)
            if '[gui] main loop start' in log_text():
                break
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

    if '[gui] main loop start' not in log_text():
        print('FAIL: the C desktop never started (see %s)' % LOG)
        return 1
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, 'scripts', 'gui_appearance_check.py'),
                        SHOT_A, '--live-shot', SHOT_B, '--profile', 'classic'],
                       cwd=ROOT)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
