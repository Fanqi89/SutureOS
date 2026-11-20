#!/usr/bin/env python3
"""SutureOS GUI appearance gate runner.

Boots the current ISO, enters the desktop with `startgui`, takes two
screendumps of the untouched default desktop (the second one is the empirical
"live data" mask) and runs scripts/gui_appearance_check.py on them. Exit code 0
means the frozen GUI did not move.

  py -3 scripts/gui_baseline_shot.py
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp, SCREEN_W, SCREEN_H  # noqa: E402

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 48131
LOG = 'build/serial_gui_shot.log'
SHOT_A = 'build/acc_gui1.ppm'
SHOT_B = 'build/acc_gui2.ppm'


def main():
    if os.path.exists(os.path.join(ROOT, LOG)):
        os.remove(os.path.join(ROOT, LOG))
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(os.path.join(ROOT, LOG), encoding='utf-8', errors='replace') as fh:
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
            for ch in 'startgui':
                key(ch)
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
        except Exception:                    # noqa: BLE001 - best effort shutdown
            pass
        try:
            proc.wait(timeout=20)
        except Exception:                    # noqa: BLE001
            proc.kill()

    if '[gui] main loop start' not in log_text():
        print('FAIL: GUI never started (see %s)' % LOG)
        return 1
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'gui_appearance_check.py'),
                        SHOT_A, '--live-shot', SHOT_B], cwd=ROOT)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
