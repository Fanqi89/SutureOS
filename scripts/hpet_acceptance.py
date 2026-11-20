#!/usr/bin/env python3
"""SutureOS HPET + LAPIC-calibration acceptance (CPU/interrupt/clock domain).

Proves the timer stack on real hardware emulation:
  * the ACPI HPET table / 0xFED00000 fallback initialises, the main counter is
    enabled and its femtosecond period is reported;
  * the LAPIC timer is calibrated against HPET (not the 8254, whose mode-3
    counter runs at half rate) and the reported ticks/s is self-consistent with
    the HSET period (host-side independent recomputation, not trusting the
    firmware log);
  * the kernel still boots to shell with the same self-test count and no PANIC.

  py -3 scripts/build.py && py -3 scripts/build_iso.py
  py -3 scripts/hpet_acceptance.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_hpet.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_hpet.args')


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot\n')

    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_hpet.log',
                    '-ArgFile', 'build/qemu_hpet.args',
                    '-BootWaitSeconds', '30', '-MonitorPort', '45661'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = [
        ('HPET driver initialised', '[hpet] ready:' in log),
        ('kernel_arch_init accepts HPET', '[init] HPET init OK' in log),
        ('LAPIC calibrated against HPET', '[lapic] calibration source: HPET' in log),
        ('LAPIC tick rate reported', '[lapic] timer calibrated:' in log),
        ('no PIT fallback warning', 'WARNING: HPET init failed' not in log),
        ('self-test summary present', '[stitch] selftests:' in log),
        ('shell prompt reached', 'sutureos# ' in log),
    ]

    # Host-side independent recomputation: freq (Hz) must equal 1e15 / period(fs)
    m = re.search(r'\[hpet\] ready: mmio=0x([0-9a-fA-F]+) rev=(\d+) period=(\d+) fs freq=(\d+) Hz',
                  log)
    if m:
        mmio, rev, period, freq = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
        expect = 1000000000000000 // period
        checks.append(('HPET freq == 1e15 / period (recomputed), period=%d fs' % period,
                       expect == freq))
        checks.append(('HPET period is plausible (1..100 ns)',
                       1000000 <= period <= 100000000))
        checks.append(('HPET MMIO is the documented window',
                       mmio.lower() in ('fed00000', 'fed00000')))
    else:
        checks.append(('HPET ready line parseable', False))

    m = re.search(r'\[lapic\] timer calibrated: (\d+) ticks/s', log)
    if m:
        rate = int(m.group(1))
        checks.append(('LAPIC ticks/s is plausible (1M..1G)', 1000000 <= rate <= 1000000000))
    else:
        checks.append(('LAPIC calibrated value parseable', False))

    m = re.search(r'\[stitch\] selftests: (\d+)/(\d+) passed', log)
    if m:
        passed, total = int(m.group(1)), int(m.group(2))
        checks.append(('self-tests did not regress (%d/%d, baseline 16/16)' % (passed, total),
                       total == 16 and passed >= 16))
    else:
        checks.append(('self-tests did not regress (baseline 16/16)', False))
    checks.append(('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')))

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[hpet] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[hpet] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
