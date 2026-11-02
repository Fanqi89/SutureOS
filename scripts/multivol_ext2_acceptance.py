#!/usr/bin/env python3
"""SutureOS multi-volume EXT2 acceptance.

Before v0.29.0 the ext2 driver could hold exactly one mounted volume. This test
boots with two ext2 disks (drive 1 on the primary IDE channel and drive 4 behind
an AHCI controller), mounts the second one through the shell while the first is
already mounted, writes to it, and proves on the HOST that:

  * volume 1 still passes the full ext2 write verifier (its self-test artifact is
    intact - this is also the regression test for the cross-volume write bug that
    was found while developing this feature), and
  * the write landed on volume 2 (the image changed only there).

  py -3 scripts/multivol_ext2_acceptance.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_multivol_ext2.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ext2two.args')

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

    # fresh fixtures: the ext2 images are written during boot
    subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfs_ext2_testimg.py')],
                   cwd=ROOT, check=False)
    shutil.copy2(os.path.join(ROOT, 'build', 'ext2.img'),
                 os.path.join(ROOT, 'build', 'ext2_pristine.img'))
    shutil.copy2(os.path.join(ROOT, 'build', 'ext2_4k.img'),
                 os.path.join(ROOT, 'build', 'ext2_4k_pristine.img'))

    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-hda build/fat32.img -hdb build/ext2.img '
                 '-device ich9-ahci,id=ahci '
                 '-drive id=e2,file=build/ext2_4k.img,format=raw,if=none '
                 '-device ide-hd,drive=e2,bus=ahci.0\n')

    cmds = [
        'mount ext2 4 /mnt/e2',
        'ls /mnt/e2',
        'ls /mnt/ext2',
        'cat /mnt/ext2/HELLO.TXT',
        'write /mnt/e2/TWO.TXT hello from volume two',
        'cat /mnt/e2/TWO.TXT',
        'umount /mnt/e2',
    ]
    keys = ';'.join(keys_for(c) for c in cmds)
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_multivol_ext2.log',
                    '-ArgFile', 'build/qemu_ext2two.args',
                    '-BootWaitSeconds', '55', '-MonitorPort', '45571',
                    '-Keys', keys, '-AfterKeysSeconds', '5'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = [
        ('boot auto-mounted the first ext2 volume at /mnt/ext2',
         '[ext2] vfs mount' in log and '/mnt/ext2' in log),
        ('second ext2 volume mounted as volume slot 1',
         '[ext2] vfs mount drive=4 at /mnt/e2 (volume slot 1)' in log),
        ('/mnt/e2 lists its own files', 'HELLO.TXT' in log and 'SUB' in log),
        ('/mnt/ext2 keeps its selftest artifact (volumes did not mix)',
         'WRT_KEEP.BIN' in log),
        ('write to /mnt/e2 succeeded', 'write: /mnt/e2/TWO.TXT' in log),
        ('read back the written text', 'hello from volume two' in log),
        ('umount of the second volume succeeded', 'Unmounted /mnt/e2' in log),
    ]

    try:
        import verify_fat32_write  # noqa: F401  (keeps the sys.path import shape)
        r1 = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'verify_ext2_write.py'),
                             os.path.join(ROOT, 'build', 'ext2.img'),
                             '--clean', os.path.join(ROOT, 'build', 'ext2_pristine.img')],
                            capture_output=True, text=True)
        tail1 = [l for l in r1.stdout.splitlines() if 'checks=' in l]
        checks.append(('host: volume 1 still passes the ext2 write verifier (40/40)',
                       'checks=40 ok=40 fail=0' in r1.stdout,
                       tail1[-1].strip() if tail1 else ''))
        r2 = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'verify_ext2_read.py'),
                             os.path.join(ROOT, 'build', 'ext2_4k_pristine.img')],
                            capture_output=True, text=True)
        tail2 = [l for l in r2.stdout.splitlines() if 'checks=' in l]
        checks.append(('host: the volume-2 fixture is a valid ext2 image (35/35)',
                       'checks=35 ok=35 fail=0' in r2.stdout,
                       tail2[-1].strip() if tail2 else ''))
        dirty = open(os.path.join(ROOT, 'build', 'ext2_4k.img'), 'rb').read()
        pristine = open(os.path.join(ROOT, 'build', 'ext2_4k_pristine.img'), 'rb').read()
        changed = sum(1 for a, b in zip(dirty, pristine) if a != b)
        checks.append(('host: the write landed on volume 2 (%d bytes changed)' % changed,
                       changed > 0))
        # the written text must be inside the second volume's image
        checks.append(('host: volume 2 image contains the written text',
                       b'hello from volume two' in dirty))
        checks.append(('host: volume 1 image does NOT contain volume 2 text',
                       b'hello from volume two' not in open(
                           os.path.join(ROOT, 'build', 'ext2.img'), 'rb').read()))
    except Exception as exc:                       # noqa: BLE001
        checks.append(('host-side verification', False, repr(exc)))

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
