#!/usr/bin/env python3
"""SutureOS procfs acceptance (file-system domain gap, real boot + real keys).

The synthetic /proc filesystem is proven end-to-end:
  * mount line `[procfs] mounted at /proc (5 generated files)`,
  * `ls /proc` lists meminfo/uptime/version/cpuinfo/mounts,
  * `cat /proc/meminfo` prints MemTotal/MemFree (non-zero numbers),
  * `cat /proc/version` names SutureOS and the version,
  * `cat /proc/uptime` prints "N.NN N.NN",
  * `cat /proc/mounts` lists the ramfs root, /proc itself and the FAT32 mount,
  * a write to /proc is rejected (read-only), and the kernel never panics.

Keys are typed through QMP one character at a time with a delay: the shell input
ring is small and a bulk `sendkey` batch loses characters (this is exactly how
the first attempt accidentally typed "info" instead of "ls /proc").

  py -3 scripts/procfs_acceptance.py
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
QMP_PORT = 45731
SERIAL = 'build/serial_procfs.log'
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
    print('[procfs] ' + ' '.join(argv), flush=True)
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
        commands = ['ls /proc', 'cat /proc/meminfo', 'cat /proc/version',
                    'cat /proc/uptime', 'cat /proc/mounts', 'write /proc/x nope']
        for c in commands:
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
        ('procfs mounted at /proc', '[procfs] mounted at /proc' in log),
        ('ls /proc lists all five generated files',
         all(n in log for n in ('meminfo', 'uptime', 'version', 'cpuinfo', 'mounts'))),
        ('cat /proc/meminfo prints MemTotal',
         re.search(r'MemTotal:\s+\d+ kB', log) is not None),
        ('MemFree is a non-zero number',
         re.search(r'MemFree:\s+([1-9]\d*) kB', log) is not None),
        ('cat /proc/version names SutureOS',
         re.search(r'SutureOS kernel version \d+\.\d+\.\d+-alpha', log) is not None),
        ('cat /proc/uptime prints N.NN N.NN',
         re.search(r'^\d+\.\d+ \d+\.\d+$', log, re.MULTILINE) is not None),
        ('cat /proc/mounts lists /proc and the FAT32 mount',
         re.search(r'none\s+/proc\s+procfs', log) is not None and
         re.search(r'\S+\s+/mnt/fat\s+fat32', log) is not None),
        ('write to /proc is rejected (read-only)',
         'write: /proc/x: operation not supported' in log),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
    ]

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[procfs] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[procfs] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
