#!/usr/bin/env python3
"""SutureOS ring-3 file tools acceptance driver (porting batch B5).

Boots the ISO with a fresh FAT32 test disk, drives a whole session of the new
ring-3 tools through QMP keyboard injection, keeps the serial transcript, asserts
one PASS per command, and then re-parses the raw disk image on the host (never
trusting the guest).

Pipeline:
    py -3 scripts/build.py
    py -3 scripts/build_iso.py
    py -3 scripts/ring3_tools_acceptance.py          # this script: QEMU + asserts
    # ... it then runs, and prints the result of:
    py -3 scripts/verify_fat32_write.py build/fat32_ata.img build/fat32.img

What it proves (each item is an assertion below):
  * `ushell` starts, the new commands run as child processes and the shell
    survives (fork + execve-with-arguments + wait4),
  * rls / rcat / rcp / rmv / rrm / rmkdir / rtouch / rdf really operate on the
    FAT32 volume (the disk image changes, verified from the host),
  * failures print the real errno (missing file, non-empty rmdir, cross-directory
    rename, recursive delete not supported),
  * the volume ends up clean: every R3* path removed, the original files intact,
    no orphaned cluster chains, FSInfo consistent,
  * the pre-existing acceptance programs (`run probe` / `run hello` /
    `run forktest`) still pass, the kernel self-tests still report 13/14 and no
    PANIC appears.

QMP port 45805 (the task's assigned port). The serial log is build/serial_r3tools.log.
"""
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
QMP_PORT = 45805
LOG_REL = 'build/serial_r3tools.log'
LOG = os.path.join(ROOT, LOG_REL)
PRISTINE = os.path.join(ROOT, 'build', 'fat32.img')
DIRTY = os.path.join(ROOT, 'build', 'fat32_ata.img')
ISO = os.path.join(ROOT, 'build', 'sutureos.iso')

# (command typed into the ring-3 shell, substring that must appear afterwards)
TOOL_COMMANDS = [
    ('rls /mnt/fat', 'entries in /mnt/fat'),
    ('rcat /mnt/fat/hello.txt', 'Hello from SutureOS FAT32!'),
    ('rmkdir /mnt/fat/R3DIR', 'rmkdir: created /mnt/fat/R3DIR'),
    ('rtouch /mnt/fat/R3DIR/A.TXT', 'rtouch: created /mnt/fat/R3DIR/A.TXT'),
    ('rls /mnt/fat/R3DIR', 'A.TXT'),
    ('rmv /mnt/fat/README.TXT /mnt/fat/R3DIR/X.TXT',
     'cross-directory move is not supported'),
    ('rcp /mnt/fat/hello.txt /mnt/fat/R3DIR/COPY.TXT',
     'rcp: /mnt/fat/hello.txt -> /mnt/fat/R3DIR/COPY.TXT ('),
    ('rcat /mnt/fat/R3DIR/COPY.TXT', 'Hello from SutureOS FAT32!'),
    ('rmv /mnt/fat/R3DIR/COPY.TXT /mnt/fat/R3DIR/MOVED.TXT',
     'rmv: renamed /mnt/fat/R3DIR/COPY.TXT -> /mnt/fat/R3DIR/MOVED.TXT'),
    ('rls /mnt/fat/R3DIR', 'MOVED.TXT'),
    ('rrm /mnt/fat/R3DIR/MOVED.TXT', 'rrm: removed file /mnt/fat/R3DIR/MOVED.TXT'),
    ('rrm /mnt/fat/R3DIR/A.TXT', 'rrm: removed file /mnt/fat/R3DIR/A.TXT'),
    ('rrm /mnt/fat/R3DIR', 'rrm: removed directory /mnt/fat/R3DIR'),
    ('rmkdir -p /mnt/fat/R3DEEP/A/B', 'rmkdir: created /mnt/fat/R3DEEP/A'),
    ('rrm /mnt/fat/R3DEEP', 'is not empty (errno=39 ENOTEMPTY)'),
    ('rrm /mnt/fat/R3DEEP/A/B', 'rrm: removed directory /mnt/fat/R3DEEP/A/B'),
    ('rrm /mnt/fat/R3DEEP/A', 'rrm: removed directory /mnt/fat/R3DEEP/A'),
    ('rrm /mnt/fat/R3DEEP', 'rrm: removed directory /mnt/fat/R3DEEP'),
    ('rtouch /mnt/fat/hello.txt', 'already exists, left untouched'),
    ('rdf /mnt/fat', 'Filesystem: /mnt/fat'),
    ('rrm /mnt/fat/nosuch.txt', 'failed (errno=2 ENOENT)'),
    ('rrm -r /mnt/fat', 'recursive delete is not supported'),
]

# (command typed into the *kernel* shell after leaving ushell, marker)
REGRESSION_COMMANDS = [
    ('run probe', '[probe] 6 exiting with 7'),
    ('run hello', 'hello from Vimtu64 C userland'),
    ('run forktest', '[forktest] RESULT PASS'),
]

KEYMAP = {
    ' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '_': 'shift-minus',
    ':': 'shift-semicolon', '=': 'equal',
}


class Qmp:
    def __init__(self, port, timeout=45):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:            # noqa: PERF203 - retry loop by design
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


def read_log():
    try:
        with open(LOG, encoding='utf-8', errors='replace') as fh:
            return fh.read()
    except OSError:
        return ''


def wait_log(needle, timeout=30.0, start=0):
    """Wait until `needle` appears in the serial log at/after offset `start`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_log()
        idx = text.find(needle, start)
        if idx >= 0:
            return idx + len(needle)
        time.sleep(0.15)
    return -1


def keys_for(text):
    keys = []
    for ch in text:
        if ch.isupper():
            keys.append('shift-' + ch.lower())
        elif ch in KEYMAP:
            keys.append(KEYMAP[ch])
        else:
            keys.append(ch)
    return keys


def send_line(q, text, per_key=0.05):
    for key in keys_for(text):
        q.hmp('sendkey ' + key)
        time.sleep(per_key)
    q.hmp('sendkey ret')


def host_image_root(path):
    """Parse the raw FAT32 image on the host: root entries + free cluster count.

    Independent of anything the guest printed (the same reader
    scripts/verify_fat32_write.py uses, imported so there is one parser).
    """
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import verify_fat32_write as vf          # noqa: E402  (host-side reader)

    img = open(path, 'rb').read()
    v = vf.parse(img)
    return {
        'entries': dict(v['entries']),
        'clusters': v['clusters'],
        'free_measured': v['free_measured'],
        'fsinfo_free': v['fsinfo_free'],
        'lfn': [nm for (_raw, nm) in v['lfn_chains']],
    }


def main():
    failures = []
    passes = []

    def check(name, ok, detail=''):
        if ok:
            passes.append(name)
            print('  [PASS] %s%s' % (name, ('  (' + detail + ')') if detail else ''))
        else:
            failures.append((name, detail))
            print('  [FAIL] %s%s' % (name, ('  (' + detail + ')') if detail else ''))

    for path in (QEMU, ISO):
        if not os.path.exists(path):
            sys.exit('missing %s' % path)

    # ---- 1. fresh fixture disk ------------------------------------------------
    print('[r3tools] regenerating the pristine FAT32 fixture ...', flush=True)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('mkfat32_testimg.py failed:\n%s\n%s' % (r.stdout, r.stderr))
    shutil.copy2(PRISTINE, DIRTY)
    before = host_image_root(PRISTINE)
    print('[r3tools] pristine root: %s' % ', '.join(sorted(before['entries'])))

    if os.path.exists(LOG):
        os.remove(LOG)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + LOG_REL,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-hda', 'build/fat32_ata.img', '-smp', '2',
            '-no-reboot', '-no-shutdown']
    print('[r3tools] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    q = None
    try:
        q = Qmp(QMP_PORT)
        if wait_log('sutureos# ', timeout=180) < 0:
            raise RuntimeError('kernel shell prompt never appeared')

        # ---- 2. enter the ring-3 shell ---------------------------------------
        send_line(q, 'ushell')
        if wait_log('user$ ', timeout=60) < 0:
            raise RuntimeError('the ring-3 shell prompt never appeared')
        print('[r3tools] ring-3 shell is up; running %d tool commands'
              % len(TOOL_COMMANDS), flush=True)

        # ---- 3. the tool session --------------------------------------------
        for cmd, marker in TOOL_COMMANDS:
            start = len(read_log())
            send_line(q, cmd)
            end = wait_log(marker, timeout=45, start=start)
            chunk = read_log()[start:]
            if end >= 0:
                check('%-52s -> %s' % (cmd, marker), True)
            else:
                check('%-52s -> %s' % (cmd, marker), False,
                      'last output: ' + chunk[-400:].replace('\n', ' | '))
            time.sleep(0.5)                      # let the prompt settle

        # ---- 4. leave the shell, then the old acceptance programs ------------
        start = len(read_log())
        send_line(q, 'exit')
        if wait_log('ushell: ring-3 shell exited with 0', timeout=60, start=start) < 0:
            check('ushell returns to the kernel shell cleanly', False,
                  read_log()[start:][-400:].replace('\n', ' | '))
        else:
            check('ushell returns to the kernel shell cleanly', True)

        for cmd, marker in REGRESSION_COMMANDS:
            start = len(read_log())
            send_line(q, cmd)
            end = wait_log(marker, timeout=60, start=start)
            chunk = read_log()[start:]
            check('regression: %-40s -> %s' % (cmd, marker), end >= 0,
                  '' if end >= 0 else 'last output: ' + chunk[-400:].replace('\n', ' | '))
            time.sleep(0.4)
        time.sleep(1.0)
    finally:
        if q is not None:
            try:
                q.hmp('quit')
            except Exception:                   # noqa: BLE001 - teardown best effort
                pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = read_log()
    print('[r3tools] serial transcript: %s (%d bytes)' % (LOG_REL, len(log)))

    # ---- 5. kernel-side health ----------------------------------------------
    check('no PANIC in the transcript', 'PANIC' not in log)
    check('no ring-3 user fault in the transcript',
          'user fault' not in log and 'user #PF' not in log)
    m = re.search(r'\[stitch\] selftests: (\d+)/(\d+) passed', log)
    check('kernel self-tests still 13/14',
          m is not None and m.group(1) == '13' and m.group(2) == '14',
          m.group(0) if m else 'self-test summary line missing')
    check('kernel self-test suite says 13/14 (WARNING line is the known moos_intr)',
          '[stitch] selftests: 13/14 passed' in log)
    check('FAT32 volume mounted', '[fat32] mount drive=0 ' in log and
          '(fat32)' in log)
    check('no guest-side FAT32 self-test failure',
          'self-test FAIL' not in log and 'ops self-test FAIL' not in log)

    # ---- 6. host-side independent verification ------------------------------
    print('[r3tools] host-side FAT32 verification:', flush=True)
    v = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'verify_fat32_write.py'),
                        DIRTY, PRISTINE], cwd=ROOT, capture_output=True, text=True)
    for line in (v.stdout or '').strip().splitlines():
        print('    ' + line)
    summary = re.search(r'checks=(\d+) ok=(\d+) fail=(\d+)', v.stdout or '')
    check('verify_fat32_write.py: every check passes', v.returncode == 0,
          summary.group(0) if summary else 'no summary line')

    # ---- 7. before/after directory comparison -------------------------------
    after = host_image_root(DIRTY)
    before_names = set(before['entries'])
    after_names = set(after['entries'])
    added = sorted(after_names - before_names)
    removed = sorted(before_names - after_names)

    print('=== FAT32 root directory: before / after (host-side parse) ===')
    print('  %-42s %-12s %s' % ('path', 'kind', 'pristine bytes'))
    for name in sorted(before_names):
        _first, size = before['entries'][name]
        print('  %-42s %-12s %d' % (name, 'file', size))
    print('  pristine free clusters: %d / %d'
          % (before['free_measured'], before['clusters']))
    print('  ---')
    for name in sorted(after_names):
        _first, size = after['entries'][name]
        mark = '+' if name in added else ' '
        print('  %s %-40s %-12s %d' % (mark, name, 'file', size))
    print('  after free clusters   : %d / %d (FSInfo says %d)'
          % (after['free_measured'], after['clusters'], after['fsinfo_free']))
    print('  added   : %s' % (', '.join(added) if added else '(none)'))
    print('  removed : %s' % (', '.join(removed) if removed else '(none)'))

    leftover = [n for n in after_names
                if n.startswith('R3') or n.startswith('COPY') or n.startswith('MOVED')]
    check('no R3* artifact left in the volume root', not leftover, ', '.join(leftover))
    check('the fixture files are all still present',
          {'HELLO.TXT', 'README.TXT', 'HELLO.DEB'} <= after_names)
    # The kernel's boot self-tests create these and nothing else: SUTURE.TXT and
    # RENAMED.TXT are the FAT32 write/rename checks, and the underscore alias is the
    # 8.3 name of the non-ASCII long-name fixture ("中文 长名.txt") written by the
    # VFAT long-name self-test. No tool of this batch may leave anything behind.
    boot_artifacts = {'SUTURE.TXT', 'RENAMED.TXT', '________.TXT'}
    check('only the boot self-test artifacts were added',
          set(added) <= boot_artifacts,
          'added=' + ', '.join(added))
    check('FSInfo free count matches the measured free clusters',
          after['fsinfo_free'] == after['free_measured'],
          'fsinfo=%d measured=%d' % (after['fsinfo_free'], after['free_measured']))

    # ---- 8. verdict ---------------------------------------------------------
    print()
    print('=== RESULT: %s ===  checks=%d ok=%d fail=%d'
          % ('PASS' if not failures else 'FAIL',
             len(passes) + len(failures), len(passes), len(failures)))
    if failures:
        for name, detail in failures:
            print('  failed: %s  %s' % (name, detail))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
