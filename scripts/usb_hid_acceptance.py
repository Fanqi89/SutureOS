#!/usr/bin/env python3
"""SutureOS USB HID acceptance: EHCI boot keyboard/mouse + hot-plug rescan.

Two real QEMU boots of the built ISO, driven through QMP only -- the host
re-parses the serial log itself (regular expressions), so nothing the guest
prints is trusted as "the test passed":

  phase 1  one EHCI controller + usb-kbd + usb-mouse + usb-tablet
           * boot-time claim: "[usb-hid] kbd addr=N iface=0 ep=0x81 mps=8" and
             the matching mouse line => >= 2 HID devices found,
           * the usb-tablet is not a boot interface: its report descriptor
             length is logged instead of decoding reports,
           * `usbinfo` (typed on the USB keyboard) lists the devices,
           * sendkey a reaches the guest as a boot keyboard report:
             "[usb-hid] key down=0x04 ascii=a mods=0x00",
           * an HMP mouse_move reaches the boot mouse: "[usb-hid] mouse dx=...",
           * a QMP hot-plug (`device_add usb-kbd,id=kbd2,bus=ehci.0`) is
             reported as "[usb-hid] hotplug: new HID on port N",
           * the kernel self-test line is unchanged (16/16), no PANIC.
  phase 2  one EHCI controller + two usb-kbd devices, one removed at runtime
           * `device_del kbd2` is reported as "[usb-hid] unplug: port N".

The key code is the real HID usage id: QEMU's usb-kbd sends the boot report
0x04 for the 'a' key, and 0x04 is exactly what the driver decodes.

Note on typing: QEMU hands injected keys to the USB keyboard as soon as one
exists (verified with a usb-kbd on a UHCI controller the guest never touches:
the PS/2 port then delivers nothing), which is why the shell drains decoded USB
characters as well - see shell_hid_fill() / usb_hid_take_char().

  py -3 scripts/build.py && py -3 scripts/build_iso.py
  py -3 scripts/usb_hid_acceptance.py
"""
import os
import re
import socket
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG1 = os.path.join(ROOT, 'build', 'serial_usb_hid.log')
LOG2 = os.path.join(ROOT, 'build', 'serial_usb_hid_unplug.log')
ARGS1 = os.path.join(ROOT, 'build', 'qemu_usb_hid.args')
ARGS2 = os.path.join(ROOT, 'build', 'qemu_usb_hid_unplug.args')


def free_port(start):
    """First QMP port nobody is listening on.

    Other porting worktrees run their own QEMUs on this host, and a busy port
    would silently connect qemu_boot.ps1 to somebody else's VM (its monitor
    commands would then land on that VM). Pick a free one instead.
    """
    for port in range(start, start + 40):
        s = socket.socket()
        try:
            s.settimeout(0.3)
            if s.connect_ex(('127.0.0.1', port)) != 0:
                return port
        finally:
            s.close()
    raise SystemExit('no free QMP port near %d' % start)


# QMP sendkey names: letters are single characters, a space is "spc".
def keys_for(text):
    out = []
    for ch in text:
        out.append('spc' if ch == ' ' else ch)
    out.append('ret')
    return out


def run_qemu(args_file, serial_log, keys, monitor_cmds, boot_wait,
             after_keys, after_monitor, port):
    if os.path.exists(serial_log):
        os.remove(serial_log)
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', os.path.relpath(serial_log, ROOT),
                    '-ArgFile', os.path.relpath(args_file, ROOT),
                    '-BootWaitSeconds', str(boot_wait),
                    '-Keys', ';'.join(keys),
                    '-AfterKeysSeconds', str(after_keys),
                    '-AfterMonitorSeconds', str(after_monitor),
                    '-MonitorCmds', monitor_cmds,
                    '-MonitorPort', str(port)],
                   cwd=ROOT, check=False)
    return open(serial_log, encoding='utf-8', errors='replace').read()


def main():
    # ---- phase 1: discovery, key report, mouse report, hot-plug -----------
    with open(ARGS1, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot '
                 '-no-shutdown '
                 '-device usb-ehci,id=ehci '
                 '-device usb-kbd,bus=ehci.0 '
                 '-device usb-mouse,bus=ehci.0 '
                 # usb-tablet is a HID device that is NOT a boot interface:
                 # it exercises the "report descriptor length is logged, the
                 # report format is not decoded" branch.
                 '-device usb-tablet,bus=ehci.0\n')
    # `usbinfo` (list), `usbinfo watch` (20 s live monitor: the hot-plug and the
    # mouse move below land while the guest is polling), then a real 'a' press
    # held for 400 ms (HMP sendkey hold time) so the boot-report sampling has
    # several chances even on a slow TCG host.
    keys = keys_for('usbinfo') + keys_for('usbinfo watch') + ['a 400']
    log1 = run_qemu(ARGS1, LOG1, keys,
                    'device_add usb-kbd,id=kbd2,bus=ehci.0;mouse_move 6 6',
                    boot_wait=35, after_keys=8, after_monitor=6,
                    port=free_port(45871))

    # ---- phase 2: unplug --------------------------------------------------
    with open(ARGS2, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot '
                 '-no-shutdown '
                 '-device usb-ehci,id=ehci '
                 '-device usb-kbd,bus=ehci.0 '
                 '-device usb-kbd,id=kbd2,bus=ehci.0\n')
    log2 = run_qemu(ARGS2, LOG2, [], 'device_del kbd2',
                    boot_wait=35, after_keys=0, after_monitor=5,
                    port=free_port(45911))

    # ---- host-side, independent log parsing -------------------------------
    kbd_line = re.compile(r'\[usb-hid\] kbd addr=(\d+) iface=(\d+) '
                          r'ep=0x([0-9a-f]+) mps=(\d+)')
    mouse_line = re.compile(r'\[usb-hid\] mouse addr=(\d+) iface=(\d+) '
                            r'ep=0x([0-9a-f]+) mps=(\d+)')
    key_event = re.compile(r'\[usb-hid\] key down=0x04 ascii=a mods=0x[0-9a-f]{2}')
    mouse_event = re.compile(r'\[usb-hid\] mouse dx=-?\d+ dy=-?\d+ btn=\d+')
    nonboot_line = re.compile(r'\[usb-hid\] hid addr=\d+ iface=\d+ class=3 '
                              r'sub=\d+ proto=\d+ report descriptor len=(\d+) ')
    hotplug_line = re.compile(r'\[usb-hid\] hotplug: new HID on port \d+')
    unplug_line = re.compile(r'\[usb-hid\] unplug: port \d+')
    guest_count = re.compile(r'usbinfo: (\d+) HID device\(s\)')
    guest_kbd_list = re.compile(r'dev\d+ kbd\s+usb-kbd\(addr=\d+,ep=0x81\)')
    selftests = re.compile(r'\[stitch\] selftests: (\d+)/(\d+) passed')

    hid_devices = len(kbd_line.findall(log1)) + len(mouse_line.findall(log1))
    m = guest_count.search(log1)
    guest_hid_count = int(m.group(1)) if m else -1
    st = selftests.search(log1)
    st_ok = bool(st) and st.group(1) == st.group(2) == '16'

    def no_panic(text):
        return 'PANIC' not in text.upper().replace('NO PANIC', '')

    checks = [
        # phase 1: discovery (log lines emitted by the HID class driver)
        ('kbd claimed at boot: [usb-hid] kbd addr=N iface=0 ep=0x81 mps=8',
         len(kbd_line.findall(log1)) >= 1 and
         kbd_line.findall(log1)[0][2] == '81' and
         kbd_line.findall(log1)[0][3] == '8'),
        ('mouse claimed at boot: [usb-hid] mouse addr=N ...',
         len(mouse_line.findall(log1)) >= 1),
        ('non-boot HID: report descriptor length logged (usb-tablet)',
         bool(nonboot_line.search(log1)) and
         int(nonboot_line.search(log1).group(1)) > 0),
        ('HID devices discovered >= 2', hid_devices >= 2),
        ('`usbinfo` reports >= 2 HID devices', guest_hid_count >= 2),
        ('`usbinfo` lists the keyboard by name', bool(guest_kbd_list.search(log1))),
        # phase 1: real key report
        ('key event 0x04 ("a") decoded: [usb-hid] key down=0x04 ascii=a',
         bool(key_event.search(log1))),
        # phase 1: 3-byte boot mouse report decoded
        ('mouse move decoded: [usb-hid] mouse dx=.. dy=.. btn=..',
         bool(mouse_event.search(log1))),
        # phase 1: hot-plug
        ('hot-plug after device_add usb-kbd: [usb-hid] hotplug: new HID',
         bool(hotplug_line.search(log1))),
        # phase 2: unplug
        ('unplug after device_del kbd2: [usb-hid] unplug: port N',
         bool(unplug_line.search(log2))),
        ('two keyboards claimed in phase 2',
         len(re.findall(r'\[usb-hid\] kbd addr=', log2)) >= 2),
        # stability
        ('kernel self-tests still 16/16', st_ok),
        ('no kernel PANIC (phase 1)', no_panic(log1)),
        ('no kernel PANIC (phase 2)', no_panic(log2)),
    ]

    print('[usb-hid] HID devices in phase 1: %d (usbinfo said %d)' %
          (hid_devices, guest_hid_count))
    print('[usb-hid] mouse reports decoded: %d' %
          len(mouse_event.findall(log1)))

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[usb-hid] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[usb-hid] RESULT:', 'PASS' if not failed else 'FAIL')
    if failed:
        print('[usb-hid] serial logs: %s , %s' % (LOG1, LOG2))
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
