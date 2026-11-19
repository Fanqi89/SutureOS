#!/usr/bin/env python3
"""SutureOS SCM_RIGHTS acceptance (v0.55.0, Wayland prerequisites).

Boots the ISO and runs `fdpass`: a memfd pool is created, seeded, and handed to the
other end of an AF_UNIX socketpair; the receiver must get a usable descriptor whose
payload is readable (same object) and the queue must drain.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/fdpass_acceptance.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_fdpass.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ata.args')
CMD = 'fdpass'


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')

    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_fdpass.log',
                    '-ArgFile', 'build/qemu_ata.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45599',
                    '-Keys', ';'.join(list(CMD)) + ';ret', '-AfterKeysSeconds', '6'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    m = re.search(r'\[fdpass\] selftest: (\d+) ok, (\d+) FAIL', log)
    ok = int(m.group(1)) if m else 0
    fail = int(m.group(2)) if m else -1

    want = [
        ('memfd pool created', 'ok   create the memfd pool to pass'),
        ('pool sized to one page', 'ok   size the pool to one page'),
        ('payload seeded', 'ok   seed the pool with a payload'),
        ('socketpair created', 'ok   create the AF_UNIX socketpair'),
        ('5 data bytes sent', 'ok   send 5 bytes of data'),
        ('memfd queued as SCM_RIGHTS', 'ok   queue the memfd as SCM_RIGHTS'),
        ('memfd is a passable descriptor', 'ok   the memfd is a passable descriptor'),
        ('console fd refused', 'ok   a console fd is not passable (honest refusal)'),
        ('5 data bytes received', 'ok   receive the 5 data bytes'),
        ('data payload exact', 'ok   the data payload is byte-exact'),
        ('one descriptor delivered', 'ok   recvmsg delivers exactly one descriptor'),
        ('received fd names the same pool', 'ok   the received descriptor names the same pool'),
        ('received fd usable as memfd', 'ok   the received descriptor is usable as a memfd'),
        ('received pool keeps its size', 'ok   the received pool still reports one page'),
        ('pool readable through the fd', 'ok   read the pool through the received fd'),
        ('payload behind the fd exact', 'ok   the payload behind the passed fd is exact'),
        ('queue empty afterwards', 'ok   the queue is empty after recvmsg'),
    ]
    checks = [('syscalls include sendmsg(46)/recvmsg(47)',
               'AF_UNIX socketpair + poll + memfd + SCM_RIGHTS registered (6)' in log)]
    checks += [(name, needle in log) for name, needle in want]
    checks.append(('no failed check', fail == 0 and ok >= 17, 'ok=%d fail=%d' % (ok, fail)))
    checks.append(('fdpass reports PASS', 'fdpass: PASS' in log))
    checks.append(('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')))

    passed = sum(1 for _, c, *_ in checks if c)
    for name, cond, *rest in checks:
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % rest[0]) if rest and not cond else ''))
    print('[fdpass] checks=%d ok=%d fail=%d' % (len(checks), passed, len(checks) - passed))
    print('[fdpass] RESULT:', 'PASS' if passed == len(checks) else 'FAIL')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
