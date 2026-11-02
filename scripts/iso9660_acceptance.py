#!/usr/bin/env python3
"""SutureOS ISO9660/ATAPI acceptance driver.

Boots the ISO with a *second* data CD attached (the boot disc itself is an
El Torito disc and must be skipped), types the read commands into the kernel
shell through QMP, and leaves the serial log for the independent verifier:

  py -3 scripts/mkiso9660_testcd.py                 # build the fixture CD
  py -3 scripts/iso9660_acceptance.py               # drive the guest, write build/serial_iso9660.log
  py -3 scripts/verify_iso9660_read.py --guest-log build/serial_iso9660.log

The checks afterwards are done by verify_iso9660_read.py, which re-parses the
ISO on the host (ECMA-119) and compares the guest's transcript byte for byte -
this script only produces the transcript.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = 47951
LOG = 'build/serial_iso9660.log'

# Every command the verifier looks for in the transcript. Paths are typed in
# lower case on purpose: ISO9660 names on the disc are upper case (8.3), the
# filesystem lookup is case-insensitive, and the verifier's regexes use the
# lower-case spelling.
COMMANDS = [
    'ls /mnt/cd',
    'cat /mnt/cd/readme.txt',
    'cat /mnt/cd/sub/nested.txt',
    'cat /mnt/cd/sub/deep/third.txt',
    'ls /mnt/cd/sub/deep',
]

KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '_': 'shift-minus'}


class Qmp:
    def __init__(self, port, timeout=45):
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
    boot_iso = os.path.join(ROOT, 'build', 'sutureos.iso')
    data_cd = os.path.join(ROOT, 'build', 'test_cd.iso')
    for p in (boot_iso, data_cd):
        if not os.path.exists(p):
            sys.exit('missing %s (build the ISO / run scripts/mkiso9660_testcd.py)' % p)

    log_abs = os.path.join(ROOT, LOG)
    if os.path.exists(log_abs):
        os.remove(log_abs)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd', '-smp', '2',
            '-no-reboot', '-no-shutdown',
            '-drive', 'id=cd2,file=build/test_cd.iso,format=raw,if=none',
            '-device', 'ide-cd,drive=cd2,bus=ide.1']
    print('[iso9660] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def read_log():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        q = Qmp(QMP_PORT)
        deadline = time.time() + 150
        while time.time() < deadline:
            if 'sutureos# ' in read_log():
                break
            time.sleep(1.0)
        else:
            raise RuntimeError('shell prompt never appeared')

        for cmd in COMMANDS:
            time.sleep(0.4)
            for ch in cmd:
                if 'A' <= ch <= 'Z':
                    key = 'shift-' + ch.lower()       # QEMU sendkey needs shift-X
                else:
                    key = KEYMAP.get(ch, ch)
                q.hmp('sendkey ' + key)
                time.sleep(0.06)
            q.hmp('sendkey ret')
            time.sleep(1.2)
        time.sleep(1.0)
        print('[iso9660] transcript written to %s' % LOG)
        print('  next: py -3 scripts/verify_iso9660_read.py --guest-log %s' % LOG)
    finally:
        try:
            q.hmp('quit')
        except Exception:                   # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


if __name__ == '__main__':
    sys.exit(main())
