#!/usr/bin/env python3
"""SutureOS `sync` acceptance: a written file reaches the disk WITHOUT umount.

Before v0.28.0 the FAT32 FSInfo (and the ext2 counters) were only flushed when
the volume was unmounted, so a file written from the shell could be invisible to
the host until then. This test writes a file, runs `sync`, and then checks the raw
image on the HOST without unmounting anything.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/sync_acceptance.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_sync.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ata.args')
PATH = '/mnt/fat/SYNC.TXT'
TEXT = 'written then synced'

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
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')

    cmds = ['write %s %s' % (PATH, TEXT), 'sync', 'cat %s' % PATH]
    # NOTE: no umount on purpose - sync alone must make the write durable
    keys = ';'.join(keys_for(c) for c in cmds)
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_sync.log',
                    '-ArgFile', 'build/qemu_ata.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45581',
                    '-Keys', keys, '-AfterKeysSeconds', '4'], cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    want = (TEXT + '\n').encode()
    checks = [
        ('write reported the byte count',
         'write: %s (%d bytes)' % (PATH, len(want)) in log),
        ('sync flushed the FAT32 mount',
         'sync: /mnt/fat (fat32) ok' in log),
        ('sync summary reports zero failures', 'sync: 0 failed' in log or
         ('sync: 1 ok, 0 failed' in log) or ('ok, 0 failed' in log)),
        ('cat reads the text back in the guest', TEXT in log),
    ]

    try:
        sys.path.insert(0, os.path.join(ROOT, 'scripts'))
        import verify_fat32_write as vf
        img = open(os.path.join(ROOT, 'build', 'fat32_ata.img'), 'rb').read()
        fs = vf.parse(img)
        ent = fs['entries'].get('SYNC.TXT')
        checks.append(('host: SYNC.TXT is on the raw image (no umount happened)',
                       ent is not None))
        if ent:
            data = fs['read_chain'](ent[0], ent[1])[:len(want)]
            checks.append(('host: SYNC.TXT content is byte-for-byte the written text',
                           data == want, repr(data)))
    except Exception as exc:                       # noqa: BLE001
        checks.append(('host-side FAT32 parse', False, repr(exc)))

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
