#!/usr/bin/env python3
"""SutureOS AF_UNIX socketpair + poll acceptance (v0.53.0, Wayland prerequisites).

Boots the ISO, runs the kernel's `unixtest` command from the shell, and asserts the
serial log: socketpair creation, bidirectional byte-exact transfers, poll readiness
(POLLOUT/POLLIN/POLLHUP), EOF handling and the syscall registrations.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/unix_acceptance.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_unix.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ata.args')
CMD = 'unixtest'


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')

    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_unix.log',
                    '-ArgFile', 'build/qemu_ata.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45593',
                    '-Keys', ';'.join(list(CMD)) + ';ret', '-AfterKeysSeconds', '6'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()

    m = re.search(r'\[unix\] socketpair selftest: (\d+) ok, (\d+) FAIL', log)
    ok = int(m.group(1)) if m else 0
    fail = int(m.group(2)) if m else -1

    checks = [
        ('syscalls 7 (poll) + 53 (socketpair) registered',
         any(int(n) >= 2 for n in
             re.findall(r'AF_UNIX socketpair[^\n]*registered \((\d+)\)', log))),
        ('unixtest command reached the kernel',
         '[unix] socketpair selftest' in log),
        ('socketpair() creates a pair', 'ok   socketpair() creates a pair' in log),
        ('both ends are unix fds', 'ok   both ends are unix fds' in log),
        ('fresh pair reports POLLOUT', 'ok   fresh pair is writable (POLLOUT)' in log),
        ('fresh pair is not readable', 'ok   fresh pair is not readable (no POLLIN)' in log),
        ('write() transfers 8 bytes', 'ok   write() sends all 8 bytes' in log),
        ('peer becomes readable (POLLIN)', 'ok   peer becomes readable after the write (POLLIN)' in log),
        ('read() receives 8 bytes', 'ok   read() receives all 8 bytes' in log),
        ('payload is byte-exact', 'ok   payload is byte-exact' in log),
        ('buffer drains back to not-readable', 'ok   buffer drains back to not-readable' in log),
        ('reverse direction works', 'ok   reverse direction write works' in log),
        ('reverse payload is exact', 'ok   reverse direction payload is exact' in log),
        ('read() returns 0 (EOF) after peer close', 'ok   read() returns 0 (EOF) after the peer closes' in log),
        ('closed peer reports POLLHUP', 'ok   closed peer reports POLLHUP' in log),
        ('no failed check', fail == 0 and ok >= 14, 'ok=%d fail=%d' % (ok, fail)),
        ('unixtest reports PASS', 'unixtest: PASS' in log),
        ('no kernel PANIC', '[PANIC]' not in log and 'PANIC' not in log.upper().replace('NO PANIC', '')),
    ]

    passed = sum(1 for _, cond, *_ in checks if (cond if isinstance(cond, bool) else cond))
    for name, cond, *rest in checks:
        flag = cond if isinstance(cond, bool) else cond
        print('  [%s] %s%s' % ('ok' if flag else 'FAIL', name,
                               ('  (%s)' % rest[0]) if rest and not flag else ''))
    print('[unix] checks=%d ok=%d fail=%d' % (len(checks), passed, len(checks) - passed))
    print('[unix] RESULT:', 'PASS' if passed == len(checks) else 'FAIL')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
