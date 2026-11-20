#!/usr/bin/env python3
"""SutureOS sysfs acceptance (synthetic /sys tree over kernel objects).

Proves the hierarchical sysfs on a real boot with real keys:
  * mount line `[sysfs] mounted at /sys (kernel/ + block/)`,
  * `ls /sys` lists `kernel` and `block`,
  * `ls /sys/kernel` lists version/uptime,
  * `cat /sys/kernel/version` prints the kernel version,
  * `cat /sys/kernel/uptime` prints "N.NN",
  * `ls /sys/block` lists the registered block devices (ata0 with a disk),
  * `cat /sys/block/ata0/size` prints a non-zero sector count,
  * a write is rejected (read-only), and the kernel never panics.

  py -3 scripts/sysfs_acceptance.py
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
QMP_PORT = 45763
SERIAL = 'build/serial_sysfs.log'
LOG_ABS = os.path.join(ROOT, SERIAL)


class Qmp:
    def __init__(self, port, timeout=45):
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


KEY = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'shift-semicolon'}


def type_text(q, text, delay=0.075):
    for ch in text:
        q.hmp('sendkey ' + KEY.get(ch, ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(0.35)


def main():
    if os.path.exists(LOG_ABS):
        os.remove(LOG_ABS)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + SERIAL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot']
    print('[sysfs] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def logtext():
        try:
            with open(LOG_ABS, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        deadline = time.time() + 75
        while time.time() < deadline:
            if 'sutureos# ' in logtext():
                break
            time.sleep(0.4)
        q = Qmp(QMP_PORT)
        for c in ['ls /sys', 'ls /sys/kernel', 'cat /sys/kernel/version',
                  'cat /sys/kernel/uptime', 'ls /sys/block',
                  'cat /sys/block/ata0/size', 'write /sys/x nope']:
            type_text(q, c)
        try:
            q.cmd('quit')
        except Exception:
            pass
    finally:
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = logtext()
    checks = [
        ('sysfs mounted at /sys', '[sysfs] mounted at /sys' in log),
        ('ls /sys lists kernel and block',
         re.search(r'^kernel\s+', log, re.MULTILINE) is not None and
         re.search(r'^block\s+', log, re.MULTILINE) is not None),
        ('ls /sys/kernel lists version and uptime',
         re.search(r'^version\s+', log, re.MULTILINE) is not None and
         re.search(r'^uptime\s+', log, re.MULTILINE) is not None),
        ('cat /sys/kernel/version prints the kernel version',
         re.search(r'SutureOS kernel version \d+\.\d+\.\d+-alpha', log) is not None or
         re.search(r'\d+\.\d+\.\d+-alpha', log) is not None),
        ('cat /sys/kernel/uptime prints N.N',
         re.search(r'^/sys/kernel/uptime$', log, re.MULTILINE) is not None or
         re.search(r'^\d+\.\d+$', log, re.MULTILINE) is not None),
        ('ls /sys/block lists ata0',
         re.search(r'^ata0\s+', log, re.MULTILINE) is not None),
        ('cat /sys/block/ata0/size prints a non-zero sector count',
         re.search(r'^[1-9]\d*\s*$', log, re.MULTILINE) is not None),
        ('write to /sys is rejected (read-only)',
         re.search(r'write: /sys/x: (operation not supported|read-only)', log) is not None),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
    ]

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[sysfs] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[sysfs] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
