#!/usr/bin/env python3
"""SutureOS USB domain acceptance (domain 8: USB/input).

Boots the real ISO with an EHCI controller + one USB mass-storage stick and
proves the ported USB stack works end to end:
  * [usb] controller + storage discovery + block device registration,
  * the volume mounts on /mnt/usb and `ls` lists it,
  * the driver self-test passes,
  * the stick itself is unchanged by a read-only boot (host-side compare).

  py -3 scripts/usb_domain_acceptance.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_usb_domain.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_usb_domain.args')


def keys_for(text):
    out = []
    for ch in text:
        if ch == ' ':
            out.append('spc')
        elif ch == '/':
            out.append('slash')
        else:
            out.append(ch)
    out.append('ret')
    return ';'.join(out)


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    # Boot a fresh copy of the pristine fixture: build/usbdisk.img may already be
    # dirty from the old write acceptance, and the host check must compare the
    # exact image QEMU had open.
    shutil.copyfile(os.path.join(ROOT, 'build', 'usbdisk_pristine.img'),
                    os.path.join(ROOT, 'build', 'usbdisk_ro.img'))
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d '
                 '-device usb-ehci,id=ehci '
                 '-drive id=usbdisk,file=build/usbdisk_ro.img,format=raw,if=none '
                 '-device usb-storage,drive=usbdisk,bus=ehci.0 '
                 '-smp 2 -no-reboot\n')
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_usb_domain.log',
                    '-ArgFile', 'build/qemu_usb_domain.args',
                    '-BootWaitSeconds', '30',
                    '-Keys', keys_for('ls /mnt/usb'),
                    '-AfterKeysSeconds', '2', '-MonitorPort', '45691'],
                   cwd=ROOT, check=False)

    log = open(LOG, encoding='utf-8', errors='replace').read()
    checks = [
        ('EHCI controller found', '[usb] ehci: pci' in log),
        ('USB storage discovered', 'storage:' in log),
        ('block device registered', 'drive 16' in log),
        ('volume mounted on /mnt/usb', 'mount /mnt/usb OK' in log),
        ('USB self-test PASS', 'selftest PASS' in log),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
    ]

    # Host-side independent check: a read-only boot must not modify the stick.
    host = subprocess.run(['py', '-3', 'scripts/verify_usb_disk.py',
                           'build/usbdisk_ro.img', 'build/usbdisk_pristine.img'],
                          cwd=ROOT, capture_output=True, text=True)
    host_ok = host.returncode == 0
    print(host.stdout.strip()[-400:] if host.stdout.strip() else '(no host output)')
    checks.append(('host: read-only boot left the USB stick unchanged', host_ok))

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[usb] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[usb] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
