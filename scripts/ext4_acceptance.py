#!/usr/bin/env python3
"""SutureOS ext4 read-only driver acceptance (real boot, real QEMU, real keys).

Two modes:

  (default) ext4 volume test
      QEMU gets the ISO plus a second IDE disk carrying build/ext4.img:
        -drive id=bdisk,file=build/ext4.img,format=raw,if=ide,index=1   -> drive 1
      The shell is driven character by character over QMP (the SutureOS shell
      input ring is polled, not IRQ buffered: a bulk `sendkey` batch loses keys,
      so every character waits ~75 ms and uppercase letters are sent as
      shift-<letter>):
        mount ext4 1 /mnt/e4      -> [ext4] vfs mount ... (read-only)
        ls /mnt/e4                -> HELLO.TXT BIG.BIN SUBDIR/ EMPTY ...
        cat /mnt/e4/HELLO.TXT     -> file text + "(125 bytes)"
        cat /mnt/e4/SUBDIR/INNER.TXT
        stat /mnt/e4/BIG.BIN      -> Size: 1572864 bytes
        cat /mnt/e4/NOTE4K.TXT    -> "(4096 bytes)"  (4 KiB read; dragging the
                                     1.5 MiB BIG.BIN through the terminal would
                                     only produce a screenful of noise - its
                                     integrity is the host verifier's job)
        cat /mnt/e4/EMPTY         -> "(0 bytes)"
        write /mnt/e4/NOPE.TXT hi -> refused, EPERM (read-only driver)
      Then scripts/verify_ext4_read.py re-parses the image AND the serial log
      (independent of the kernel) and must PASS.

  --regression (also with --all)
      Boots with no ext4 disk at all and proves the port is inert: exactly one
      new "[ext4] no ..." line, the ext2/FAT32/SquashFS/ISO9660/VFS/self-test
      lines identical to the pre-port baseline, self-tests still 16/16 and no
      PANIC.

Usage:
    py -3 scripts/ext4_acceptance.py              # ext4 volume test
    py -3 scripts/ext4_acceptance.py --regression # no-ext4-disk regression
    py -3 scripts/ext4_acceptance.py --all        # both
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
QMP_PORT = 45771
SERIAL = 'build/serial_ext4.log'
LOG_ABS = os.path.join(ROOT, SERIAL)
REG_SERIAL = 'build/serial_ext4_regression.log'
REG_LOG_ABS = os.path.join(ROOT, REG_SERIAL)
EXT4_IMG = os.path.join(ROOT, 'build', 'ext4.img')
MANIFEST = os.path.join(ROOT, 'build', 'ext4_manifest.json')

# The pre-port boot log, filtered to the file-system / self-test lines, in the
# exact order the kernel emits them (captured from the ISO built BEFORE the ext4
# driver existed). The regression mode asserts this sequence is still there and
# still in order; the only additions allowed are the ext4 driver-registration
# line and the single "[ext4] no ext4 volume" probe line (checked separately).
# Entries are PREFIXES: the FAT32 mount line carries dynamic geometry, so only
# its stable prefix is compared.
REGRESSION_EXPECTED = [
    "[stitch] selftests: 16/16 passed",
    "[init] Initializing block layer + VFS...",
    "[vfs] Registered driver: ext2",
    "[vfs] Registered driver: fat32",
    "[vfs] Registered driver: squashfs",
    "[vfs] Registered driver: iso9660",
    "[vfs] Registered driver: xj_vfs",
    "[vfs] Registered driver: procfs",
    "[vfs] Registered driver: sysfs",
    "[vfs] VFS subsystem initialized",
    "[vfs] Mounted ram on / (xj_vfs)",
    "[vfs] Mounted none on /proc (procfs)",
    "[vfs] Mounted none on /sys (sysfs)",
    "[fat32] mount drive=0",
    "[fat32] vfs mount drive=0 at /mnt/fat",
    "[vfs] Mounted 0 on /mnt/fat (fat32)",
    "[init] FAT32 volume auto-mounted at /mnt/fat (drive 0)",
    "[init] ext2: no ext2 volume on drive 1 (expected without -hdb)",
    "[init] squashfs: no SquashFS volume on drive 1/virtio",
    "[iso9660] skip drive 14: El Torito boot disc, not a data disc",
    "[fat32] write self-test PASS (SUTURE.TXT, 32 bytes)",
    "[fat32] ops self-test PASS (mkdir/rmdir, rename, unlink: 3/3)",
    "[fat32] lfn self-test PASS (open=1 payload=1 readdir=1)",
    "[fat32] lfn-write self-test PASS (create=1 readdir=1 rename=1 unlink=1 "
    "utf8=1 rc_ren=0 rc_unl=0)",
]


class Qmp:
    def __init__(self, port, timeout=90):
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


KEY = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'shift-semicolon',
       '_': 'shift-minus'}


def key_name(ch):
    """sendkey name for one character. A bare uppercase letter ("H") is accepted
    by the monitor but lost by the guest's polled PS/2 decode, so it must be sent
    as shift-h. Same for shifted punctuation."""
    if ch in KEY:
        return KEY[ch]
    if ch.isalpha() and ch.isupper():
        return 'shift-' + ch.lower()
    if ch.isdigit() or (ch.isalpha() and ch.islower()):
        return ch
    raise ValueError('no sendkey mapping for %r' % ch)


def type_text(q, text, delay=0.075):
    for ch in text:
        q.hmp('sendkey ' + key_name(ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(0.55)


def logtext(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as fh:
            return fh.read()
    except OSError:
        return ''


def wait_for_prompt(proc, path, timeout=300):
    """Wait until the shell prompt shows up. The machine is shared with other
    QEMU-based test runs, so allow a generous budget and fail loudly (with the
    serial log tail) if QEMU dies early instead of raising from the QMP connect."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if 'sutureos# ' in logtext(path):
            return True
        if proc.poll() is not None:
            print('[ext4] QEMU exited early (rc=%s); serial log tail:' % proc.returncode)
            print('\n'.join(logtext(path).splitlines()[-12:]))
            return False
        time.sleep(0.4)
    print('[ext4] timed out waiting for the shell prompt (%d s); log tail:' % timeout)
    print('\n'.join(logtext(path).splitlines()[-12:]))
    return False


def make_fresh_disks(need_ext4):
    """Regenerate the FAT32 fixture (its self-tests write to it, so a dirty image
    would change the self-test lines) and, when asked, the ext4 image."""
    py = sys.executable
    r = subprocess.run([py, os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print('[ext4] FAT32 fixture generation FAILED\n' + r.stdout + r.stderr)
        return False
    shutil.copy2(os.path.join(ROOT, 'build', 'fat32.img'),
                 os.path.join(ROOT, 'build', 'fat32_ata.img'))
    if need_ext4:
        r = subprocess.run([py, os.path.join(ROOT, 'scripts', 'mkfs_ext4_testimg.py')],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            print('[ext4] ext4 fixture generation FAILED\n' + r.stdout + r.stderr)
            return False
        for line in r.stdout.strip().splitlines():
            if 'RESULT' in line or 'image sha256' in line or 'free ' in line:
                print('[mkfs] ' + line.strip())
    return True


def qemu_argv(serial, with_ext4):
    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + serial.replace('\\', '/'),
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot']
    if with_ext4:
        argv += ['-drive', 'id=bdisk,file=build/ext4.img,format=raw,if=ide,index=1']
    return argv


EXT4_COMMANDS = [
    'mount ext4 1 /mnt/e4',
    'ls /mnt/e4',
    'cat /mnt/e4/HELLO.TXT',
    'cat /mnt/e4/SUBDIR/INNER.TXT',
    'stat /mnt/e4/BIG.BIN',
    'stat /mnt/e4/SUBDIR',
    'cat /mnt/e4/NOTE4K.TXT',
    'cat /mnt/e4/EMPTY',
    'write /mnt/e4/NOPE.TXT this must be refused',
    'ls /mnt/e4',
]


def drive_session(commands, serial):
    """Boot once, type the commands, quit. Returns (log, ok).

    QEMU can be killed under us by other test runs on the same machine (their
    cleanup does `Stop-Process qemu-system-x86_64`), which shows up as a QMP
    connection reset in the middle of typing. That is an environment failure, not
    an ext4 failure, so the caller retries; `ok=False` is never treated as a
    PASS or as an ext4 bug.
    """
    log_abs = os.path.join(ROOT, serial)
    if os.path.exists(log_abs):
        os.remove(log_abs)
    argv = qemu_argv(serial, with_ext4=True)
    print('[ext4] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    ok = True
    try:
        if not wait_for_prompt(proc, log_abs):
            return logtext(log_abs), False
        q = Qmp(QMP_PORT, timeout=120)
        for c in commands:
            print('[ext4] key> ' + c, flush=True)
            type_text(q, c)
        try:
            q.cmd('quit')
        except Exception:
            pass
    except (OSError, RuntimeError, ValueError) as e:
        print('[ext4] QEMU/QMP session lost mid-run (%r) - retrying' % (e,), flush=True)
        ok = False
    finally:
        try:
            proc.wait(timeout=25)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
    return logtext(log_abs), ok


def run_ext4_test():
    print('[ext4] mode: ext4 volume acceptance', flush=True)
    if not make_fresh_disks(need_ext4=True):
        return 1
    log = ''
    for attempt in range(1, 4):
        if attempt > 1:
            print('[ext4] attempt %d/3' % attempt, flush=True)
        log, ok = drive_session(EXT4_COMMANDS, SERIAL)
        if ok:
            break
    if not ok:
        print('[ext4] RESULT: FAIL (QEMU session could not be completed - '
              'the machine is running other QEMU tests; re-run when it is idle)')
        return 1

    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))

    # ---- driver wiring -----------------------------------------------------
    check('vfs registered the ext4 driver', '[vfs] Registered driver: ext4' in log)
    check('boot probe recognised the ext4 volume on drive 1',
          re.search(r'\[ext4\] ext4 volume on drive 1: blocksz=4096 blocks=4096 '
                    r'inodes=64', log) is not None)
    check('shell mount produced the [ext4] vfs mount line',
          re.search(r'\[ext4\] vfs mount drive=1 at /mnt/e4 \(read-only', log) is not None)
    check('VFS mount reported success for ext4',
          'Mounted 1 (ext4) on /mnt/e4' in log)
    m = re.search(r'\[ext4\] mount drive=(\d+) blocksz=(\d+) blocks=(\d+) inodes=(\d+) '
                  r'groups=(\d+) extent_depth_max=(\d+) \(read-only\)', log)
    check('mount line reports the volume geometry', m is not None)
    if m:
        check('mount line: drive is 1', m.group(1) == '1', m.group(1))
        check('mount line: block size is 4096', m.group(2) == '4096', m.group(2))
        check('mount line: 4096 blocks', m.group(3) == '4096', m.group(3))
        check('mount line: 64 inodes', m.group(4) == '64', m.group(4))
        check('mount line: one block group', m.group(5) == '1', m.group(5))
        check('mount line: deepest extent tree is depth 1', m.group(6) == '1',
              m.group(6))
    check('driver announced the read-only policy',
          '[ext4] read-only: open/read/stat/readdir only' in log)

    # ---- per-file extent evidence from the guest --------------------------
    check('guest saw /HELLO.TXT with one extent',
          '[ext4] file HELLO.TXT size=125 extents=1 depth=0' in log)
    check('guest saw /BIG.BIN as 384 extents, depth 1, two leaf blocks',
          '[ext4] file BIG.BIN size=1572864 extents=384 depth=1 leaves=2' in log)
    check('guest saw /SUBDIR/FRAG.BIN as 384 extents, depth 1, two leaf blocks',
          '[ext4] file SUBDIR/FRAG.BIN size=1572864 extents=384 depth=1 leaves=2' in log)
    check('guest saw the sparse file as two extents (one hole)',
          '[ext4] file SPARSE.BIN size=12288 extents=2 depth=0' in log)
    check('guest saw the 0-byte file as zero extents',
          '[ext4] file EMPTY size=0 extents=0 depth=0' in log)
    check('guest reported the filetype symlink with its target',
          '[ext4] link LINK.TXT -> /HELLO.TXT' in log)
    check('guest probed the subdirectory',
          '[ext4] dir SUBDIR size=4096 extents=1' in log)

    # ---- ls ----------------------------------------------------------------
    ls_block = log.split('ls /mnt/e4')[-2] if log.count('ls /mnt/e4') >= 1 else ''
    for name in ('HELLO.TXT', 'BIG.BIN', 'SUBDIR', 'EMPTY', 'SPARSE.BIN',
                 'NOTE4K.TXT', 'LINK.TXT'):
        check('ls /mnt/e4 lists %s' % name, name in ls_block)
    check('ls marks SUBDIR as a directory (trailing /)',
          re.search(r'SUBDIR\s+/', ls_block) is not None)

    # ---- file contents -----------------------------------------------------
    check('cat /mnt/e4/HELLO.TXT printed the file and 125 bytes',
          'Hello from SutureOS ext4!' in log and '(125 bytes)' in log)
    check('cat /mnt/e4/HELLO.TXT printed both lines of the file',
          'This file lives on a real ext4 volume' in log)
    check('cat /mnt/e4/SUBDIR/INNER.TXT printed the nested file',
          'Nested file: SUBDIR/INNER.TXT' in log and '(71 bytes)' in log)
    check('cat /mnt/e4/NOTE4K.TXT read exactly 4096 bytes',
          '(4096 bytes)' in log)
    check('cat /mnt/e4/EMPTY printed (0 bytes)',
          re.search(r'cat /mnt/e4/EMPTY.*?\(0 bytes\)', log, re.S) is not None)
    check('stat /mnt/e4/BIG.BIN reported 1572864 bytes',
          re.search(r'File: /mnt/e4/BIG.BIN\s*\n\s*Type: regular file\s*\n'
                    r'\s*Size: 1572864 bytes', log) is not None)
    check('stat /mnt/e4/SUBDIR reported a 4096-byte directory',
          re.search(r'File: /mnt/e4/SUBDIR\s*\n\s*Type: directory\s*\n'
                    r'\s*Size: 4096 bytes', log) is not None)

    # ---- read-only policy --------------------------------------------------
    check('the write attempt was rejected by the driver',
          '[ext4] write rejected: the ext4 driver is read-only' in log)
    check('the shell reported the write as not permitted',
          re.search(r'write: /mnt/e4/NOPE.TXT: (operation not permitted|'
                    r'operation not supported|read-only file system|'
                    r'permission denied)', log) is not None,
          'see log')
    check('the refused file really did not appear in the listing',
          log.count('NOPE.TXT') <= 2)

    # ---- health ------------------------------------------------------------
    check('no [ext4] warning/bad/corrupt lines',
          re.search(r'\[ext4\] (WARNING|BAD|mount failed|ERROR|corrupt)', log) is None,
          ','.join(sorted(set(re.findall(r'\[ext4\] (WARNING|BAD|mount failed|ERROR)',
                                         log)))))
    check('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', ''))
    check('module self-tests are still 16/16',
          '[stitch] selftests: 16/16 passed' in log)

    # ---- independent host verification of the image the guest just read ----
    vjson = os.path.join(ROOT, 'build', 'ext4_verify.json')
    if os.path.exists(vjson):
        os.remove(vjson)
    vfy = subprocess.run([sys.executable,
                          os.path.join(ROOT, 'scripts', 'verify_ext4_read.py'),
                          'build/ext4.img', '--guest-log', SERIAL,
                          '--json', 'build/ext4_verify.json'],
                         cwd=ROOT, capture_output=True, text=True)
    vout = vfy.stdout.strip()
    print(vout)
    vres = re.search(r'\[verify_ext4\] checks=(\d+) ok=(\d+) fail=(\d+)', vout)
    check('host verifier PASSED (independent parse + guest log cross-check)',
          vfy.returncode == 0 and 'RESULT: PASS' in vout,
          (vout.splitlines()[-1] if vout else vfy.stderr[-200:]))
    if vres:
        check('host verifier ran more than 150 checks',
              int(vres.group(1)) >= 150, vres.group(1))
        check('host verifier had zero failures', int(vres.group(3)) == 0, vres.group(3))
        print('[ext4] host verifier: %s/%s checks passed'
              % (vres.group(2), vres.group(1)))
    # The read-only proof: the verifier compares the image sha256 with the
    # manifest captured at generation time, i.e. the guest mounted and read the
    # volume without writing a single byte back.
    ro_ok = False
    ro_detail = 'no json table'
    if os.path.exists(vjson):
        try:
            with open(vjson, encoding='utf-8') as fh:
                table = json.load(fh)['checks']
            rows = [r for r in table if r['name'].startswith('image sha256 matches')]
            ro_ok = bool(rows) and rows[0]['ok']
            ro_detail = rows[0]['detail'] if rows else 'row missing'
        except (OSError, ValueError, KeyError) as e:
            ro_detail = repr(e)
    check('the ext4 image is byte-identical after the guest mounted it '
          '(read-only proof)', ro_ok, ro_detail)

    failed = [n for n, ok, _d in checks if not ok]
    for name, ok, detail in checks:
        print('  [%s] %s%s' % ('ok' if ok else 'FAIL', name,
                               (' (%s)' % detail) if (detail and not ok) else ''))
    print('[ext4] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[ext4] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


def boot_regression_session(serial):
    """One plain boot with no ext4 disk: wait for the prompt, quit gracefully."""
    log_abs = os.path.join(ROOT, serial)
    if os.path.exists(log_abs):
        os.remove(log_abs)
    argv = qemu_argv(serial, with_ext4=False)
    print('[ext4] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    ok = True
    try:
        if not wait_for_prompt(proc, log_abs):
            return logtext(log_abs), False
        try:
            q = Qmp(QMP_PORT, timeout=120)
            q.cmd('quit')
        except Exception:
            pass
    except (OSError, RuntimeError) as e:
        print('[ext4] QEMU/QMP session lost (%r) - retrying' % (e,), flush=True)
        ok = False
    finally:
        try:
            proc.wait(timeout=25)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
    return logtext(log_abs), ok


def run_regression():
    print('[ext4] mode: no-ext4-disk regression', flush=True)
    if not make_fresh_disks(need_ext4=False):
        return 1
    log = ''
    ok = False
    for attempt in range(1, 4):
        if attempt > 1:
            print('[ext4] attempt %d/3' % attempt, flush=True)
        log, ok = boot_regression_session(REG_SERIAL)
        if ok:
            break
    if not ok:
        print('[ext4-regression] RESULT: FAIL (QEMU session could not be completed)')
        return 1

    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))

    ext4_lines = [ln for ln in log.splitlines() if ln.startswith('[ext4]')]
    check('exactly one [ext4] line appears in the boot log', len(ext4_lines) == 1,
          repr(ext4_lines))
    check('that line is the "no ext4 volume" report',
          len(ext4_lines) == 1 and ext4_lines[0].startswith('[ext4] no ext4 volume '
                                                          'on drive 1'),
          repr(ext4_lines))
    check('self-tests are still 16/16 (no new self-test item)',
          '[stitch] selftests: 16/16 passed' in log)
    check('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', ''))
    check('no [ext4] error/warning lines',
          re.search(r'\[ext4\] (WARNING|BAD|mount failed|ERROR)', log) is None)

    # The pre-port file-system/self-test sequence must be untouched.
    got = [ln for ln in log.splitlines()
           if ln.startswith(('[vfs]', '[fat32]', '[ext2]', '[squashfs]',
                             '[iso9660]', '[init]', '[selftest]', '[stitch]'))]

    def matches(expected, line):
        return line.startswith(expected)

    missing = []
    for exp in REGRESSION_EXPECTED:
        if not any(matches(exp, ln) for ln in got):
            missing.append(exp)
    check('every pre-port file-system/self-test line is still emitted',
          not missing, 'missing=%r' % (missing,))
    # ...and in the same order (greedy subsequence match).
    idx = 0
    for ln in got:
        if idx < len(REGRESSION_EXPECTED) and matches(REGRESSION_EXPECTED[idx], ln):
            idx += 1
    check('the pre-port log sequence order is unchanged',
          idx == len(REGRESSION_EXPECTED),
          'matched %d/%d (next expected: %r)'
          % (idx, len(REGRESSION_EXPECTED),
             REGRESSION_EXPECTED[idx] if idx < len(REGRESSION_EXPECTED) else ''))
    check('the FAT32 self-tests still pass on the fixture',
          '[fat32] write self-test PASS' in log and
          '[fat32] lfn-write self-test PASS' in log)
    check('ext2 reports no volume on drive 1 (unchanged behaviour)',
          '[init] ext2: no ext2 volume on drive 1' in log)
    check('squashfs reports no volume (unchanged behaviour)',
          '[init] squashfs: no SquashFS volume on drive 1/virtio' in log)
    check('the ISO boot disc is still skipped by iso9660 (unchanged behaviour)',
          '[iso9660] skip drive 14: El Torito boot disc, not a data disc' in log)

    failed = [n for n, ok, _d in checks if not ok]
    for name, ok, detail in checks:
        print('  [%s] %s%s' % ('ok' if ok else 'FAIL', name,
                               (' (%s)' % detail) if (detail and not ok) else ''))
    print('[ext4-regression] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[ext4-regression] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


def main():
    ap = argparse.ArgumentParser(description='SutureOS ext4 acceptance')
    ap.add_argument('--regression', action='store_true',
                    help='boot WITHOUT an ext4 disk and prove nothing changed')
    ap.add_argument('--all', action='store_true', help='run both modes')
    args = ap.parse_args()

    if not os.path.exists(os.path.join(ROOT, 'build', 'sutureos.iso')):
        print('[ext4] build/sutureos.iso is missing: run scripts/build_iso.py first')
        return 1
    if args.regression and not args.all:
        return run_regression()
    rc = run_ext4_test()
    if args.all:
        rc2 = run_regression()
        rc = 0 if (rc == 0 and rc2 == 0) else 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
