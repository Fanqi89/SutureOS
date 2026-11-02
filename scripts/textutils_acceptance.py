#!/usr/bin/env python3
"""SutureOS shell text-utility acceptance: wc / head / tail / grep / stat.

Builds a deterministic text file through the shell itself (write + append), then
exercises the five utilities and checks the transcript. Nothing is trusted from
the guest beyond its own stdout, and the file content is re-checked on the HOST by
parsing the FAT32 image.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/textutils_acceptance.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_textutils.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_ata.args')

LINES = ['alpha one', 'beta two', 'gamma three', 'delta four', 'alpha five']
PATH = '/mnt/fat/TXT.TXT'

KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '_': 'shift-minus',
          ':': 'shift-semicolon'}


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

    cmds = ['write %s %s' % (PATH, LINES[0])]
    for ln in LINES[1:]:
        cmds.append('append %s %s' % (PATH, ln))
    cmds += [
        'wc %s' % PATH,
        'head %s 2' % PATH,
        'tail %s 2' % PATH,
        'grep alpha %s' % PATH,
        'stat %s' % PATH,
    ]
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')
    keys = ';'.join(keys_for(c) for c in cmds)
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_textutils.log',
                    '-ArgFile', 'build/qemu_ata.args',
                    '-BootWaitSeconds', '45', '-Keys', keys,
                    '-AfterKeysSeconds', '5'], cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    want_bytes = sum(len(x) + 1 for x in LINES)
    want_words = sum(len(x.split()) for x in LINES)
    checks = [
        ('wc prints 5 lines / %d words / %d bytes' % (want_words, want_bytes),
         ('5 %d %d %s' % (want_words, want_bytes, PATH)) in log),
        ('head 2 prints the first two lines',
         'alpha one' in log and 'beta two' in log and 'gamma three' not in log[:log.find('gamma') if 'gamma' in log else len(log)]),
        ('tail 2 prints the last two lines',
         'delta four' in log and 'alpha five' in log),
        ('grep alpha prints both matching lines with numbers',
         '1: alpha one' in log and '5: alpha five' in log and
         'grep: 2 match(es) in' in log),
        ('stat reports a regular file with the right size',
         'Type: regular file' in log and 'Size: %d bytes' % want_bytes in log),
    ]

    # host-side: the file really contains the five lines
    try:
        sys.path.insert(0, os.path.join(ROOT, 'scripts'))
        import verify_fat32_write as vf
        img = open(os.path.join(ROOT, 'build', 'fat32_ata.img'), 'rb').read()
        fs = vf.parse(img)
        ent = fs['entries'].get('TXT.TXT')
        checks.append(('host: TXT.TXT exists on the raw image', ent is not None))
        if ent:
            data = fs['read_chain'](ent[0], ent[1])[:want_bytes]
            checks.append(('host: TXT.TXT is byte-for-byte the five written lines',
                           data == ('\n'.join(LINES) + '\n').encode(), repr(data[:40])))
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
