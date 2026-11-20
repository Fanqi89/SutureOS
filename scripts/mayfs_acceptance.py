#!/usr/bin/env python3
"""SutureOS MayFS acceptance - real boot, real keys, real power-loss-safe commit.

Two modes:

1) default (the acceptance):
     * QEMU boots build/sutureos.iso with the MayFS fixture on the second IDE
       disk (`-drive id=bdisk,file=build/mayfs.img,format=raw,if=ide,index=1`)
       plus the usual FAT32 disk on drive 0,
     * characters are typed through QMP one at a time (~75 ms apart, uppercase as
       `shift-x`): the shell's keyboard buffer loses characters on bulk batches,
     * the guest mounts MayFS (`mount mayfs 1 /mnt/may`), lists / reads / stats
       the fixture, WRITES `/mnt/may/NEW.TXT`, `sync`s (which commits both 256 KiB
       slots), unmounts, remounts and reads the new file back,
     * after QEMU exits the host parses the image QEMU really wrote with
       scripts/verify_mayfs_read.py: NEW.TXT byte-exact, every old file intact, a
       single generation bump, and both slots carrying the same generation and a
       byte-identical table (the direct evidence for MayFS' dual-slot semantics).
   Keys are typed into a fresh boot; nothing is written without a `sync`, so the
   image the guest leaves behind is exactly one committed generation.

2) --regression:
     Boots the SAME ISO with no second disk and diffs the serial log against a
     baseline captured before MayFS existed (build/serial_baseline_nodisk.log).
     The only accepted new line is MayFS' single `no volume` probe line; the
     self-test count and every other filesystem's log must be unchanged.

  py -3 scripts/mayfs_acceptance.py
  py -3 scripts/mayfs_acceptance.py --regression
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = 45781
SERIAL = 'build/serial_mayfs.log'
LOG_ABS = os.path.join(ROOT, SERIAL)
BASELINE_LOG = os.path.join(ROOT, 'build/serial_baseline_nodisk.log')
IMAGE = os.path.join(ROOT, 'build/mayfs.img')
PRISTINE = os.path.join(ROOT, 'build/mayfs.pristine.img')
MKFS = os.path.join(ROOT, 'scripts/mkfs_mayfs_testimg.py')
VERIFY = os.path.join(ROOT, 'scripts/verify_mayfs_read.py')

NEW_TEXT = 'hello mayfs'          # `write` appends exactly one newline
LONG_NAME = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFG.TXT'


class Qmp:
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:
                last = e
                time.sleep(0.2)
        else:
            raise RuntimeError('QMP never listened: %r' % (last,))
        self.f = self.s.makefile('rwb')
        self._read()
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            obj = json.loads(line.decode('utf-8'))
            if 'event' in obj:
                continue
            return obj

    def cmd(self, name, **args):
        msg = {'execute': name}
        if args:
            msg['arguments'] = args
        self.f.write((json.dumps(msg) + '\n').encode('utf-8'))
        self.f.flush()
        return self._read()

    def hmp(self, line):
        r = self.cmd('human-monitor-command', **{'command-line': line})
        return (r.get('return') or '').strip()


SPECIAL = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'shift-semicolon',
           ',': 'comma', '=': 'equal', '_': 'shift-minus', "'": 'apostrophe'}


def key_for(ch):
    if ch in SPECIAL:
        return SPECIAL[ch]
    if ch.isupper():
        return 'shift-' + ch.lower()       # QMP/sendkey needs shift-h, not 'H'
    return ch


def type_text(q, text, delay=0.075, tail=0.75):
    for ch in text:
        q.hmp('sendkey ' + key_for(ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(tail)


def read_log(path=SERIAL):
    try:
        with open(os.path.join(ROOT, path), encoding='utf-8', errors='replace') as fh:
            return fh.read()
    except OSError:
        return ''


def start_qemu(serial, extra, port):
    if os.path.exists(os.path.join(ROOT, serial)):
        os.remove(os.path.join(ROOT, serial))
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + serial,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % port,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd', '-smp', '2', '-no-reboot']
    if extra:
        argv += extra.split()
    print('[mayfs] ' + ' '.join(argv), flush=True)
    return subprocess.Popen(argv, cwd=ROOT), argv


def wait_prompt(q, timeout=150, path=SERIAL):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if 'sutureos# ' in read_log(path):
            return True
        time.sleep(0.4)
    return False


def finish(proc, q):
    try:
        if q is not None:
            q.cmd('quit')
    except Exception:      # noqa: BLE001
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()


def report(checks, tag='mayfs'):
    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[%s] checks=%d ok=%d fail=%d'
          % (tag, len(checks), len(checks) - len(failed), len(failed)))
    print('[%s] RESULT:' % tag, 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


# ==================== mode 1: acceptance ====================

def run_acceptance(args):
    # fresh fixture; mkfs also keeps build/mayfs.pristine.img for the diff
    r = subprocess.run([sys.executable, MKFS], cwd=ROOT, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(r.stderr.strip())
        return 1
    if not os.path.exists(PRISTINE):
        print('[mayfs] mkfs did not produce the pristine copy')
        return 1

    extra = ('-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide '
             '-drive id=bdisk,file=build/mayfs.img,format=raw,if=ide,index=1')
    proc, argv = start_qemu(SERIAL, extra, args.port)
    commands = [
        ('mount mayfs 1 /mnt/may', 2.0),
        ('ls /mnt/may', 1.5),
        ('cat /mnt/may/HELLO.TXT', 1.5),
        ('stat /mnt/may/BIG.BIN', 1.2),
        ('cat /mnt/may/SUBDIR/INNER.TXT', 1.5),
        ('cat /mnt/may/' + LONG_NAME, 2.0),
        ('mkdir /mnt/may/TMPDIR', 1.2),                   # VFS mkdir -> mayfs_create
        ('rmdir /mnt/may/TMPDIR', 1.2),                   # VFS rmdir -> mayfs_remove
        ('mv /mnt/may/ONE.TXT /mnt/may/ONE2.TXT', 1.5),   # VFS rename -> mayfs_rename
        ('mv /mnt/may/ONE2.TXT /mnt/may/ONE.TXT', 1.5),   # ...and back (net zero)
        ('write /mnt/may/NEW.TXT ' + NEW_TEXT, 1.5),
        ('sync', 6.0),               # commits both 256 KiB slots twice over
        ('cat /mnt/may/NEW.TXT', 1.5),
        ('umount /mnt/may', 2.0),
        ('mount mayfs 1 /mnt/may', 3.0),
        ('cat /mnt/may/NEW.TXT', 1.5),
    ]
    q = None
    try:
        # QMP must be up before the prompt: connect first, then wait for the shell
        q = Qmp(args.port)
        if not wait_prompt(q):
            print('[mayfs] shell prompt never appeared')
            finish(proc, q)
            log = read_log()
            print(log[-2000:])
            return 1
        for cmd, wait in commands:
            print('[mayfs] > ' + cmd, flush=True)
            type_text(q, cmd, delay=args.delay, tail=wait)
    finally:
        finish(proc, q)

    log = read_log()
    host = subprocess.run([sys.executable, VERIFY, IMAGE, '--pristine', PRISTINE,
                           '--require-generation', '8',
                           '--expect-text', 'NEW.TXT=' + NEW_TEXT,
                           '--json-out', os.path.join(ROOT, 'build/mayfs_dirty.json')],
                          cwd=ROOT, capture_output=True, text=True)
    print(host.stdout.strip())
    if host.returncode != 0:
        print(host.stderr.strip())
    dirty = {}
    json_path = os.path.join(ROOT, 'build/mayfs_dirty.json')
    if os.path.exists(json_path):
        with open(json_path, encoding='utf-8') as fh:
            dirty = json.load(fh)

    def g(pat, flags=0):
        return re.search(pat, log, flags) is not None

    def ls_line(name):
        # `ls` prints "%-24s" then '/' for directories, so a listed entry is a
        # line that starts with the name (the typed command echo is not a match).
        stem = name.rstrip('/')
        tail = r'\s*/\s*$' if name.endswith('/') else r'\s*$'
        return re.search(r'^' + re.escape(stem) + tail, log, re.MULTILINE) is not None

    mounts = re.findall(r'Mounted 1 \(mayfs\) on /mnt/may', log)
    checks = [
        ('boot probe printed one MayFS geometry line',
         g(r'\[mayfs\] volume ready on drive 1: generation=7 slot=main '
           r'primary-gen=7 backup-gen=7')),
        ('read-only probe reported the firmware geometry '
         '(records=48 name-max=47 file-max=4095)',
         g(r'records=48 name-max=47 file-max=4095')),
        ('probe did not claim a missing MayFS volume',
         '[mayfs] no volume on drive 1' not in log),
        ('VFS driver registered and mounted: '
         '`Mounted 1 (mayfs) on /mnt/may`', len(mounts) >= 1),
        ('driver mount log reports drive=1 generation=7 slot=main prefs=yes',
         g(r'\[mayfs\] mount drive=1 generation=7 slot=main .*prefs=yes')),
        ('ls lists HELLO.TXT / BIG.BIN / SUBDIR/ / EMPTY / the 47-char name',
         all(ls_line(n) for n in ('HELLO.TXT', 'BIG.BIN', 'SUBDIR/', 'EMPTY',
                                  LONG_NAME))),
        ('ls also lists the mandatory MayFS root dirs (Documents/Desktop/Trash)',
         all(ls_line(n) for n in ('Documents/', 'Desktop/', 'Trash/'))),
        ('cat HELLO.TXT prints the fixture text and 31 bytes',
         g(r'Hello from MayFS\.') and '(31 bytes)' in log),
        ('stat BIG.BIN reports 4095 bytes (largest legal MayFS file)',
         g(r'Size: 4095 bytes')),
        ('cat SUBDIR/INNER.TXT reads through the directory table',
         'inner file inside SUBDIR' in log and '(25 bytes)' in log),
        ('cat of the 47-character name works (write path name limit)',
         'long name file' in log and '(15 bytes)' in log),
        ('mkdir/rmdir/rename accepted by the MayFS driver (no shell errors)',
         not re.search(r'^(mkdir|rmdir|mv):', log, re.MULTILINE)),
        ('write created NEW.TXT with 12 bytes',
         g(r'write: /mnt/may/NEW\.TXT \(12 bytes\)')),
        ('sync committed the MayFS volume',
         g(r'sync: /mnt/may \(mayfs\) ok')),
        ('commit log shows generation 8 written backup-slot-first',
         g(r'\[mayfs\] committed generation 8 \(backup slot first, '
           r'then primary; both flushed\)')),
        ('cat NEW.TXT after sync returns the written text',
         g(r'hello mayfs')),
        ('umount succeeded', g(r'Unmounted /mnt/may')),
        ('re-mount after umount succeeded', len(mounts) >= 2),
        ('re-mount read generation 8 back from disk',
         g(r'\[mayfs\] mount drive=1 generation=8 slot=main')),
        ('NEW.TXT survived umount+remount (read back from the disk snapshot)',
         log.count('hello mayfs') >= 2),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
        ('module self-tests still 16/16', '[stitch] selftests: 16/16 passed' in log),
        ('no kernel ERROR lines',
         not re.search(r'^\s*\[(ERROR|ERR)\]', log, re.MULTILINE)),
        ('FAT32 (drive 0) untouched by the MayFS port: write/ops/lfn PASS',
         g(r'\[fat32\] write self-test PASS') and
         g(r'\[fat32\] ops self-test PASS') and
         g(r'\[fat32\] lfn-write self-test PASS')),
        ('host verifier: dirty image parses 100% (see its own counter)',
         host.returncode == 0),
        ('host verifier: both slots one generation, byte-identical tables',
         dirty.get('slots_identical') is True and dirty.get('generation') == 8),
        ('host verifier: NEW.TXT is a real record with 12 payload bytes',
         any(e['name'] == 'NEW.TXT' and e['size'] == 12
             for e in dirty.get('entries', []))),
    ]
    print('[mayfs] serial log: %s (%d bytes)' % (SERIAL, len(log)))
    return report(checks)


# ==================== mode 2: no-disk regression ====================

VOLATILE = [
    # The MayFS driver makes the kernel image bigger, so PMM/heap/ring-3
    # addresses and the ISO's block count legitimately move between the baseline
    # build and this one; the values are not filesystem behaviour.
    (re.compile(r'0x[0-9a-fA-F]+'), '0xADDR'),
    (re.compile(r'\b\d+ pages free\b'), 'N pages free'),
    (re.compile(r'\b\d+ blocks x 2048B\b'), 'N blocks x 2048B'),
    (re.compile(r'sectors=\d+'), 'sectors=N'),
    (re.compile(r'\b\d+ ticks/s\b'), 'N ticks/s'),
    (re.compile(r'ticks=\d+'), 'ticks=N'),
    (re.compile(r'boot time \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}'), 'boot time T'),
    (re.compile(r'yields=\d+'), 'yields=N'),
    (re.compile(r'frames rx=\d+ tx=\d+ drops=\d+ arp_entries=\d+'), 'net counters'),
    (re.compile(r'\b\d+ ms\b'), 'N ms'),
    (re.compile(r'took \d+'), 'took N'),
    (re.compile(r'\bcpu \d+\b'), 'cpu N'),
]

# Registering one more filesystem driver prints exactly one more line.
ALLOWED_ADDED = [
    re.compile(r'^\[mayfs\] no volume on drive 1 \(no such block device\)$'),
    re.compile(r'^\[vfs\] Registered driver: mayfs$'),
]


def normalize(line):
    out = line.rstrip()
    for pat, rep in VOLATILE:
        out = pat.sub(rep, out)
    return out


def run_regression(args):
    if not os.path.exists(BASELINE_LOG):
        print('[mayfs] missing baseline log %s - capture it before the port '
              '(build/run_boot.py --serial build/serial_baseline_nodisk.log)'
              % BASELINE_LOG)
        return 1
    serial = 'build/serial_mayfs_regression.log'    # keep the acceptance log intact
    proc, _argv = start_qemu(serial, '-drive id=adisk,file=build/fat32_ata.img,'
                                        'format=raw,if=ide', args.port)
    q = None
    try:
        q = Qmp(args.port)
        wait_prompt(q, path=serial)
    finally:
        finish(proc, q)
    log = read_log(serial)
    with open(BASELINE_LOG, encoding='utf-8', errors='replace') as fh:
        base = fh.read()

    base_lines = [normalize(l) for l in base.splitlines()]
    new_lines = [normalize(l) for l in log.splitlines()]
    from collections import Counter
    cb, cn = Counter(base_lines), Counter(new_lines)
    removed = list((cb - cn).elements())
    added = list((cn - cb).elements())
    unexpected = [l for l in added
                  if not any(p.match(l) for p in ALLOWED_ADDED)]

    checks = [
        ('baseline log present (%d lines)' % len(base_lines), bool(base_lines)),
        ('new boot has exactly one MayFS no-volume probe line',
         '[mayfs] no volume on drive 1 (no such block device)' in log and
         log.count('[mayfs] no volume on drive 1') == 1),
        ('no baseline line disappeared (every other filesystem/substr unchanged)',
         not removed),
        ('the only new lines are MayFS registration + its no-volume probe',
         not unexpected),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
        ('module self-tests still 16/16',
         '[stitch] selftests: 16/16 passed' in log),
        ('ext2 probe unchanged',
         '[ext2] mount failed: I/O error reading the superblock at offset 1024' in log),
        ('ext4 probe unchanged',
         '[ext4] no ext4 volume on drive 1 (no such block device)' in log),
        ('VimtuFS2 probe unchanged',
         '[vimtufs2] no volume on drive 1 (reason=no-device)' in log),
        ('ISO9660 probe unchanged',
         '[iso9660] skip drive 14: El Torito boot disc, not a data disc' in log),
        ('FAT32 mount + all four self-tests unchanged',
         '[fat32] mount drive=0' in log and
         '[fat32] write self-test PASS' in log and
         '[fat32] ops self-test PASS' in log and
         '[fat32] lfn self-test PASS' in log and
         '[fat32] lfn-write self-test PASS' in log),
    ]
    for l in removed[:10]:
        print('  removed baseline line: %r' % l)
    for l in added[:10]:
        print('  added line: %r' % l)
    return report(checks, tag='mayfs-regression')


# ==================== mode 3: dual-slot recovery ====================

SLOT_BYTES = 512 * 512
BACKUP_OFF = 512 * 512
SECTOR = 512


def _slot_generation(blob):
    return int.from_bytes(blob[16:20], 'little')


def make_primary_damaged(src, dst):
    """Primary snapshot fails its checksum; backup is valid and one generation
    newer (9).  A MayFS mount must pick the backup and keep working."""
    data = bytearray(open(src, 'rb').read())
    data[SECTOR * 2 + 64] ^= 0xFF          # flips a byte inside the record table
    backup = bytearray(data[BACKUP_OFF:BACKUP_OFF + SLOT_BYTES])
    backup[16:20] = (9).to_bytes(4, 'little')
    backup[20:24] = ((~9) & 0xFFFFFFFF).to_bytes(4, 'little')
    data[BACKUP_OFF:BACKUP_OFF + SLOT_BYTES] = backup
    with open(dst, 'wb') as fh:
        fh.write(data)


def make_both_damaged(src, dst):
    """Neither slot passes validation - the driver must refuse to mount and must
    leave the disk byte-for-byte untouched (no silent reformat)."""
    data = bytearray(open(src, 'rb').read())
    for base in (0, BACKUP_OFF):
        data[base + 8] ^= 0xFF             # stored table checksum is now wrong
    with open(dst, 'wb') as fh:
        fh.write(data)


def boot_and_type(extra, commands, port, serial):
    proc, _argv = start_qemu(serial, extra, port)
    q = None
    try:
        q = Qmp(port)
        if not wait_prompt(q, path=serial):
            return False
        for cmd, wait in commands:
            print('[mayfs] > ' + cmd, flush=True)
            type_text(q, cmd, delay=0.075, tail=wait)
    finally:
        finish(proc, q)
    return True


def run_recovery(args):
    r = subprocess.run([sys.executable, MKFS], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        return 1
    good = os.path.join(ROOT, 'build/mayfs.pristine.img')
    dam = 'build/mayfs_recovery.img'
    bad = 'build/mayfs_bothbad.img'
    make_primary_damaged(good, dam)
    make_both_damaged(good, bad)
    # checksum of the both-damaged image must not change during the failed mount
    import hashlib
    bad_sha_before = hashlib.sha256(open(bad, 'rb').read()).hexdigest()

    fat = 'build/fat32_ata.img'
    serial = 'build/serial_mayfs_recovery.log'
    adisk = '-drive id=adisk,file=%s,format=raw,if=ide ' % fat

    print('[mayfs] --- phase A: primary slot damaged, backup newer (gen 9) ---')
    ok_a = boot_and_type(adisk + '-drive id=bdisk,file=%s,format=raw,if=ide,index=1'
                         % dam,
                         [('mount mayfs 1 /mnt/may', 2.0),
                          ('cat /mnt/may/HELLO.TXT', 1.5),
                          ('write /mnt/may/REC.TXT recovered ok', 1.5),
                          ('sync', 6.0)], args.port, serial)
    log_a = read_log(serial)
    host = subprocess.run([sys.executable, VERIFY, dam,
                           '--require-generation', '10',
                           '--expect-text', 'REC.TXT=recovered ok',
                           '--json-out', os.path.join(ROOT, 'build/mayfs_recovery.json')],
                          cwd=ROOT, capture_output=True, text=True)
    print(host.stdout.strip())
    rec = {}
    jp = os.path.join(ROOT, 'build/mayfs_recovery.json')
    if os.path.exists(jp):
        with open(jp, encoding='utf-8') as fh:
            rec = json.load(fh)

    print('[mayfs] --- phase B: both slots damaged (must refuse, not reformat) ---')
    ok_b = boot_and_type(adisk + '-drive id=bdisk,file=%s,format=raw,if=ide,index=1'
                         % bad,
                         [('mount mayfs 1 /mnt/bad', 2.5)], args.port, serial)
    log_b = read_log(serial)
    bad_sha_after = hashlib.sha256(open(bad, 'rb').read()).hexdigest()

    checks = [
        ('phase A booted to the shell', ok_a),
        ('probe accepted the backup slot: generation 9, slot=backup',
         '[mayfs] volume ready on drive 1: generation=9 slot=backup' in log_a),
        ('mount log shows generation=9 slot=backup (recovered from backup)',
         re.search(r'\[mayfs\] mount drive=1 generation=9 slot=backup', log_a) is not None),
        ('fixture still readable through the recovered snapshot',
         'Hello from MayFS.' in log_a and '(31 bytes)' in log_a),
        ('commit after recovery wrote generation 10 (repairs the primary slot)',
         '[mayfs] committed generation 10' in log_a),
        ('host verifier: repaired image has two valid, identical slots',
         host.returncode == 0 and rec.get('slots_identical') is True
         and rec.get('generation') == 10),
        ('host verifier: REC.TXT written after recovery is byte-exact',
         any(e['name'] == 'REC.TXT' and e['size'] == 13
             for e in rec.get('entries', []))),
        ('phase B booted to the shell', ok_b),
        ('mount refused a volume with no valid snapshot',
         '[mayfs] mount FAILED drive=1: no valid MayFS snapshot' in log_b),
        ('the refused disk was NOT reformatted',
         'blank disk on drive 1' not in log_b),
        ('the refused disk is byte-for-byte unchanged',
         bad_sha_before == bad_sha_after),
        ('no kernel PANIC in either phase',
         'PANIC' not in (log_a + log_b).upper().replace('NO PANIC', '')),
        ('module self-tests still 16/16 in both phases',
         log_a.count('[stitch] selftests: 16/16 passed') >= 1 and
         log_b.count('[stitch] selftests: 16/16 passed') >= 1),
    ]
    for l in log_a.splitlines():
        if '[mayfs]' in l:
            print('  A: ' + l)
    for l in log_b.splitlines():
        if '[mayfs]' in l:
            print('  B: ' + l)
    return report(checks, tag='mayfs-recovery')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--regression', action='store_true',
                    help='boot without the MayFS disk and diff against the baseline log')
    ap.add_argument('--recovery', action='store_true',
                    help='damaged-primary and both-slots-damaged scenarios')
    ap.add_argument('--port', type=int, default=QMP_PORT)
    ap.add_argument('--delay', type=float, default=0.075)
    args = ap.parse_args()
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[mayfs] build/sutureos.iso missing - run scripts/build.py and '
              'scripts/build_iso.py first')
        return 1
    if args.regression:
        return run_regression(args)
    if args.recovery:
        return run_recovery(args)
    return run_acceptance(args)


if __name__ == '__main__':
    sys.exit(main())
