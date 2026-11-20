#!/usr/bin/env python3
"""SutureOS v0.60: Wayland protocol completion acceptance.

The minimal server (kernel/ipc/wl_server.c) grew the pieces it was still missing,
and this script proves each one end to end from ring 3 in real QEMU:

  * wl_surface.frame(new_id) + wl_callback.done(serial) on the next commit
    (`[wl] frame callback done id=10 serial=2`);
  * wl_surface.damage accumulation, logged at commit and folded into the commit
    line (`[wl] damage surface=9 32,32 32x32`);
  * a second commit with different pixels (frame 2 hash != frame 1 hash) and two
    wl_buffer.release events;
  * wl_display.error for a request naming an object that was never created
    (`[wl] display.error object=99 code=0`), and wl_display.delete_id after a
    client-initiated destroy;
  * wl_seat.get_pointer producing a real wl_pointer that receives
    enter/motion (from the kernel's PS/2 pointer snapshot);
  * the client still exporting frame #1 to /mnt/fat/WL.BIN and reporting
    `[wlclient] RESULT PASS`.

Host-side verification (never trusting the guest's numbers):
  * WL.BIN is parsed out of build/fat32_ata.img with scripts/verify_fat32_write
    and compared byte-for-byte with frame #1;
  * FNV-1a of frame #1 is compared with the FIRST `[wl] commit ... hash=` and the
    client's `[wlclient] pixels hash=`;
  * FNV-1a of frame #2 (frame #1 with the 32x32 block recoloured to 0xFF112233) is
    compared with the SECOND commit hash.  Only frame #1 is written to disk: the
    FAT32 write path manages ~20 KB/s under TCG, so exporting both frames would
    double a ~13 s step for no extra proof.

  py -3 scripts/build.py && py -3 scripts/build_iso.py
  py -3 scripts/wl_protocol_acceptance.py
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_wlproto.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_wlproto.args')
IMG = os.path.join(ROOT, 'build', 'fat32_ata.img')

KEYS = ';'.join(['r', 'u', 'n', 'spc'] + list('wlclient') + ['ret'])

SURF_W, SURF_H, SURF_STRIDE = 320, 200, 320 * 4
SURF_BYTES = SURF_STRIDE * SURF_H
DAMAGE_X, DAMAGE_Y, DAMAGE_W, DAMAGE_H = 32, 32, 32, 32
DAMAGE_COLOR = 0xFF112233


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def expected_pattern():
    """Frame #1: the exact bytes user/apps/wlclient.c writes into its pool."""
    out = bytearray(SURF_BYTES)
    for y in range(SURF_H):
        row = y * SURF_STRIDE
        gy = (y * 2) & 0xFF
        for x in range(SURF_W):
            gx = (x * 2) & 0xFF
            if ((x >> 5) + (y >> 5)) & 1:
                c = 0xFF000000 | (gx << 16) | (gy << 8) | 0x40
            else:
                c = 0xFF000000 | (gy << 16) | 0x20 | (gx << 8)
            out[row + x * 4:row + x * 4 + 4] = c.to_bytes(4, 'little')
    return bytes(out)


def expected_frame2(frame1):
    """Frame #2: frame #1 with the damaged 32x32 block recoloured."""
    out = bytearray(frame1)
    colour = DAMAGE_COLOR.to_bytes(4, 'little')
    for y in range(DAMAGE_Y, DAMAGE_Y + DAMAGE_H):
        row = y * SURF_STRIDE
        for x in range(DAMAGE_X, DAMAGE_X + DAMAGE_W):
            out[row + x * 4:row + x * 4 + 4] = colour
    return bytes(out)


def regenerate_fixture():
    """A fresh FAT32 volume, then the copy QEMU will actually write into."""
    r = subprocess.run(['py', '-3', os.path.join('scripts', 'mkfat32_testimg.py')],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print('[wlproto] fixture generation failed:')
        print(r.stdout[-800:])
        print(r.stderr[-800:])
        return False
    shutil.copyfile(os.path.join(ROOT, 'build', 'fat32.img'), IMG)
    return True


def run_qemu():
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide\n')
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_wlproto.log',
                    '-ArgFile', 'build/qemu_wlproto.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45911',
                    # The client's one 250 KiB FAT32 write manages ~20 KB/s under
                    # TCG (~13 s); the rest of the protocol tail is milliseconds.
                    '-Keys', KEYS, '-AfterKeysSeconds', '60'],
                   cwd=ROOT, check=False)
    return open(LOG, encoding='utf-8', errors='replace').read()


def main():
    if not os.path.exists(os.path.join(ROOT, 'build', 'sutureos.iso')):
        print('[wlproto] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2
    if not regenerate_fixture():
        print('[wlproto] RESULT: FAIL (no fixture)')
        return 1
    log = run_qemu()

    commits = re.findall(r'\[wl\] commit surface=(\d+) buffer=(\d+) (\d+)x(\d+) '
                         r'stride=(\d+) hash=([0-9a-f]{8}) \((\d+) bytes from memfd '
                         r'fd=\d+\)(?: damage=(\d+),(\d+) (\d+)x(\d+))? serial=(\d+)',
                         log)
    frame_done = re.search(r'\[wl\] frame callback done id=(\d+) serial=(\d+)', log)
    client_frame = re.search(r'\[wlclient\] frame done serial=(\d+)', log)
    srv_enter = re.search(r'\[wl\] pointer enter id=(\d+) surface=(\d+) (-?\d+),(-?\d+) '
                          r'\(kernel mouse (-?\d+),(-?\d+) serial=\d+\)', log)
    cli_enter = re.search(r'\[wlclient\] pointer enter (-?\d+),(-?\d+)', log)
    srv_motion = re.search(r'\[wl\] pointer motion id=(\d+) time=(\d+) (-?\d+),(-?\d+)',
                           log)
    cli_motion = re.search(r'\[wlclient\] pointer motion (-?\d+),(-?\d+)', log)
    srv_error = re.search(r'\[wl\] display\.error object=(\d+) code=(\d+) '
                          r'message="([^"]*)"', log)
    cli_error = re.search(r'\[wlclient\] display error object=(\d+) code=(\d+)', log)
    client_hash = re.search(r'\[wlclient\] pixels hash=([0-9a-f]{8}) bytes=(\d+)', log)

    # ---- host-side, independent verification of the pixel bytes -----------
    host_ok = False
    host_detail = ''
    wl_bin = None
    host_hash = None
    try:
        sys.path.insert(0, os.path.join(ROOT, 'scripts'))
        import verify_fat32_write as v
        img = open(IMG, 'rb').read()
        fs = v.parse(img)
        if 'WL.BIN' not in fs['entries']:
            host_detail = 'WL.BIN not found in the FAT32 image'
        else:
            first, size = fs['entries']['WL.BIN']
            wl_bin = fs['read_chain'](first, size)[:size]
            host_hash = fnv1a(wl_bin)
            expect = expected_pattern()
            if size != SURF_BYTES:
                host_detail = 'WL.BIN is %d bytes, expected %d' % (size, SURF_BYTES)
            elif wl_bin != expect:
                first_bad = next(i for i in range(SURF_BYTES)
                                 if wl_bin[i] != expect[i])
                host_detail = ('WL.BIN differs from frame #1 at byte %d'
                               % first_bad)
            else:
                host_ok = True
                host_detail = ('WL.BIN is %d bytes, byte-exact against frame #1, '
                               'FNV-1a %08x' % (size, host_hash))
    except Exception as exc:                    # noqa: BLE001 - report, never hide
        host_detail = 'host FAT32 parse failed: %r' % (exc,)

    frame1 = expected_pattern()
    frame2 = expected_frame2(frame1)
    h1, h2 = fnv1a(frame1), fnv1a(frame2)
    commit_hashes = [int(c[5], 16) for c in commits]
    releases = len(re.findall(r'\[wl\] buffer\.release id=8', log))

    checks = [
        ('server ready + client connected',
         '[wl] minimal Wayland server ready' in log and
         '[wl] client connected name="wayland-0"' in log),
        ('exactly two wl_surface commits', len(commits) == 2,
         '%d commit line(s)' % len(commits)),
        ('commit #1 is 320x200 stride=1280 with the frame-1 hash',
         len(commits) >= 1 and commits[0][2] == str(SURF_W) and
         commits[0][3] == str(SURF_H) and commits[0][4] == str(SURF_STRIDE) and
         int(commits[0][5], 16) == h1),
        # re.findall() yields '' (not None) for a group that did not take part in
        # the match, so "no damage" is the falsy string here.
        ('commit #1 has no damage rect', len(commits) >= 1 and not commits[0][7]),
        ('commit #2 carries the damage rect 32,32 32x32',
         len(commits) == 2 and commits[1][7] == str(DAMAGE_X) and
         commits[1][8] == str(DAMAGE_Y) and commits[1][9] == str(DAMAGE_W) and
         commits[1][10] == str(DAMAGE_H),
         ('damage=%s,%s %sx%s' % commits[1][7:11]) if len(commits) == 2
         else 'no 2nd commit'),
        ('commit #2 hash is the frame-2 hash (pixels really changed)',
         len(commits) == 2 and int(commits[1][5], 16) == h2 and h2 != h1),
        ('[wl] damage line logged on commit',
         re.search(r'\[wl\] damage surface=9 32,32 32x32', log) is not None),
        ('wl_surface.frame registered the callback',
         '[wl] surface.frame surface=9 callback=10' in log),
        ('wl_callback.done fired on the next commit',
         frame_done is not None and frame_done.group(1) == '10',
         frame_done.group(0) if frame_done else 'missing'),
        ('callback serial is monotonic and matches the commit serial',
         frame_done is not None and len(commits) == 2 and
         frame_done.group(2) == commits[1][11] and int(commits[1][11]) >
         int(commits[0][11])),
        ('client saw wl_callback.done for the frame request',
         client_frame is not None and frame_done is not None and
         client_frame.group(1) == frame_done.group(2),
         client_frame.group(0) if client_frame else 'missing'),
        ('client logged "second commit ok"', '[wlclient] second commit ok' in log),
        ('two wl_buffer.release events', releases == 2, '%d release(s)' % releases),
        ('wl_callback.destroy accepted by the server',
         '[wl] callback.destroy id=10 (done=1' in log),
        ('wl_display.delete_id emitted for the destroyed callback',
         '[wl] display.delete_id id=10' in log and
         '[wlclient]   event wl_display.delete_id id=10' in log),
        ('wl_seat.get_pointer created a real wl_pointer',
         '[wl] seat.get_pointer id=11 (real wl_pointer' in log),
        ('wl_pointer.enter carries the surface + coordinates',
         srv_enter is not None and srv_enter.group(2) == '9',
         srv_enter.group(0) if srv_enter else 'missing'),
        ('wl_pointer.motion emitted after enter', srv_motion is not None,
         srv_motion.group(0) if srv_motion else 'missing'),
        ('client received enter/motion with the server coordinates',
         cli_enter is not None and cli_motion is not None and
         srv_enter is not None and srv_motion is not None and
         (cli_enter.group(1), cli_enter.group(2)) ==
         (srv_enter.group(3), srv_enter.group(4)) and
         (cli_motion.group(1), cli_motion.group(2)) ==
         (srv_motion.group(3), srv_motion.group(4)),
         '%s / %s' % (cli_enter.group(0) if cli_enter else 'missing',
                      cli_motion.group(0) if cli_motion else 'missing')),
        ('pointer coordinates were clamped into the 320x200 surface',
         srv_enter is not None and 0 <= int(srv_enter.group(3)) < SURF_W and
         0 <= int(srv_enter.group(4)) < SURF_H),
        ('wl_pointer.release accepted by the server',
         '[wl] pointer.release id=11' in log),
        ('wl_display.error emitted for the unknown object',
         srv_error is not None and srv_error.group(1) == '99' and
         srv_error.group(2) == '0',
         srv_error.group(0) if srv_error else 'missing'),
        ('client received and logged wl_display.error',
         cli_error is not None and cli_error.group(1) == '99'),
        ('client reported RESULT PASS', '[wlclient] RESULT PASS' in log),
        ('kernel self-tests still 16/16',
         '[stitch] selftests: 16/16 passed' in log),
        ('no kernel PANIC',
         'PANIC' not in log.upper().replace('NO PANIC', '')),
        ('HOST: WL.BIN is frame #1, byte-exact, parsed from the image', host_ok),
        ('HOST: WL.BIN FNV-1a == commit #1 hash == client-reported hash',
         host_ok and len(commit_hashes) >= 1 and host_hash == commit_hashes[0] and
         client_hash is not None and host_hash == int(client_hash.group(1), 16)),
        ('HOST: recomputed frame #2 FNV-1a == commit #2 hash',
         len(commit_hashes) == 2 and commit_hashes[1] == h2),
    ]

    print('[wlproto] frame1 hash computed on host: %08x' % h1)
    print('[wlproto] frame2 hash computed on host: %08x' % h2)
    print('[wlproto] kernel commit hashes: %s'
          % ', '.join('%08x' % h for h in commit_hashes))
    print('[wlproto] client pixels hash: %s'
          % (client_hash.group(1) if client_hash else 'MISSING'))
    print('[wlproto] host WL.BIN hash: %s'
          % ('%08x' % host_hash if host_hash is not None else 'MISSING'))
    print('[wlproto] host check: %s' % host_detail)
    if srv_enter:
        print('[wlproto] pointer: kernel mouse %s,%s -> surface %s,%s'
              % (srv_enter.group(5), srv_enter.group(6), srv_enter.group(3),
                 srv_enter.group(4)))

    failed = [name for name, cond, *_ in checks if not cond]
    for name, cond, *rest in checks:
        detail = ('  (%s)' % rest[0]) if rest and rest[0] else ''
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, detail))
    print('[wlproto] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[wlproto] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
