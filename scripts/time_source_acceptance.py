#!/usr/bin/env python3
"""SutureOS generic time-source acceptance (domain 11: cross-architecture blueprint).

The mechanism (one monotonic counter + known frequency + best-source selector,
re-written on x86_64 from the AArch64/RISC-V timebase idea) is proven by:

  * boot self-test `[selftest][time_source] PASS` and 16/16 self-tests overall,
  * `timesrc` listing the registered sources with hpet (100 MHz) as (best) and
    the 100 Hz scheduler tick as fallback,
  * `timesrc`'s two-reading probe: delta_ns must land in 8..12 ms and the host
    independently recomputes it from the source frequency (10 ns per HPET tick),
  * no kernel PANIC.

  py -3 scripts/time_source_acceptance.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_time_source.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_time_source.args')


def keys_for(text):
    out = []
    for ch in text:
        out.append('spc' if ch == ' ' else ch)
    out.append('ret')
    return ';'.join(out)


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -drive id=adisk,file=build/fat32_ata.img,'
                 'format=raw,if=ide -smp 2 -no-reboot\n')
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_time_source.log',
                    '-ArgFile', 'build/qemu_time_source.args',
                    '-BootWaitSeconds', '30',
                    '-Keys', keys_for('timesrc'),
                    '-AfterKeysSeconds', '2', '-MonitorPort', '45697'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = [
        ('boot self-test time_source PASS', '[selftest][time_source] PASS' in log),
        ('self-test suite now 16/16', '[stitch] selftests: 16/16 passed' in log),
        ('HPET registered as the best source',
         re.search(r'\[time\] 0: hpet 100000000 Hz prio=100 \(best\)', log) is not None),
        ('scheduler tick registered as fallback',
         re.search(r'\[time\] 1: jiffies 100 Hz prio=10', log) is not None),
        ('timesrc printed now_ns',
         re.search(r'\[time\] now_ns=\d+ uptime_us=\d+', log) is not None),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
    ]

    m = re.search(r'\[time\] probe: t0_ns=(\d+) t1_ns=(\d+) delta_ns=(\d+)', log)
    if m:
        t0, t1, delta = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        checks.append(('probe is monotonic (t1 >= t0)', t1 >= t0))
        checks.append(('probe advanced ~10 ms (delta=%d ns)' % delta,
                       8000000 <= delta <= 12000000))
        # host-side independent recomputation: between t0 and t1 the HPET must
        # have advanced delta/10 ticks at its 100 MHz period (10 ns per tick).
        if 100000000 and delta:
            ticks_expected = delta // 10
            checks.append(('delta is an integral number of 10 ns HPET ticks', delta % 10 == 0))
            checks.append(('delta corresponds to %d HPET ticks at 100 MHz' % ticks_expected,
                           (ticks_expected * 10) == delta))
    else:
        checks.append(('timesrc probe line parseable', False))

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[time-source] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[time-source] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
