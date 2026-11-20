#!/usr/bin/env python3
"""SutureOS minimal Wayland server acceptance (v0.58.0).

Boots the ISO, types `run wlclient` and proves the whole Wayland path end to end
from ring 3:

  * the named AF_UNIX endpoint works both ways (bind / duplicate-bind refused /
    connect to "wayland-0" / connect to a missing name -> -ENOENT);
  * wl_display.get_registry -> wl_registry.global x3, wl_display.sync ->
    wl_callback.done, wl_registry.bind for wl_compositor/wl_shm/wl_seat,
    wl_seat.capabilities;
  * wl_shm.create_pool receives the client's memfd through SCM_RIGHTS,
    wl_shm_pool.create_buffer + wl_compositor.create_surface +
    wl_surface.attach/commit work, and the server reads the pixels out of the
    shared pool, logging `[wl] commit ... hash=XXXXXXXX`;
  * wl_buffer.release comes back and the client prints `[wlclient] RESULT PASS`;
  * no PANIC and the kernel self-tests are still 16/16.

The hash check is done on the HOST, not trusted from the guest: the client writes
the same pixels to /mnt/fat/WL.BIN, which this script parses out of
build/fat32_ata.img with scripts/verify_fat32_write.parse(), recomputes FNV-1a
itself and also rebuilds the expected checkerboard byte-for-byte.

  py -3 scripts/build.py && py -3 scripts/build_iso.py
  py -3 scripts/wl_server_acceptance.py
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_wl.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_wl.args')
IMG = os.path.join(ROOT, 'build', 'fat32_ata.img')

# `run wlclient` -> r;u;n;spc;w;l;c;l;i;e;n;t;ret
KEYS = ';'.join(['r', 'u', 'n', 'spc'] + list('wlclient') + ['ret'])

SURF_W, SURF_H, SURF_STRIDE = 320, 200, 320 * 4
SURF_BYTES = SURF_STRIDE * SURF_H


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def expected_pattern():
    """The exact bytes user/apps/wlclient.c writes into its shared pool."""
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


def regenerate_fixture():
    """A fresh FAT32 volume, then the copy QEMU will actually write into."""
    r = subprocess.run(['py', '-3', os.path.join('scripts', 'mkfat32_testimg.py')],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print('[wl] fixture generation failed:')
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
                    '-SerialLog', 'build/serial_wl.log',
                    '-ArgFile', 'build/qemu_wl.args',
                    '-BootWaitSeconds', '45', '-MonitorPort', '45901',
                    # The client's last step is writing the 250 KiB pixel copy to
                    # WL.BIN through the FAT32 write path, which is slow under TCG
                    # (~20 KB/s). 8 s cut the file off at 151552/256000 bytes, so the
                    # window is generous on purpose; the run still ends the moment
                    # QEMU quits, it is not a sleep to pad the test.
                    '-Keys', KEYS, '-AfterKeysSeconds', '35'],
                   cwd=ROOT, check=False)
    return open(LOG, encoding='utf-8', errors='replace').read()


def main():
    if not regenerate_fixture():
        print('[wl] RESULT: FAIL (no fixture)')
        return 1
    log = run_qemu()

    commit = re.search(r'\[wl\] commit surface=(\d+) buffer=(\d+) (\d+)x(\d+) '
                       r'stride=(\d+) hash=([0-9a-f]{8}) \((\d+) bytes from memfd '
                       r'fd=(\d+)\)', log)
    pool = re.search(r'\[wl\] create_pool id=(\d+) fd=(\d+) size=(\d+)'
                     r'(?: \(memfd (\d+) bytes\))?', log)
    client_hash = re.search(r'\[wlclient\] pixels hash=([0-9a-f]{8}) bytes=(\d+)',
                            log)

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
                first_bad = next(i for i in range(SURF_BYTES) if wl_bin[i] != expect[i])
                host_detail = ('WL.BIN differs from the expected pattern at byte %d'
                               % first_bad)
            else:
                host_ok = True
                host_detail = ('WL.BIN is %d bytes, byte-exact against the expected '
                               'pattern, FNV-1a %08x' % (size, host_hash))
    except Exception as exc:                    # noqa: BLE001 - report, never hide
        host_detail = 'host FAT32 parse failed: %r' % (exc,)

    kernel_hash = int(commit.group(6), 16) if commit else None

    checks = [
        ('server started lazily on connect', '[wl] minimal Wayland server ready' in log),
        ('named endpoint "wayland-0" bound', '[wl] named endpoint "wayland-0" bound' in log),
        ('client connected', '[wl] client connected name="wayland-0"' in log),
        ('ring-3 bind() accepted', 'ok   bind(fd, "wlclient-test") succeeds' in log),
        ('duplicate bind refused (EADDRINUSE)',
         'ok   a duplicate bind of the same name is refused (EADDRINUSE)' in log),
        ('connect() to a missing name -> -ENOENT',
         'ok   connect() to an unbound name fails with -ENOENT' in log),
        ('wl_display.get_registry dispatched', '[wl] get_registry id=' in log),
        ('registry advertised 3 globals', 'ok   registry advertised >= 3 globals' in log),
        ('bind wl_compositor', '[wl] bind id=3 interface="wl_compositor"' in log),
        ('bind wl_shm', '[wl] bind id=4 interface="wl_shm"' in log),
        ('bind wl_seat', '[wl] bind id=5 interface="wl_seat"' in log),
        ('wl_seat.capabilities emitted', '[wl] seat capabilities id=5 caps=3' in log),
        ('sync round trip (wl_callback.done)',
         'ok   sync round trip completed (wl_callback.done)' in log),
        ('create_pool received the memfd over SCM_RIGHTS',
         bool(pool) and pool.group(1) == '7'),
        # the memfd object is sized in whole 4 KiB pages, so the compositor sees
        # 258048 bytes for a 256000-byte pool: the request is what must match.
        ('create_pool saw a sized memfd',
         bool(pool) and pool.group(3) == str(SURF_BYTES) and
         pool.group(4) is not None and
         int(pool.group(4)) >= SURF_BYTES and
         int(pool.group(4)) % 4096 == 0),
        ('create_buffer 320x200 stride=1280',
         'create_buffer id=8 pool=7 offset=0 320x200 stride=1280 format=1' in log),
        ('create_surface', '[wl] create_surface id=9' in log),
        ('attach buffer to surface', '[wl] attach surface=9 buffer=8 x=0 y=0' in log),
        ('commit read the pixels out of the pool',
         bool(commit) and commit.group(1) == '9' and commit.group(2) == '8' and
         commit.group(3) == str(SURF_W) and commit.group(4) == str(SURF_H) and
         commit.group(5) == str(SURF_STRIDE) and
         commit.group(7) == str(SURF_BYTES)),
        ('wl_buffer.release emitted by the server',
         '[wl] buffer.release id=8' in log),
        ('wl_buffer.release received by the client',
         'ok   wl_buffer.release received after commit' in log),
        ('client reported RESULT PASS', '[wlclient] RESULT PASS' in log),
        ('client wrote ' + str(SURF_BYTES) + ' bytes of pixels to WL.BIN',
         'ok   the pixel bytes reached /mnt/fat/WL.BIN through the VFS' in log),
        ('kernel self-tests still 16/16', '[stitch] selftests: 16/16 passed' in log),
        ('no kernel PANIC',
         'PANIC' not in log.upper().replace('NO PANIC', '')),
        ('HOST: WL.BIN byte-exact + hash parsed from the image', host_ok),
        ('HOST: WL.BIN FNV-1a equals the kernel commit hash',
         host_ok and kernel_hash is not None and host_hash == kernel_hash),
        ('HOST: WL.BIN FNV-1a equals the client-reported hash',
         host_ok and bool(client_hash) and host_hash == int(client_hash.group(1), 16)),
    ]

    print('[wl] client hash: %s' % (client_hash.group(1) if client_hash else 'MISSING'))
    print('[wl] kernel commit hash: %s' % ('%08x' % kernel_hash if kernel_hash
                                           is not None else 'MISSING'))
    print('[wl] host WL.BIN hash: %s' % ('%08x' % host_hash if host_hash is not None
                                         else 'MISSING'))
    print('[wl] host check: %s' % host_detail)

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[wl] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[wl] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
