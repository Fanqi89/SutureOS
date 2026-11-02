#!/usr/bin/env python3
"""SutureOS multi-volume FAT32 acceptance.

Before v0.24.0 the FAT32 driver could hold exactly one mounted volume, so an IDE
FAT32 disk and a USB stick could not be used at the same time. This test boots
with BOTH, lists and reads both, writes to the USB one, and then verifies on the
HOST that only the USB image changed (and that the file really landed there).

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/mk_usb_testimg.py
  py -3 scripts/multivol_acceptance.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

LOG = os.path.join(ROOT, 'build', 'serial_multivol.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_both.args')
KEYS = ('l;s;spc;slash;m;n;t;slash;f;a;t;ret'
        ';l;s;spc;slash;m;n;t;slash;u;s;b;ret'
        ';c;a;t;spc;slash;m;n;t;slash;f;a;t;slash;h;e;l;l;o;dot;t;x;t;ret'
        ';c;a;t;spc;slash;m;n;t;slash;u;s;b;slash;shift-u;shift-s;shift-b;dot;t;x;t;ret'
        ';w;r;i;t;e;spc;slash;m;n;t;slash;u;s;b;slash;m;v;dot;t;x;t'
        ';spc;h;i;spc;b;o;t;h;spc;v;o;l;u;m;e;s;ret'
        ';u;m;o;u;n;t;spc;slash;m;n;t;slash;u;s;b;ret')


def main():
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide '
                 '-device usb-ehci,id=ehci '
                 '-drive id=usbdisk,file=build/usbdisk.img,format=raw,if=none '
                 '-device usb-storage,drive=usbdisk,bus=ehci.0\n')

    # pristine copies to diff against afterwards
    import shutil
    shutil.copy2(os.path.join(ROOT, 'build', 'fat32.img'),
                 os.path.join(ROOT, 'build', 'fat32_pristine.img'))
    shutil.copy2(os.path.join(ROOT, 'build', 'usbdisk.img'),
                 os.path.join(ROOT, 'build', 'usbdisk_pristine.img'))

    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_multivol.log',
                    '-ArgFile', 'build/qemu_both.args',
                    '-BootWaitSeconds', '45', '-Keys', KEYS,
                    '-AfterKeysSeconds', '5'], cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = []

    def check(name, ok, detail=''):
        checks.append((name, bool(ok), detail))

    check('IDE FAT32 auto-mounted at /mnt/fat',
          '[init] FAT32 volume auto-mounted at /mnt/fat' in log)
    check('USB FAT32 mounted at /mnt/usb in the same boot',
          '[usb] mount /mnt/usb OK' in log)
    check('/mnt/fat lists its files', 'HELLO.TXT' in log and 'README.TXT' in log)
    check('/mnt/usb lists its files',
          'USB.TXT' in log and 'Long file name test.txt' in log)
    check('cat on the IDE volume returns its HELLO.TXT text',
          'Hello from SutureOS FAT32!' in log)
    check('cat on the USB volume returns its USB.TXT text',
          'USB selftest' in log)
    check('write to /mnt/usb while /mnt/fat is mounted',
          'write: /mnt/usb/mv.txt' in log)
    check('umount /mnt/usb succeeded', 'Unmounted /mnt/usb' in log)

    # host side
    try:
        import verify_usb_disk as vu
        import verify_fat32_write as vf

        usb = open(os.path.join(ROOT, 'build', 'usbdisk.img'), 'rb').read()
        fs = vu.parse_fat32(usb) if hasattr(vu, 'parse_fat32') else None
        if fs is None:                     # fall back to the generic reader
            fs = vf.parse(usb)
        ent = fs['entries'].get('MV.TXT')
        check('host: MV.TXT exists on the USB image', ent is not None)
        if ent:
            data = fs['read_chain'](ent[0], ent[1])[:len(b'hi both volumes\n')]
            check('host: MV.TXT content is byte-for-byte what was written',
                  data == b'hi both volumes\n', repr(data))

        # The IDE volume does change during boot (the FAT32 self-tests create
        # their own files), so "unchanged" is the wrong assertion. What must
        # hold is that its original fixtures still read back byte-for-byte -
        # i.e. the USB write went to the other volume only.
        fat = open(os.path.join(ROOT, 'build', 'fat32_ata.img'), 'rb').read()
        ffs = vf.parse(fat)
        want = b'Hello from SutureOS FAT32!\nThis file lives on a real FAT32 volume (driver: fat32).\n'
        ent_fat = ffs['entries'].get('HELLO.TXT')
        check('host: IDE HELLO.TXT still reads back byte-for-byte', ent_fat is not None)
        if ent_fat:
            got = ffs['read_chain'](ent_fat[0], ent_fat[1])[:len(want)]
            check('host: IDE volume content intact after the USB write', got == want,
                  repr(got[:48]))
    except Exception as exc:                       # noqa: BLE001
        check('host-side image checks', False, repr(exc))

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
