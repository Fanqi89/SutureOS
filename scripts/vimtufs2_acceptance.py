#!/usr/bin/env python3
"""SutureOS VimtuFS2 acceptance (real boot, real keystrokes, host-side proof).

What is proven, and how
  A VimtuFS2 volume (VimtuOS' disk format) is generated on the host by
  scripts/mkfs_vimtufs2_testimg.py, attached as the **second IDE disk**
  (`-drive id=bdisk,...,if=ide,index=1` -> block drive 1), and mounted inside the
  guest with the ordinary shell:

      mount vimtufs2 1 /mnt/v2
      ls /mnt/v2
      cat /mnt/v2/HELLO.TXT
      cat /mnt/v2/SUBDIR/INNER.TXT
      stat /mnt/v2/BIG2.BIN
      ...

  Then QEMU is shut down cleanly (`quit`) and scripts/verify_vimtufs2_read.py
  re-parses the on-disk image on the host with a parser that shares no code with
  the guest driver - including a SHA-256 comparison that also proves the
  read-only driver did not modify one byte.

Why keystrokes are injected one character at a time
  The shell input ring is 64 bytes and a bulk `sendkey` batch drops characters
  (this is exactly how earlier acceptance scripts typed "info" instead of
  "ls /proc"), so every character is sent separately with a ~75 ms delay.

Modes
  --mode primary    (default) v5 volume on drive 1: mount, list, read, stat,
                    refuse a write, df, name-too-long, missing file + host verify
  --mode v4         same reads against a v4 volume (proves the version dispatch)
  --mode negative   boot a v5 volume whose feature_bits is 0xf (journal|quota|
                    ACL|snapshot): the driver must refuse it with the exact bits
  --mode nodisk     regression: no second disk -> exactly one extra
                    "[vimtufs2] no volume on drive 1" line, 16/16 selftests

Usage:
  py -3 scripts/vimtufs2_acceptance.py
  py -3 scripts/vimtufs2_acceptance.py --mode v4
  py -3 scripts/vimtufs2_acceptance.py --mode negative
  py -3 scripts/vimtufs2_acceptance.py --mode nodisk
  py -3 scripts/vimtufs2_acceptance.py --mode primary --replay build/serial_x.log
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
DEFAULT_PORT = 45775


class Qmp:
    """Minimal QMP client (JSON over a loopback TCP socket)."""

    def __init__(self, port, attempts=3):
        last = None
        for _ in range(attempts):
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                self.f = self.s.makefile('rwb')
                self._read()
                self.cmd('qmp_capabilities')
                return
            except (OSError, RuntimeError) as e:
                last = e
                try:
                    self.s.close()
                except Exception:                     # noqa: BLE001
                    pass
                time.sleep(0.5)
        raise RuntimeError('QMP never answered: %r' % (last,))

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


# QEMU key names for the characters the acceptance types.  Upper case needs an
# explicit shift- prefix (`sendkey H` types nothing; `sendkey shift-h` types H).
SPECIAL = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
           ':': 'shift-semicolon', '=': 'equal', '_': 'shift-minus'}


def key_name(ch):
    if ch in SPECIAL:
        return SPECIAL[ch]
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    return ch


def type_line(q, text, delay=0.075):
    for ch in text:
        q.hmp('sendkey ' + key_name(ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(0.45)


def read_log(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as fh:
            return fh.read()
    except OSError:
        return ''


def port_free(port):
    s = socket.socket()
    try:
        s.bind(('127.0.0.1', port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def pick_port(start, tries=8):
    for i in range(tries):
        if port_free(start + i):
            return start + i
    return start


def boot_and_type(disks, commands, log_rel, port):
    """Boot the ISO with `disks`, type every command, quit cleanly, return log.

    Two flake sources are handled instead of failing the acceptance:
      * a previous run can leave the QMP port in TIME_WAIT (QEMU dies with
        "Failed to find an available port") -> retry on the next free port;
      * the QMP socket has been seen to be reset mid-typing (host-side network
        hiccup, guest still alive) -> kill our QEMU and redo the whole boot.
    """
    log_abs = os.path.join(ROOT, log_rel)
    last_err = None
    for attempt in range(3):
        proc = None
        try:
            for ptry in range(4):
                p = pick_port(port + (attempt * 4 + ptry) * 2)
                if os.path.exists(log_abs):
                    os.remove(log_abs)
                argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
                        '-serial', 'file:' + log_rel,
                        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % p,
                        '-cdrom', 'build/sutureos.iso', '-boot', 'd',
                        '-smp', '2', '-no-reboot']
                for spec in disks:
                    argv += ['-drive', spec]
                print('[vimtufs2] qemu: ' + ' '.join(argv), flush=True)
                proc = subprocess.Popen(argv, cwd=ROOT)
                time.sleep(3.0)
                if proc.poll() is None:
                    port = p
                    break
                last_err = 'qemu exited with %s on QMP port %d' % (proc.returncode, p)
                print('[vimtufs2] %s - retrying on another port' % last_err, flush=True)
                proc = None
            if proc is None:
                raise RuntimeError(last_err or 'qemu would not start')

            deadline = time.time() + 120
            while time.time() < deadline:
                if 'sutureos# ' in read_log(log_abs):
                    break
                time.sleep(0.4)
            else:
                raise RuntimeError('guest never reached the shell prompt')
            time.sleep(1.0)
            q = Qmp(port)
            for cmd in commands:
                print('[vimtufs2] type: %s' % cmd, flush=True)
                type_line(q, cmd)
            time.sleep(1.5)
            q.cmd('quit')
        except Exception as exc:                     # noqa: BLE001
            last_err = exc
            if proc is not None and proc.poll() is None:
                try:
                    proc.kill()
                except Exception:                    # noqa: BLE001
                    pass
            try:
                proc.wait(timeout=10)
            except Exception:                        # noqa: BLE001
                pass
            if attempt < 2:
                print('[vimtufs2] boot attempt %d failed (%r) - retrying'
                      % (attempt + 1, exc), flush=True)
                time.sleep(2.0)
            continue
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
        return read_log(log_abs)
    raise RuntimeError('boot failed three times: %r' % (last_err,))


# ============================================================================
# checks (each takes the serial log and returns a list of (name, ok[, detail]))
# ============================================================================

def check_common(log, checks):
    checks.append(('kernel selftests are 16/16',
                   '[stitch] selftests: 16/16 passed' in log))
    checks.append(('no kernel PANIC',
                   'PANIC' not in log.upper().replace('NO PANIC', '')))
    checks.append(('no ERROR log line',
                   re.search(r'(?m)^.*\bERROR\b', log) is None))


def check_host_verifier(image, manifest, checks, label='host verifier'):
    script = os.path.join(ROOT, 'scripts', 'verify_vimtufs2_read.py')
    argv = [sys.executable, script, image]
    if manifest:
        argv += ['--manifest', manifest]
    out = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)
    passed = 'RESULT: PASS' in out.stdout and out.returncode == 0
    m = re.search(r'checks=(\d+) ok=(\d+) fail=(\d+)', out.stdout)
    checks.append(('%s PASS (%s)' % (label, m.group(0) if m else 'no counter'),
                   passed))
    if not passed:
        print(out.stdout[-4000:])
        print(out.stderr[-2000:], file=sys.stderr)


PRIMARY_COMMANDS = [
    'mount vimtufs2 1 /mnt/v2',
    'ls /mnt/v2',
    'cat /mnt/v2/HELLO.TXT',
    'cat /mnt/v2/SUBDIR/INNER.TXT',
    'cat /mnt/v2/EMPTY',
    'stat /mnt/v2/BIG2.BIN',
    'stat /mnt/v2/SUBDIR',
    # Reading inside BIG2.BIN proves the guest driver really follows the
    # indirect pointer tables (the file is 300 blocks: 4 direct + 128 single
    # indirect + 168 double indirect).  `hexdump` keeps the log small.
    'hexdump /mnt/v2/BIG2.BIN 2048 16',
    'hexdump /mnt/v2/BIG2.BIN 68000 16',
    'hexdump /mnt/v2/BIG2.BIN 150000 16',
    'cat /mnt/v2/ABCDEFGHIJKLMNOPQRSTUVWXYZ01234',
    'cat /mnt/v2/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345',
    'write /mnt/v2/NOPE.TXT hello',
    'df',
    'cat /mnt/v2/NOFILE.TXT',
]

# The generator fills BIG2.BIN with this documented, position dependent
# pattern, so the acceptance can predict the bytes it expects to read back
# without trusting the manifest.
def big2_byte(i):
    return (i * 31 + (i >> 8) * 7 + 0x5A) & 0xFF


BIG2_PROBES = [
    (2048, 'first block behind the single indirect block'),
    (68000, 'first block behind the double indirect block'),
    (150000, 'second double-indirect child block (tail of the file)'),
]


def expected_hex(off, count=16):
    return ' '.join('%02x' % big2_byte(off + i) for i in range(count))


def checks_primary(log):
    checks = []
    checks.append(('boot probe reports the volume on drive 1',
                   '[vimtufs2] volume ready on drive 1: version=5 blocks=24576 '
                   'inodes=1536 free=23878' in log))
    checks.append(('core mount line has version/blocks/inodes',
                   re.search(r'\[vimtufs2\] mount drive=1 version=5 blocks=24576 '
                             r'inodes=1536 inode=128B', log) is not None))
    checks.append(('VFS driver line for mount vimtufs2 1 /mnt/v2',
                   '[vimtufs2] vfs mount drive=1 at /mnt/v2' in log))
    checks.append(('mount line says read-only',
                   re.search(r'\[vimtufs2\] vfs mount .*read-only', log) is not None))
    checks.append(('ls lists HELLO.TXT and BIG2.BIN',
                   re.search(r'(?m)^HELLO\.TXT\s*$', log) is not None and
                   re.search(r'(?m)^BIG2\.BIN\s*$', log) is not None))
    checks.append(('ls marks SUBDIR as a directory and lists EMPTY',
                   re.search(r'(?m)^SUBDIR\s*/\s*$', log) is not None and
                   re.search(r'(?m)^EMPTY\s*$', log) is not None))
    checks.append(('ls lists the 31-byte name',
                   re.search(r'(?m)^ABCDEFGHIJKLMNOPQRSTUVWXYZ01234\s*$', log) is not None))
    checks.append(('HELLO.TXT opens as inode 1 with size 21 and 1 block',
                   '[vimtufs2] file HELLO.TXT inode=1 size=21 blocks=1' in log))
    checks.append(('cat HELLO.TXT prints the payload',
                   'Hello from VimtuFS2!' in log and '(21 bytes)' in log))
    checks.append(('cat SUBDIR/INNER.TXT prints the payload',
                   'This file lives in SUBDIR.' in log and '(27 bytes)' in log))
    checks.append(('EMPTY reads as zero bytes (no blocks mapped)',
                   '[vimtufs2] file EMPTY inode=5 size=0 blocks=0' in log and
                   '(0 bytes)' in log))
    checks.append(('stat BIG2.BIN reports 153600 bytes (300 blocks)',
                   re.search(r'File: /mnt/v2/BIG2\.BIN', log) is not None and
                   'Size: 153600 bytes' in log))
    checks.append(('stat SUBDIR reports a directory', 'Type: directory' in log))
    for off, label in BIG2_PROBES:
        checks.append(('BIG2.BIN reads the right bytes at offset %d (%s)'
                       % (off, label),
                       expected_hex(off) in log and
                       ('%08x  ' % off) in log))
    checks.append(('31-byte name reads back through the driver',
                   '31-byte name' in log and '(13 bytes)' in log))
    checks.append(('a 32-byte name is refused with the exact limit',
                   re.search(r'\[vimtufs2\] name too long: 32 bytes \(limit 31\)',
                             log) is not None and
                   re.search(r'cat: /mnt/v2/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345: '
                             r'invalid argument', log) is not None))
    checks.append(('a write to the volume is refused (read-only driver)',
                   '[vimtufs2] open for write refused' in log and
                   re.search(r'write: /mnt/v2/NOPE\.TXT: operation not supported',
                             log) is not None))
    checks.append(('df reports the VimtuFS2 volume with its real size',
                   re.search(r'(?m)^vimtufs2\s+1\s+12288\s+\d+\s+11939\s+/mnt/v2',
                             log) is not None))
    checks.append(('a missing file reports not-found',
                   re.search(r'cat: /mnt/v2/NOFILE\.TXT: no such file or directory',
                             log) is not None))
    checks.append(('no unexpected cpu exception after the shell started',
                   'unexpected cpu exception' not in log.split('sutureos# ')[-1]))
    check_common(log, checks)
    check_host_verifier('build/vimtufs2.img', None, checks)
    return checks


V4_COMMANDS = [
    'mount vimtufs2 1 /mnt/v3',
    'ls /mnt/v3',
    'cat /mnt/v3/HELLO.TXT',
    'cat /mnt/v3/SUBDIR/INNER.TXT',
    'stat /mnt/v3/BIG2.BIN',
]


def checks_v4(log):
    checks = []
    checks.append(('boot probe reports a v4 volume',
                   '[vimtufs2] volume ready on drive 1: version=4' in log))
    checks.append(('mount line says version=4',
                   re.search(r'\[vimtufs2\] mount drive=1 version=4 blocks=24576',
                             log) is not None))
    checks.append(('VFS driver mounts the v4 volume',
                   '[vimtufs2] vfs mount drive=1 at /mnt/v3' in log))
    checks.append(('v4 HELLO.TXT reads (21 bytes)',
                   'Hello from VimtuFS2!' in log and '(21 bytes)' in log))
    checks.append(('v4 directory child reads (27 bytes)', '(27 bytes)' in log))
    checks.append(('v4 stat of the 300-block file is right',
                   'Size: 153600 bytes' in log))
    checks.append(('v4 ls lists the directory entry',
                   re.search(r'(?m)^SUBDIR\s*/\s*$', log) is not None))
    check_common(log, checks)
    check_host_verifier('build/vimtufs2_v4.img', 'build/vimtufs2_v4_manifest.json',
                        checks, 'host verifier (v4)')
    return checks


NEGATIVE_COMMANDS = [
    'mount vimtufs2 1 /mnt/bad',
    'ls /mnt/bad',
]


def checks_negative(log):
    checks = []
    checks.append(('boot probe refuses the volume and names the feature bits',
                   re.search(r'\[vimtufs2\] no volume on drive 1 \(reason=feature '
                             r'feature_bits=0xf \(journal\|quota\|acl\|snapshot\) '
                             r'unsupported\)', log) is not None))
    checks.append(('mount refuses it too, with the same bits',
                   re.search(r'\[vimtufs2\] mount FAILED drive=1 lba=0 \(reason=feature '
                             r'feature_bits=0xf', log) is not None))
    checks.append(('the shell reports the failed mount',
                   re.search(r'mount: /mnt/bad: operation not supported', log) is not None))
    checks.append(('nothing is mounted at /mnt/bad',
                   '[vimtufs2] vfs mount drive=1 at /mnt/bad' not in log and
                   re.search(r'ls: /mnt/bad: no such file or directory', log) is not None))
    checks.append(('no volume geometry line was ever printed',
                   '[vimtufs2] mount drive=1 version=' not in log))
    check_common(log, checks)
    return checks


def checks_nodisk(log):
    vf_lines = [ln for ln in log.splitlines() if ln.startswith('[vimtufs2]')]
    checks = []
    checks.append(('exactly one [vimtufs2] line at boot', len(vf_lines) == 1,
                   vf_lines))
    checks.append(('that line is the no-volume report for drive 1',
                   bool(vf_lines) and
                   re.search(r'^\[vimtufs2\] no volume on drive 1 \(reason=no-device\)$',
                             vf_lines[0]) is not None))
    checks.append(('the probe did not add a second [blk] read failure',
                   log.count('[blk] Read failed') <= 1,
                   'count=%d' % log.count('[blk] Read failed')))
    check_common(log, checks)
    return checks


COMBO_COMMANDS = [
    'mount vimtufs2 1 /mnt/v2',
    'ls /mnt/v2',
    'ls /mnt/fat',
    'cat /mnt/fat/HELLO.TXT',
    'cat /mnt/v2/HELLO.TXT',
    'stat /mnt/v2/BIG2.BIN',
]


def checks_combo(log):
    """VimtuFS2 next to the pre-existing FAT32 stack: neither disturbs the other."""
    checks = []
    checks.append(('FAT32 volume is still auto-mounted at boot',
                   '[init] FAT32 volume auto-mounted at /mnt/fat (drive 0)' in log))
    checks.append(('FAT32 self-test still runs and passes',
                   'self-test PASS' in log))
    checks.append(('the VimtuFS2 volume mounts on drive 1',
                   '[vimtufs2] vfs mount drive=1 at /mnt/v2' in log))
    checks.append(('ls /mnt/v2 lists the VimtuFS2 entries',
                   re.search(r'(?m)^SUBDIR\s*/\s*$', log) is not None))
    checks.append(('ls /mnt/fat still lists the FAT32 entries',
                   re.search(r'(?m)^HELLO\.TXT\s*$', log) is not None))
    checks.append(('both filesystems serve their own HELLO.TXT',
                   log.count('Hello from VimtuFS2!') == 1 and
                   log.count('Hello from SutureOS FAT32!') == 1))
    checks.append(('the 300-block VimtuFS2 file still stats right',
                   'Size: 153600 bytes' in log))
    check_common(log, checks)
    check_host_verifier('build/vimtufs2.img', None, checks,
                        'host verifier after the combo boot')
    return checks


MODES = {
    'primary':  (['id=bdisk,file=build/vimtufs2.img,format=raw,if=ide,index=1'],
                 PRIMARY_COMMANDS, 'build/serial_vimtufs2.log', checks_primary),
    'v4':       (['id=bdisk,file=build/vimtufs2_v4.img,format=raw,if=ide,index=1'],
                 V4_COMMANDS, 'build/serial_vimtufs2_v4.log', checks_v4),
    'negative': (['id=bdisk,file=build/vimtufs2_badfeat.img,format=raw,if=ide,index=1'],
                 NEGATIVE_COMMANDS, 'build/serial_vimtufs2_neg.log', checks_negative),
    'nodisk':   ([], [], 'build/serial_vimtufs2_nodisk.log', checks_nodisk),
    'combo':    (['id=adisk,file=build/fat32_vf_combo.img,format=raw,if=ide',
                  'id=bdisk,file=build/vimtufs2.img,format=raw,if=ide,index=1'],
                 COMBO_COMMANDS, 'build/serial_vimtufs2_combo.log', checks_combo),
}


def main(argv=None):
    ap = argparse.ArgumentParser(description='SutureOS VimtuFS2 acceptance')
    ap.add_argument('--mode', choices=sorted(MODES), default='primary')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--replay', default=None,
                    help='skip QEMU and re-run the checks against an existing log')
    args = ap.parse_args(argv)

    disks, commands, log_rel, checker = MODES[args.mode]
    if args.replay:
        log = read_log(os.path.join(ROOT, args.replay))
        print('[vimtufs2] replaying checks against %s' % args.replay)
    else:
        if not os.path.exists(os.path.join(ROOT, 'build', 'sutureos.iso')):
            print('[vimtufs2] build/sutureos.iso is missing - run scripts/build.py '
                  'and scripts/build_iso.py first')
            return 1
        try:
            log = boot_and_type(disks, commands, log_rel, args.port)
        except Exception as exc:                     # noqa: BLE001
            print('[vimtufs2] acceptance could not run: %r' % (exc,))
            return 1

    checks = checker(log)
    failed = []
    for item in checks:
        name, ok = item[0], item[1]
        detail = item[2] if len(item) > 2 else ''
        line = '  [%s] %s' % ('ok' if ok else 'FAIL', name)
        if not ok and detail:
            line += '  (%s)' % (detail,)
        print(line)
        if not ok:
            failed.append(name)
    print('[vimtufs2] mode=%s checks=%d ok=%d fail=%d'
          % (args.mode, len(checks), len(checks) - len(failed), len(failed)))
    print('[vimtufs2] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
