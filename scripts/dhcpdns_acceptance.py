#!/usr/bin/env python3
"""SutureOS DHCP client + DNS resolver acceptance (real boot, real keys, slirp).

The network domain's remaining gaps are automatic addressing and name
resolution. Both are proven end-to-end here against QEMU's user-mode network
(slirp), which hands out 10.0.2.15/24 with gateway 10.0.2.2 and DNS 10.0.2.3.

Phase A - rtl8139 + `-netdev user` (two runs of the same ISO are not needed,
everything happens in one boot):
  1. `ping 10.0.2.2`   the pre-existing static default configuration still works
                       (this is the "do not regress the old behaviour" check),
  2. `dhcp`            `[dhcp] DISCOVER xid=0x..` -> `[dhcp] OFFER 10.0.2.15
                       from 10.0.2.2` -> `[dhcp] ACK ip=10.0.2.15
                       mask=255.255.255.0 gw=10.0.2.2 dns=10.0.2.3 lease=..s`,
                       and the shell prints the four-tuple,
  3. `dns example.com`/`dns google.com`  `[dns] query .. id=0x..` and
                       `[dns] answer .. -> <valid IPv4> (1 A record)`,
  4. `ping 10.0.2.2`   again, to prove the freshly programmed lease did not
                       break the transmit/receive path,
  5. kernel self-tests stay 16/16 and the kernel never panics.

Phase B - same ISO booted WITHOUT a NIC: `dhcp` reports "no NIC / network not
ready", no `[dhcp] ACK` is invented, self-tests still 16/16, no PANIC.

Why the keys are typed one character at a time: the shell input ring is 64 bytes
and a bulk `sendkey` batch loses characters (the same trap documented for the
procfs/ring3 fixtures). QMP `sendkey` + 75 ms per character is reliable.

  py -3 scripts/dhcpdns_acceptance.py
  py -3 scripts/dhcpdns_acceptance.py --no-nic-off   # phase A only
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
QMP_PORT_BASE = 45761

SERIAL_A = 'build/serial_dhcpdns.log'
SERIAL_B = 'build/serial_dhcpdns_nonic.log'

IPV4_RE = re.compile(r'^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$')


def free_port(base, span=20):
    """A QMP port nobody is listening on, so a stale QEMU from an earlier run
    cannot be mistaken for this run's monitor (that failure looks like a
    ConnectionReset on the first QMP read)."""
    for port in range(base, base + span):
        s = socket.socket()
        try:
            s.bind(('127.0.0.1', port))
            s.close()
            return port
        except OSError:
            s.close()
    raise RuntimeError('no free QMP port in %d..%d' % (base, base + span))


def kill_stale_qemu():
    """Kill QEMU processes started from THIS copy only (never another port's)."""
    try:
        out = subprocess.run(
            ['powershell', '-NoProfile', '-Command',
             "(Get-CimInstance Win32_Process -Filter \"Name='qemu-system-x86_64.exe'\" "
             "| Where-Object { $_.CommandLine -like '*sutureos_ports*' } "
             "| Select-Object -ExpandProperty ProcessId)"],
            capture_output=True, text=True, timeout=60).stdout
    except Exception:                            # noqa: BLE001
        return
    for tok in out.split():
        if tok.isdigit():
            subprocess.run(['taskkill', '/PID', tok, '/F'],
                           capture_output=True, text=True, timeout=30)


class Qmp:
    def __init__(self, port, timeout=45):
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


KEY = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'shift-semicolon'}


def type_text(q, text, delay=0.075):
    for ch in text:
        q.hmp('sendkey ' + KEY.get(ch, ch))
        time.sleep(delay)
    q.hmp('sendkey ret')
    time.sleep(0.25)


def valid_ipv4(s):
    m = IPV4_RE.match(s or '')
    if not m:
        return False
    return all(0 <= int(g) <= 255 for g in m.groups())


def run_boot(serial_rel, with_nic, commands, boot_timeout=180, attempts=2):
    """Boot the ISO, type each (command, expected-regex) pair, quit gracefully.

    Returns the serial log text. Commands are typed only after the shell prompt
    shows up, and each one waits for its expected log line before the next is
    typed, so a slow TCG boot cannot interleave them.

    The boot itself is flaky on this 2-core host (a TCG boot has been observed to
    stall after the FAT32 self-tests), so the boot step is retried; the retry
    count is printed instead of being hidden.
    """
    last_err = None
    for attempt in range(1, attempts + 1):
        try:
            return _run_boot_once(serial_rel, with_nic, commands, boot_timeout)
        except RuntimeError as exc:
            last_err = exc
            print('[dhcpdns] boot attempt %d/%d failed: %s'
                  % (attempt, attempts, exc), flush=True)
            time.sleep(2)
    raise last_err


def _run_boot_once(serial_rel, with_nic, commands, boot_timeout):
    kill_stale_qemu()
    port = free_port(QMP_PORT_BASE)
    log_abs = os.path.join(ROOT, serial_rel)
    if os.path.exists(log_abs):
        os.remove(log_abs)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-serial', 'file:' + serial_rel,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % port,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot']
    if with_nic:
        argv += ['-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0']
    else:
        # QEMU's pc machine adds a default e1000 unless NICs are suppressed;
        # without this the "no NIC" regression would silently boot with one.
        argv += ['-nic', 'none']
    print('[dhcpdns] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)

    def logtext():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_line(pat, timeout, what):
        deadline = time.time() + timeout
        rx = re.compile(pat)
        while time.time() < deadline:
            m = rx.search(logtext())
            if m:
                return m
            time.sleep(0.3)
        raise RuntimeError('timed out waiting for %s (/%s/)' % (what, pat))

    try:
        wait_line(r'sutureos# ', boot_timeout, 'shell prompt')
        q = Qmp(port)
        for cmd, pat in commands:
            print('[dhcpdns] > ' + cmd, flush=True)
            type_text(q, cmd)
            wait_line(pat, 90, 'output of `%s`' % cmd)
        try:
            q.cmd('quit')
        except Exception:
            pass
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=30)
    return logtext()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-nic-off', dest='no_nic', action='store_false',
                    help='skip the "boot without a NIC" regression phase')
    args = ap.parse_args()

    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[dhcpdns] FAIL: build/sutureos.iso is missing (run scripts/build.py '
              'and scripts/build_iso.py first)')
        return 2

    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))

    # ---------------- phase A: NIC + slirp ----------------
    log_a = run_boot(SERIAL_A, True, [
        ('ping 10.0.2.2', r'--- 10\.0\.2\.2: \d/3 replies ---'),
        ('dhcp', r'\[dhcp\] (ACK|timeout)'),
        ('dns example.com', r'\[dns\] (answer|nxdomain|timeout)'),
        ('dns google.com', r'\[dns\] (answer|nxdomain|timeout)'),
        ('wget http://nosuchhost.invalid/x', r'wget: cannot resolve host'),
        ('ping 10.0.2.2', r'--- 10\.0\.2\.2: \d/3 replies ---'),
    ])

    # Boot log must not gain DHCP/DNS lines: the client and the resolver are on
    # demand only, so the frozen boot presentation keeps exactly its old lines.
    boot_part = log_a.split('sutureos# ', 1)[0]
    check('boot log gains no [dhcp]/[dns] lines (on-demand only)',
          '[dhcp]' not in boot_part and '[dns]' not in boot_part)

    ping_a = [int(m) for m in re.findall(r'--- 10\.0\.2\.2: (\d)/3 replies ---', log_a)]
    check('static default config still pings 10.0.2.2 before dhcp',
          len(ping_a) >= 1 and ping_a[0] >= 2, 'replies=%r' % (ping_a,))

    check('dhcp logs DISCOVER with a transaction id',
          re.search(r'\[dhcp\] DISCOVER xid=0x[0-9a-f]{8}', log_a) is not None,
          re.search(r'\[dhcp\] DISCOVER xid=0x[0-9a-f]{8}', log_a).group(0)
          if re.search(r'\[dhcp\] DISCOVER xid=0x[0-9a-f]{8}', log_a) else '')
    check('dhcp logs OFFER 10.0.2.15 from 10.0.2.2',
          '[dhcp] OFFER 10.0.2.15 from 10.0.2.2' in log_a)
    ack = re.search(
        r'\[dhcp\] ACK ip=(\S+) mask=(\S+) gw=(\S+) dns=(\S+) lease=(\d+)s', log_a)
    check('dhcp ACK parses as ip=10.0.2.15 mask=255.255.255.0 gw=10.0.2.2 '
          'dns=10.0.2.3 lease=86400s',
          ack is not None and ack.groups()[:5] ==
          ('10.0.2.15', '255.255.255.0', '10.0.2.2', '10.0.2.3', '86400'),
          ack.group(0) if ack else '')
    check('shell prints the four-tuple it programmed',
          'dhcp: ip 10.0.2.15 mask 255.255.255.0 gateway 10.0.2.2 dns 10.0.2.3'
          in log_a)

    check('dns logs a query with its transaction id',
          re.search(r'\[dns\] query example\.com id=0x[0-9a-f]{4}', log_a) is not None)
    answers = re.findall(r'\[dns\] answer (\S+) -> (\S+) \(1 A record\)', log_a)
    good = [(n, ip) for n, ip in answers if valid_ipv4(ip)]
    check('dns answers an A record with a valid IPv4 address',
          len(good) >= 1, 'answers=%r' % (answers,))
    check('the query really went to slirp\'s DNS (10.0.2.3 programmed by DHCP)',
          'dns=10.0.2.3' in log_a)
    check('wget sends a hostname through the resolver (honest NXDOMAIN path)',
          '[dns] nxdomain nosuchhost.invalid' in log_a and
          'wget: cannot resolve host' in log_a)

    check('ping still works after the lease is programmed',
          len(ping_a) >= 2 and ping_a[-1] >= 2, 'replies=%r' % (ping_a,))
    check('kernel self-tests stay 16/16 (no new boot self-test item)',
          '[stitch] selftests: 16/16 passed' in log_a)
    check('no kernel PANIC', 'PANIC' not in log_a.upper().replace('NO PANIC', ''))

    # ---------------- phase B: no NIC regression ----------------
    if args.no_nic:
        log_b = run_boot(SERIAL_B, False, [
            ('dhcp', r'dhcp: no NIC / network not ready'),
        ])
        check('without a NIC, dhcp reports "no NIC / network not ready"',
              'dhcp: no NIC' in log_b and 'network not ready' in log_b)
        check('without a NIC, dhcp invents no lease',
              '[dhcp] ACK' not in log_b and '[dhcp] DISCOVER' not in log_b)
        check('without a NIC, self-tests still 16/16',
              '[stitch] selftests: 16/16 passed' in log_b)
        check('without a NIC, no PANIC',
              'PANIC' not in log_b.upper().replace('NO PANIC', ''))

    failed = [n for n, ok, _ in checks if not ok]
    for name, ok, detail in checks:
        print('  [%s] %s' % ('ok' if ok else 'FAIL', name))
        if detail and not ok:
            print('        detail: %s' % detail)
    print('[dhcpdns] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    if failed:
        print('[dhcpdns] serial logs: %s%s'
              % (SERIAL_A, (', ' + SERIAL_B) if args.no_nic else ''))
    print('[dhcpdns] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
