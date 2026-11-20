#!/usr/bin/env python3
"""SutureOS OS67 inode-filesystem (Minix v1 layout) acceptance.

Real boot with the fixture image as the second IDE disk (drive 1), then real
keystrokes through QMP:
    mount os67fs 1 /mnt/o67
    ls /mnt/o67
    cat /mnt/o67/HELLO.TXT
    cat /mnt/o67/SUBDIR/INNER.TXT
    stat /mnt/o67/BIG.BIN
    cat /mnt/o67/EMPTY
    write /mnt/o67/NOPE.TXT hi        (must be refused: read-only driver)
    umount /mnt/o67
plus the independent host parser (scripts/verify_os67_read.py) on the same
image, proving the guest did not modify it.

  py -3 scripts/os67fs_acceptance.py
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
QMP_PORT = 45791
SERIAL = 'build/serial_os67fs.log'
LOG_ABS = os.path.join(ROOT, SERIAL)


class Qmp:
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:
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


KEY = {' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', '_': 'shift-minus'}


def type_text(q, text, delay=0.075):
    for ch in text:
        if 'A' <= ch <= 'Z':
            q.hmp('sendkey shift-' + ch.lower())
        else:
            q.hmp('sendkey ' + KEY.get(ch, ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(0.45)


def main():
    if not os.path.exists(os.path.join(ROOT, 'build', 'os67.img')):
        print('fixture missing: run py -3 scripts/mkfs_os67_testimg.py first')
        return 2
    if os.path.exists(LOG_ABS):
        os.remove(LOG_ABS)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=bdisk,file=build/os67.img,format=raw,if=ide,index=1',
            '-smp', '2', '-no-reboot']
    print('[os67fs] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def logtext():
        try:
            with open(LOG_ABS, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        deadline = time.time() + 90
        while time.time() < deadline:
            if 'sutureos# ' in logtext():
                break
            time.sleep(0.4)
        q = Qmp(QMP_PORT)
        for cmd in ['mount os67fs 1 /mnt/o67', 'ls /mnt/o67',
                    'cat /mnt/o67/HELLO.TXT', 'cat /mnt/o67/SUBDIR/INNER.TXT',
                    'stat /mnt/o67/BIG.BIN', 'cat /mnt/o67/EMPTY',
                    'write /mnt/o67/NOPE.TXT hi', 'umount /mnt/o67']:
            type_text(q, cmd)
        try:
            q.cmd('quit')
        except Exception:
            pass
    finally:
        try:
            proc.wait(timeout=25)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = logtext()
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'verify_os67_read.py'),
                        'build/os67.img'], cwd=ROOT, capture_output=True, text=True)
    verifier_ok = '[os67fs-verify] RESULT: PASS' in (r.stdout or '')

    def has(pattern):
        return re.search(pattern, log, re.MULTILINE) is not None

    checks = [
        ('boot probe reports the minix v1 volume',
         has(r'\[os67fs\] volume on drive 1: minix v1')),
        ('mount succeeds with a geometry line',
         has(r'\[os67fs\] mount drive=1 ninodes=\d+ zones=\d+')),
        ('vfs mount line names the mount point',
         has(r'\[os67fs\] vfs mount drive=1 at /mnt/o67')),
        ('ls lists HELLO.TXT and BIG.BIN',
         has(r'^HELLO\.TXT\s') and has(r'^BIG\.BIN\s')),
        ('ls lists SUBDIR and EMPTY', has(r'^SUBDIR\s') and has(r'^EMPTY\s')),
        ('cat HELLO.TXT prints the cached text', 'Hello from OS67 minix FS!' in log),
        ('cat SUBDIR/INNER.TXT follows the nested directory',
         'Nested file inside SUBDIR.' in log),
        ('stat BIG.BIN reports 20000 bytes', has(r'Size: 20000 bytes')),
        ('cat EMPTY reports 0 bytes', has(r'\(0 bytes\)')),
        ('write is refused with a read-only message',
         'write rejected: this driver is read-only' in log and
         has(r'write: /mnt/o67/NOPE\.TXT: operation not supported')),
        ('umount succeeds', has(r'Unmounted /mnt/o67') or has(r'\[OK\] Filesystem unmounted')),
        ('kernel self-tests are 16/16', '[stitch] selftests: 16/16 passed' in log),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
        ('host verifier PASS (independent parse of the same image)', verifier_ok),
    ]

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[os67fs] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[os67fs] RESULT:', 'PASS' if not failed else 'FAIL')
    if failed and not verifier_ok:
        print(r.stdout[-800:] if r.stdout else r.stderr[-800:])
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
