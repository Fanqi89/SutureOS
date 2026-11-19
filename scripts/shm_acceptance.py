#!/usr/bin/env python3
"""SutureOS memfd + MAP_SHARED acceptance (v0.54.0, Wayland prerequisites).

Boots the ISO and runs the kernel's `shmtest` command from the shell: a memfd is
created, sized to three pages, mapped twice at different addresses, and writes
through one mapping must be visible through the other (shared physical frames).

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/shm_acceptance.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_shm.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ata.args')
CMD = 'shmtest'


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')

    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_shm.log',
                    '-ArgFile', 'build/qemu_ata.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45597',
                    '-Keys', ';'.join(list(CMD)) + ';ret', '-AfterKeysSeconds', '6'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    m = re.search(r'\[shm\] memfd selftest: (\d+) ok, (\d+) FAIL', log)
    ok = int(m.group(1)) if m else 0
    fail = int(m.group(2)) if m else -1

    want = [
        ('memfd_create returns an fd', 'ok   memfd_create_obj() returns an fd'),
        ('the fd is recognised as a memfd', 'ok   the fd is recognised as a memfd'),
        ('ftruncate to 3 pages succeeds', 'ok   ftruncate to 3 pages succeeds'),
        ('the object is 12288 bytes', 'ok   the object is 12288 bytes'),
        ('all three pages are backed', 'ok   all three pages are backed'),
        ('no overrun past page 3', 'ok   page 3 is not backed (no overrun)'),
        ('first MAP_SHARED succeeds', 'ok   first MAP_SHARED succeeds'),
        ('second MAP_SHARED succeeds', 'ok   second MAP_SHARED succeeds'),
        ('map A writes visible through map B',
         'ok   writes through map A are visible through map B (shared frames)'),
        ('map B writes visible through map A',
         'ok   writes through map B are visible through map A'),
        ('kernel page view agrees', 'ok   the kernel-side page holds the same byte'),
        ('memfd_read agrees', 'ok   memfd_read() sees the mapped write'),
        ('unmap A succeeds', 'ok   unmap A succeeds'),
        ('unmap B succeeds', 'ok   unmap B succeeds'),
    ]
    checks = [('syscalls: socketpair + poll + memfd registered',
               any(int(n) >= 4 for n in
                   re.findall(r'AF_UNIX socketpair[^\n]*registered \((\d+)\)', log)))]
    checks += [(name, needle in log) for name, needle in want]
    checks.append(('no failed check', fail == 0 and ok >= 14, 'ok=%d fail=%d' % (ok, fail)))
    checks.append(('shmtest reports PASS', 'shmtest: PASS' in log))
    checks.append(('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')))

    passed = sum(1 for _, c, *_ in checks if c)
    for name, cond, *rest in checks:
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % rest[0]) if rest and not cond else ''))
    print('[shm] checks=%d ok=%d fail=%d' % (len(checks), passed, len(checks) - passed))
    print('[shm] RESULT:', 'PASS' if passed == len(checks) else 'FAIL')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
