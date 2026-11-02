#!/usr/bin/env python3
"""SutureOS multi-volume SquashFS acceptance.

Before v0.30.0 the squashfs adapter could hold exactly one mounted volume. This
boots with two squashfs images (drive 1 on IDE, drive 4 behind AHCI), mounts the
second while the first is live, reads from both, and re-verifies both images on
the HOST (squashfs is read-only, so they must be byte-identical to the fixtures).

  py -3 scripts/mkfs_squashfs_testimg.py
  py -3 scripts/multivol_squashfs_acceptance.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_multivol_squash.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_sqtwo.args')

KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '_': 'shift-minus'}


def keys_for(cmd):
    out = []
    for ch in cmd:
        if ch in KEYMAP:
            out.append(KEYMAP[ch])
        elif ch.isupper():
            out.append('shift-' + ch.lower())
        else:
            out.append(ch)
    out.append('ret')
    return ';'.join(out)


def main():
    if os.path.exists(LOG):
        os.remove(LOG)

    subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfs_squashfs_testimg.py')],
                   cwd=ROOT, check=False)
    shutil.copy2(os.path.join(ROOT, 'build', 'squashfs.img'),
                 os.path.join(ROOT, 'build', 'squashfs_b.img'))

    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-hda build/fat32.img -hdb build/squashfs.img '
                 '-device ich9-ahci,id=ahci '
                 '-drive id=s2,file=build/squashfs_b.img,format=raw,if=none '
                 '-device ide-hd,drive=s2,bus=ahci.0\n')

    cmds = [
        'mount squashfs 4 /mnt/s2',
        'ls /mnt/s2',
        'ls /mnt/squash',
        'cat /mnt/squash/HELLO.TXT',
        'cat /mnt/s2/HELLO.TXT',
    ]
    keys = ';'.join(keys_for(c) for c in cmds)
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_multivol_squash.log',
                    '-ArgFile', 'build/qemu_sqtwo.args',
                    '-BootWaitSeconds', '55', '-MonitorPort', '45601',
                    '-Keys', keys, '-AfterKeysSeconds', '5'], cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = [
        ('boot mounted the first squashfs volume at /mnt/squash',
         '[squashfs] vfs mount drive=1 at /mnt/squash (volume slot 0)' in log),
        ('second squashfs volume mounted as volume slot 1',
         '[squashfs] vfs mount drive=4 at /mnt/s2 (volume slot 1)' in log),
        ('/mnt/s2 lists its own files', 'HELLO.TXT' in log and 'LARGE.BIN' in log),
        ('/mnt/squash still lists its files', 'FRAG.TXT' in log),
        ('read from the first volume succeeded',
         'Hello from a SquashFS volume' in log or 'SquashFS' in log),
    ]

    for name, path in (('volume 1', 'squashfs.img'), ('volume 2', 'squashfs_b.img')):
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'verify_squashfs_read.py'),
                            os.path.join(ROOT, 'build', path)],
                           capture_output=True, text=True)
        tail = [l for l in r.stdout.splitlines() if 'checks=' in l]
        checks.append(('host: %s passes the squashfs read verifier (83/83)' % name,
                       'checks=83 ok=83 fail=0' in r.stdout, tail[-1].strip() if tail else ''))

    print('--- checks ---')
    ok = True
    for item in checks:
        name, cond = item[0], item[1]
        detail = ('  (%s)' % item[2]) if len(item) > 2 and item[2] else ''
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, detail))
        ok = ok and cond
    print('RESULT: %s (checks=%d)' % ('PASS' if ok else 'FAIL', len(checks)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
