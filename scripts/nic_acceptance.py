#!/usr/bin/env python3
"""SutureOS NIC acceptance: prove BOTH link layers really move packets.

Why this exists: the boot-time self test is a good gate for RTL8139, but on an
e1000-only machine (VMware, or QEMU `-device e1000`) the emulator does not always
service a *polled* receive while the boot self test runs with interrupts masked,
so the boot log alone cannot prove the e1000 driver. This script drives the real
kernel twice - once per NIC - and asserts on the serial log:

  rtl8139 : "[net] rtl8139 found at ..." + "[net] selftest ping 10.0.2.2 -> reply in"
  e1000   : "[net] e1000 found at ..."   + an interactive `ping 10.0.2.2` that
            prints >= 3 "64 bytes from 10.0.2.2" lines (typed with QMP sendkey)

The ping is typed into the kernel shell, so it exercises exactly the code path a
user would: ARP resolve -> ICMP echo -> RX ring -> stack -> printed reply.

Usage:
  py -3 scripts/nic_acceptance.py            # both NICs
  py -3 scripts/nic_acceptance.py e1000      # one of them
Exit code 0 only when every selected NIC passed. Logs: build/nic_<chip>.log
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = 47930

# QMP `sendkey` names for "ping 10.0.2.2\n"
KEYS_PING = ('p', 'i', 'n', 'g', 'spc', '1', '0', 'dot', '0', 'dot', '2',
             'dot', '2', 'ret')


class Qmp:
    def __init__(self, port, timeout=40):
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


def run_one(chip):
    """Boot the ISO with `chip` as the only NIC and return the serial log text."""
    log_rel = 'build/nic_%s.log' % chip
    log_abs = os.path.join(ROOT, log_rel)
    if os.path.exists(log_abs):
        os.remove(log_abs)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + log_rel,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd', '-smp', '2',
            '-no-reboot', '-no-shutdown',
            '-netdev', 'user,id=n0', '-device', '%s,netdev=n0' % chip,
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide']
    print('[nic] %s: %s' % (chip, ' '.join(argv)), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def read_log():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        q = Qmp(QMP_PORT)
        # Wait for the shell prompt (the boot self tests take a while under TCG).
        deadline = time.time() + 150
        while time.time() < deadline:
            if 'sutureos# ' in read_log():
                break
            time.sleep(1.0)
        else:
            raise RuntimeError('shell prompt never appeared')

        time.sleep(1.0)
        for k in KEYS_PING:
            q.hmp('sendkey ' + k)
            time.sleep(0.12)
        # Give the three probes + ARP time to finish.
        deadline = time.time() + 25
        while time.time() < deadline:
            if read_log().count('64 bytes from 10.0.2.2') >= 3:
                break
            time.sleep(0.5)
        return read_log()
    finally:
        try:
            q.hmp('quit')
        except Exception:                   # noqa: BLE001 - teardown best effort
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    wanted = sys.argv[1:] or ['rtl8139', 'e1000']
    failures = []

    for chip in wanted:
        print('\n=== %s ===' % chip, flush=True)
        try:
            log = run_one(chip)
        except Exception as e:              # noqa: BLE001 - report, do not raise
            print('  driver run failed: %r' % (e,))
            failures.append(chip)
            continue

        link_ok = ('[net] %s found at' % chip) in log
        replies = log.count('64 bytes from 10.0.2.2')
        boot_ping = '[net] selftest ping 10.0.2.2 -> reply in' in log
        print('  link-up line      : %s' % ('PASS' if link_ok else 'FAIL'))
        print('  interactive ping  : %s (%d reply line(s))'
              % ('PASS' if replies >= 3 else 'FAIL', replies))
        print('  boot self test    : %s'
              % ('reply' if boot_ping else 'no reply (documented for e1000)'))

        if chip == 'rtl8139':
            ok = link_ok and replies >= 3 and boot_ping
        else:
            ok = link_ok and replies >= 3
        if not ok:
            failures.append(chip)
        print('  RESULT: %s' % ('PASS' if ok else 'FAIL'))

    print('\n=== overall: %s ===' % ('PASS' if not failures else
                                     'FAIL (%s)' % ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
