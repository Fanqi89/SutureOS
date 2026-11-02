#!/usr/bin/env python3
"""SutureOS wget acceptance: download a real file from a real HTTP server.

The host runs a tiny HTTP server on 127.0.0.1:8089; QEMU's user-mode network maps
guest `10.0.2.2` onto the host loopback, so the guest's `wget` talks to a real
socket. After the run the FAT32 disk image is parsed on the HOST and the
downloaded bytes are compared with the served bytes - the guest's own log lines
are checked too, but they are not the evidence.

  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/wget_acceptance.py
"""
import hashlib
import http.server
import os
import random
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

PORT = 8089
SERVE_DIR = os.path.join(ROOT, 'build', 'www')
LOG = os.path.join(ROOT, 'build', 'serial_wget.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_net_ata.args')

KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', ':': 'shift-semicolon'}


def keys_for(text):
    out = []
    for ch in text:
        if ch in KEYMAP:
            out.append(KEYMAP[ch])
        elif ch.isupper():
            out.append('shift-' + ch.lower())
        else:
            out.append(ch)
    out.append('ret')
    return ';'.join(out)


def make_payload():
    os.makedirs(SERVE_DIR, exist_ok=True)
    rnd = random.Random(20261102)
    blob = bytes(rnd.randrange(256) for _ in range(3072))
    with open(os.path.join(SERVE_DIR, 'dl.bin'), 'wb') as fh:
        fh.write(blob)
    text = b'hello from the host via wget\n'
    with open(os.path.join(SERVE_DIR, 'hello.txt'), 'wb') as fh:
        fh.write(text)
    return blob, text


def start_server():
    import functools
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=SERVE_DIR)
    httpd = http.server.ThreadingHTTPServer(('0.0.0.0', PORT), handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd


def main():
    blob, text = make_payload()
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-netdev user,id=n0 -device rtl8139,netdev=n0 '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')

    srv = start_server()
    try:
        time.sleep(0.5)
        cmds = [
            'wget http://10.0.2.2:%d/dl.bin /mnt/fat/DL.BIN' % PORT,
            'wget http://10.0.2.2:%d/hello.txt /mnt/fat/HELLO2.TXT' % PORT,
            'cat /mnt/fat/HELLO2.TXT',
        ]
        keys = ';'.join(keys_for(c) for c in cmds)
        powershell = ['powershell', '-NoProfile', '-File',
                      os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                      '-SerialLog', LOG, '-ArgFile', 'build/qemu_net_ata.args',
                      '-BootWaitSeconds', '40', '-Keys', keys,
                      '-AfterKeysSeconds', '6',
]
        print('[host] starting QEMU + HTTP server on port %d' % PORT)
        subprocess.run(powershell, cwd=ROOT, check=False)
    finally:
        srv.shutdown()

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = []

    def check(name, ok, detail=''):
        checks.append((name, bool(ok), detail))

    check('both downloads reported HTTP 200',
          log.count('wget: HTTP 200') >= 2, 'count=%d' % log.count('wget: HTTP 200'))
    check('first download wrote 3072 bytes',
          'wget: wrote 3072 bytes to /mnt/fat/DL.BIN' in log)
    check('second download wrote the text file',
          'wget: wrote %d bytes to /mnt/fat/HELLO2.TXT' % len(text) in log)
    check('cat shows the downloaded text', text.decode().strip() in log)

    # Host-side: parse the raw FAT32 image, compare bytes.
    try:
        import verify_fat32_write as v
        img = open(os.path.join(ROOT, 'build', 'fat32_ata.img'), 'rb').read()
        fs = v.parse(img)
        ent = fs['entries'].get('DL.BIN')
        check('host: DL.BIN exists on the image', ent is not None)
        if ent:
            data = fs['read_chain'](ent[0], ent[1])[:len(blob)]
            check('host: DL.BIN is byte-for-byte the served binary',
                  data == blob,
                  'sha256 %s vs %s' % (hashlib.sha256(data).hexdigest()[:16],
                                       hashlib.sha256(blob).hexdigest()[:16]))
        ent2 = fs['entries'].get('HELLO2.TXT')
        check('host: HELLO2.TXT exists on the image', ent2 is not None)
        if ent2:
            data2 = fs['read_chain'](ent2[0], ent2[1])[:len(text)]
            check('host: HELLO2.TXT is byte-for-byte the served text', data2 == text,
                  repr(data2[:40]))
    except Exception as exc:                     # noqa: BLE001
        check('host-side FAT32 parse', False, repr(exc))

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
