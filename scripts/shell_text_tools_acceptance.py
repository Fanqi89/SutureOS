#!/usr/bin/env python3
"""SutureOS shell text tools acceptance (write / append / hexdump).

Boots with the FAT32 test disk, types the three commands through QMP, checks the
transcript, then re-parses the raw image on the host (never trusting the guest)
to confirm NOTE.TXT really contains the two lines that were written.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/shell_text_tools_acceptance.py
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from imgview_acceptance import Qmp          # noqa: E402  (QMP helper)

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = 47991
LOG = 'build/serial_texttools.log'
KEYMAP = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '_': 'shift-minus'}

LINE1 = 'hello shell text tools'
LINE2 = 'second line'
COMMANDS = [
    'write /mnt/fat/NOTE.TXT ' + LINE1,
    'append /mnt/fat/NOTE.TXT ' + LINE2,
    'cat /mnt/fat/NOTE.TXT',
    'hexdump /mnt/fat/NOTE.TXT 0 32',
]


def main():
    log_abs = os.path.join(ROOT, LOG)
    if os.path.exists(log_abs):
        os.remove(log_abs)
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = subprocess.Popen(argv, cwd=ROOT)

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    try:
        q = Qmp(PORT)
        for _ in range(150):
            if 'sutureos# ' in log_text():
                break
            time.sleep(1)
        for cmd in COMMANDS:
            time.sleep(0.6)
            for ch in cmd:
                if 'A' <= ch <= 'Z':
                    key = 'shift-' + ch.lower()
                else:
                    key = KEYMAP.get(ch, ch)
                q.hmp('sendkey ' + key)
                time.sleep(0.05)
            q.hmp('sendkey ret')
            time.sleep(1.2)
        time.sleep(2.0)
    finally:
        try:
            q.hmp('quit')
        except Exception:
            pass
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = log_text()
    checks = []

    def check(name, ok, detail=''):
        checks.append((name, bool(ok), detail))

    check('write reported the byte count',
          'write: /mnt/fat/NOTE.TXT (%d bytes)' % (len(LINE1) + 1) in log)
    check('append reported the grown size',
          'append: /mnt/fat/NOTE.TXT now %d bytes' %
          (len(LINE1) + 1 + len(LINE2) + 1) in log)
    check('cat shows the first line', LINE1 in log)
    check('cat shows the appended line', LINE2 in log)
    check('hexdump printed an offset column and ASCII column',
          '|hello shell' in log and '(32 bytes at offset 0)' in log)

    # Host side: re-parse the raw FAT32 image and compare bytes.
    want = (LINE1 + '\n' + LINE2 + '\n').encode()
    try:
        sys.path.insert(0, os.path.join(ROOT, 'scripts'))
        import verify_fat32_write as v
        img = open(os.path.join(ROOT, 'build', 'fat32_ata.img'), 'rb').read()
        fs = v.parse(img)
        ent = fs['entries'].get('NOTE.TXT')
        check('host: NOTE.TXT exists on the raw image', ent is not None)
        if ent:
            data = fs['read_chain'](ent[0], ent[1])
            check('host: NOTE.TXT content is byte-for-byte what was written',
                  data == want, '%r != %r' % (data, want))
        check('host: the pre-existing fixture files survived',
              all(k in fs['entries'] for k in ('HELLO.TXT', 'README.TXT')))
    except Exception as exc:                     # noqa: BLE001 - report as failure
        check('host-side FAT32 parse', False, repr(exc))

    print('--- checks ---')
    ok_all = True
    for name, ok, detail in checks:
        print('  [%s] %s%s' % ('ok' if ok else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))
        ok_all = ok_all and ok
    print('RESULT: %s (checks=%d)' % ('PASS' if ok_all else 'FAIL', len(checks)))
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())
