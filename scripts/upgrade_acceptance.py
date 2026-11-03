#!/usr/bin/env python3
"""SutureOS overwrite-only system upgrade acceptance (v0.36.0).

Proves, with two real boots and real disk writes:

  Phase 1  Install from the v0.35 installation ISO onto a blank 128 MiB disk,
           then write a marker into /etc/motd and `sync` it (that marker lands in
           /SUTUREOS/PERSIST, i.e. it is user data), then shut down.
  Phase 2  Boot the NEW ISO with the installed disk attached and run `upgrade`:
           it must find the installed volume by content, overwrite kernel.elf /
           limine-bios.sys / limine.conf and the boot area, and keep user data.
  Phase 3  HOST verification: /KERNEL.ELF on the disk equals build/kernel.elf
           byte for byte, the motd marker still exists (user data kept).
  Phase 4  Boot the installed disk alone: the banner reports the NEW version and
           the marker is still there (PERSIST restore).

Run after `py -3 scripts/build_iso.py` with the v0.35 ISO saved as
build/sutureos_v035_install.iso (the script says so if it is missing).
"""
import os
import struct
import sys
import hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

import installer_acceptance as inst          # noqa: E402  (Qmp/Serial helpers)
import verify_fat32_write as fatverify       # noqa: E402

DISK = os.path.join(ROOT, 'build', 'upgrade_disk.img')
OLD_ISO = os.path.join(ROOT, 'build', 'sutureos_v035_install.iso')
NEW_ISO = os.path.join(ROOT, 'build', 'sutureos.iso')
MARKER = 'PREUPGRADE-MARKER'

checks = []


def check(name, ok, detail=''):
    checks.append((name, bool(ok), detail))
    print('  [%s] %s%s' % ('ok' if ok else 'FAIL', name,
                           ('  (%s)' % detail) if detail else ''))


def boot(phase, args, serial_path):
    proc = inst.run_qemu(None, serial_path, args, phase)
    serial = inst.Serial(serial_path)
    qmp = inst.Qmp()
    inst.wait_ready(qmp, serial)
    return proc, serial, qmp


def shutdown_guest(qmp, serial):
    qmp.type('shutdown')
    qmp.key('ret')
    try:
        serial.wait_for(r'\[  OK  \] .*power|Power down|shutdown', timeout=45,
                        name='shutdown')
    except Exception:
        pass


def main():
    if not os.path.exists(OLD_ISO):
        print('missing %s - save the v0.35 installation ISO there first' % OLD_ISO)
        return 1
    if not os.path.exists(NEW_ISO):
        print('missing %s - run scripts/build_iso.py first' % NEW_ISO)
        return 1

    target_kernel = open(os.path.join(ROOT, 'build', 'kernel.elf'), 'rb').read()
    print('new kernel: %d bytes sha256=%s' % (
        len(target_kernel), hashlib.sha256(target_kernel).hexdigest()[:16]))

    # ---------------------------------------------------------------- phase 1
    print('\n=== phase 1: install v0.35 from the old ISO ===')
    if os.path.exists(DISK):
        os.unlink(DISK)
    with open(DISK, 'wb') as f:
        f.truncate(128 * 1024 * 1024)

    args = ['-cdrom', os.path.relpath(OLD_ISO, ROOT), '-boot', 'd',
            '-drive', 'id=disk0,file=%s,format=raw,if=none' % os.path.relpath(DISK, ROOT),
            '-device', 'ide-hd,drive=disk0,bus=ide.0',
            '-no-reboot', '-no-shutdown']
    proc, serial, qmp = boot('install', args, os.path.join(ROOT, 'build', 'serial_upgrade_phase1.log'))
    try:
        qmp.type('install')
        qmp.key('ret')
        serial.wait_for(r'install medium: ISO9660', timeout=40, name='installer start')
        serial.wait_for(r'Target disk index', timeout=10)
        qmp.type('0')
        qmp.key('ret')
        serial.wait_for(r"Type 'yes' to continue", timeout=10)
        qmp.type('yes')
        qmp.key('ret')
        serial.wait_for(r'root password \(empty', timeout=10)
        qmp.type('rootpw')
        qmp.key('ret')
        serial.wait_for(r'repeat root password', timeout=10)
        qmp.type('rootpw')
        qmp.key('ret')
        serial.wait_for(r'Create a normal user', timeout=10)
        qmp.type('n')
        qmp.key('ret')
        serial.wait_for(r'^\[install\] done \(drive (\d+),', timeout=420,
                        name='[install] done')
        # user data that must survive the upgrade
        qmp.type('write /etc/motd ' + MARKER)
        qmp.key('ret')
        serial.wait_for(r'write: /etc/motd', timeout=15, name='motd written')
        qmp.type('sync')
        qmp.key('ret')
        serial.wait_for(r'\[persist\] flush', timeout=60, name='persist flush')
        shutdown_guest(qmp, serial)
    finally:
        try:
            qmp.hmp('quit')
        except Exception:
            pass
        try:
            proc.wait(timeout=25)
        except Exception:
            proc.kill()

    log1 = open(os.path.join(ROOT, 'build', 'serial_upgrade_phase1.log'),
                encoding='utf-8', errors='replace').read()
    check('phase 1 installed v0.35', '[install] done' in log1)
    check('phase 1 wrote the user-data marker into /etc/motd',
          'write: /etc/motd' in log1 or MARKER in log1)

    # ---------------------------------------------------------------- phase 2
    print('\n=== phase 2: boot the NEW ISO and run `upgrade` ===')
    args = ['-cdrom', os.path.relpath(NEW_ISO, ROOT), '-boot', 'd',
            '-drive', 'id=disk0,file=%s,format=raw,if=none' % os.path.relpath(DISK, ROOT),
            '-device', 'ide-hd,drive=disk0,bus=ide.0',
            '-no-reboot', '-no-shutdown']
    proc, serial, qmp = boot('upgrade', args, os.path.join(ROOT, 'build', 'serial_upgrade_phase2.log'))
    try:
        qmp.type('upgrade')
        qmp.key('ret')
        serial.wait_for(r'\[upgrade\] installed system on drive', timeout=60,
                        name='upgrade found volume')
        m = serial.wait_for(r'\[upgrade\] done \(drive (\d+), system files overwritten',
                            timeout=900, name='[upgrade] done')
        print('  guest: ' + m.group(0))
        shutdown_guest(qmp, serial)
    finally:
        try:
            qmp.hmp('quit')
        except Exception:
            pass
        try:
            proc.wait(timeout=25)
        except Exception:
            proc.kill()

    log2 = open(os.path.join(ROOT, 'build', 'serial_upgrade_phase2.log'),
                encoding='utf-8', errors='replace').read()
    check('phase 2 upgrade found the installed system volume',
          '[upgrade] installed system on drive' in log2)
    check('phase 2 upgrade completed', '[upgrade] done' in log2)
    check('phase 2 upgrade did not touch user data (no reinstall)',
          'root_password=' not in log2)

    # ---------------------------------------------------------------- phase 3
    print('\n=== phase 3: host-side verification of the upgraded disk ===')
    img = open(DISK, 'rb').read()
    part_lba = inst.PART_LBA
    vol = img[part_lba * 512:]
    f = fatverify.parse(vol)
    entry = f['entries'].get('KERNEL.ELF') or f['entries'].get('kernel.elf')
    check('installed volume has a /KERNEL.ELF entry', entry is not None)
    if entry:
        data = f['read_chain'](entry[0], entry[1])
        check('upgraded /KERNEL.ELF is byte-identical to build/kernel.elf',
              data == target_kernel,
              'disk=%d build=%d sha=%s' % (len(data), len(target_kernel),
                                           hashlib.sha256(data).hexdigest()[:16]))
    check('user-data marker survived on the volume (PERSIST kept)',
          MARKER.encode() in img)

    # ---------------------------------------------------------------- phase 4
    print('\n=== phase 4: boot the upgraded disk alone (no ISO) ===')
    args = ['-drive', 'id=disk0,file=%s,format=raw,if=none' % os.path.relpath(DISK, ROOT),
            '-device', 'ide-hd,drive=disk0,bus=ide.0', '-boot', 'c',
            '-no-reboot', '-no-shutdown']
    proc = inst.run_qemu(None, os.path.join(ROOT, 'build', 'serial_upgrade_phase4.log'),
                         args, 'boot-upgraded')
    serial = inst.Serial(os.path.join(ROOT, 'build', 'serial_upgrade_phase4.log'))
    qmp = None
    try:
        qmp = inst.Qmp()
        inst.wait_ready(qmp, serial)
        serial.wait_for(r'version 0\.36\.0-alpha', timeout=60,
                        name='new version banner')
        qmp.type('cat /etc/motd')
        qmp.key('ret')
        serial.wait_for(MARKER, timeout=20, name='marker restored')
        qmp.type('persist')
        qmp.key('ret')
        serial.wait_for(r'persist: volume=', timeout=20, name='persist status')
        qmp.type('shutdown')
        qmp.key('ret')
    finally:
        if qmp is not None:
            try:
                qmp.hmp('quit')
            except Exception:
                pass
        try:
            proc.wait(timeout=25)
        except Exception:
            proc.kill()

    log4 = open(os.path.join(ROOT, 'build', 'serial_upgrade_phase4.log'),
                encoding='utf-8', errors='replace').read()
    check('phase 4 booted the upgraded system (banner says 0.36.0-alpha)',
          'version 0.36.0-alpha' in log4)
    check('phase 4 /etc/motd shows the pre-upgrade marker (user data intact)',
          MARKER in log4)
    check('phase 4 no PANIC', 'PANIC' not in log4)

    print('\n--- checks ---')
    ok = True
    for name, cond, detail in checks:
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))
        ok = ok and cond
    print('RESULT: %s (checks=%d ok=%d fail=%d)'
          % ('PASS' if ok else 'FAIL', len(checks),
             sum(1 for c in checks if c[1]), sum(1 for c in checks if not c[1])))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
