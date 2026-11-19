#!/usr/bin/env python3
"""SutureOS ring-3 Wayland-prerequisite probe acceptance (v0.56.0).

Runs `run wlprobe`: a real ring-3 program that creates an AF_UNIX socketpair, a
memfd, maps it MAP_SHARED, and hands the fd to the peer with SCM_RIGHTS — proving
the four kernel capabilities are usable from user mode, not just from ring 0.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/wlprobe_acceptance.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_wlprobe.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ata.args')

# `run wlprobe` -> r;u;n;spc;w;l;p;r;o;b;e;ret
KEYS = ';'.join(['r', 'u', 'n', 'spc'] + list('wlprobe') + ['ret'])


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')

    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_wlprobe.log',
                    '-ArgFile', 'build/qemu_ata.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45601',
                    '-Keys', KEYS, '-AfterKeysSeconds', '8'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    want = [
        ('ring-3 program started', '[wlprobe] ring-3 Wayland prerequisite probe'),
        ('socketpair from ring 3', 'ok   ring-3 socketpair(AF_UNIX) succeeds'),
        ('the two fds differ', 'ok   the two fds differ'),
        ('memfd from ring 3', 'ok   ring-3 memfd_create(4096) returns an fd'),
        ('MAP_SHARED from ring 3', 'ok   ring-3 mmap(MAP_SHARED) of the memfd succeeds'),
        ('shared writes stick', 'ok   writes through the shared mapping stick'),
        ('SCM_RIGHTS send from ring 3', 'ok   ring-3 sendmsg(fd + SCM_RIGHTS) transfers 5 bytes'),
        ('recvmsg from ring 3', 'ok   ring-3 recvmsg returns the 5 data bytes'),
        ('data byte-exact', 'ok   the data payload is byte-exact'),
        ('descriptor delivered', 'ok   recvmsg delivered the passed descriptor'),
        ('descriptor usable', 'ok   the delivered descriptor is usable'),
        ('probe reports PASS', '[wlprobe] RESULT PASS'),
        ('console echoed the command', 'run wlprobe'),
    ]
    checks = [(name, needle in log) for name, needle in want]
    if 'PANIC' in log.upper().replace('NO PANIC', ''):
        checks.append(('no kernel PANIC', False))
    else:
        checks.append(('no kernel PANIC', True))
    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[wlprobe] checks=%d ok=%d fail=%d' % (len(checks), len(checks) - len(failed), len(failed)))
    print('[wlprobe] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
