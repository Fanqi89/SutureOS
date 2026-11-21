#!/usr/bin/env python3
"""SutureOS: Wayland server completion acceptance (multi-client batch).

What this proves, in one real QEMU boot, from ring 3 and from the host:

  * MULTI-CLIENT: `run wltwo` opens TWO connections to "wayland-0" at the same
    time, each commits its own surface (160x100 scale 2 / 96x64 scale 1), and
    the server keeps BOTH: the `[wl] kept surfaces:` line lists two entries with
    two different hashes, and each connection's re-commit only replaces its own
    entry.  wl_server_latest_surface() still means "most recent commit".
  * wl_keyboard is a real event stream: the acceptance types on the PS/2
    keyboard while wltwo waits; the server turns the decoded scancodes into
    wl_keyboard.key(evdev, state) events with paired press/release, after
    keymap (NO_KEYMAP) / enter / modifiers.
  * wl_pointer is a real event stream: monitor mouse moves produce
    wl_pointer.motion, `mouse_button 1` / `mouse_button 0` produce press *and*
    release, and the client's wl_pointer.release produces wl_pointer.leave.
    wl_pointer.frame is emitted because wltwo binds wl_seat at version 7.
  * wl_surface.set_buffer_scale(2) is recorded (logical 80x50 for a 160x100
    buffer) and reported on commit.
  * wl_region: add+add+subtract gives the expected area (9600 -> 9500) and is
    attached as opaque/input region, visible on the commit line.
  * wl_subcompositor/wl_subsurface: get_subsurface + set_position(24,16) is
    recorded and reported on commit.

Host-side verification never trusts the guest's own numbers:
  * the same two patterns are recomputed in Python and their FNV-1a hashes are
    compared with the server's `[wl] commit ... hash=` lines (frame 1 *and*
    frame 2 of surface A, frame 1 of surface B);
  * /mnt/fat/WLTWO.BIN and /mnt/fat/WLTWO2.BIN are parsed straight out of
    build/fat32_ata.img with scripts/verify_fat32_write and compared
    byte-for-byte with those recomputed patterns.

Timing: the keyboard typing uses a slow per-key delay so the characters land
while wltwo is running; the monitor mouse commands run after the keys, and the
boot waits afterwards, so wltwo's bounded input window (90 s) collects both.

  py -3 scripts/build.py && py -3 scripts/build_iso.py
  py -3 scripts/wl_multi_acceptance.py
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_wlmulti.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_wlmulti.args')
IMG = os.path.join(ROOT, 'build', 'fat32_ata.img')

# Surface A: 160x100, scale 2 (logical 80x50).  Surface B: 96x64, scale 1.
A_W, A_H, A_STRIDE = 160, 100, 160 * 4
A_BYTES = A_STRIDE * A_H
B_W, B_H, B_STRIDE = 96, 64, 96 * 4
B_BYTES = B_STRIDE * B_H
A_SCALE, B_SCALE = 2, 1

DMG_X, DMG_Y, DMG_W, DMG_H = 16, 16, 32, 32
DMG_COLOR = 0xFF2040C0

# Region A: add(0,0 160x50) + add(0,60 40x40) - subtract(10,10 10x10)
REGION_AREA = (160 * 50) + (40 * 40) - (10 * 10)
SUB_POS = (24, 16)

KEYS = ';'.join(['r', 'u', 'n', 'spc'] + list('wltwo') + ['ret'] +
                list('asdfghjk'))
MONITOR_CMDS = ('mouse_move 40 30;mouse_move 40 30;mouse_move 40 30;'\
                'mouse_button 1;mouse_button 0;mouse_button 1;mouse_button 0')


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def pattern_a(frame2=False):
    """Surface A: what user/apps/wltwo.c fill_a() writes (frame 1), and the same
    with the 32x32 marker block (frame 2)."""
    out = bytearray(A_BYTES)
    for y in range(A_H):
        row = y * A_STRIDE
        gy = (y * 5) & 0xFF
        for x in range(A_W):
            gx = (x * 3) & 0xFF
            c = 0xFF000000 | (gx << 16) | (gy << 8) | 0x11
            out[row + x * 4:row + x * 4 + 4] = c.to_bytes(4, 'little')
    if frame2:
        colour = DMG_COLOR.to_bytes(4, 'little')
        for y in range(DMG_Y, DMG_Y + DMG_H):
            row = y * A_STRIDE
            for x in range(DMG_X, DMG_X + DMG_W):
                out[row + x * 4:row + x * 4 + 4] = colour
    return bytes(out)


def pattern_b():
    """Surface B: what wltwo.c fill_b() writes."""
    out = bytearray(B_BYTES)
    for y in range(B_H):
        row = y * B_STRIDE
        gy = (y * 4) & 0xFF
        for x in range(B_W):
            gx = (x * 4) & 0xFF
            if ((x >> 3) + (y >> 3)) & 1:
                c = 0xFF000000 | (gx << 16) | 0x80 | (gy << 8)
            else:
                c = 0xFF000000 | (gy << 16) | (gx << 8) | 0x20
            out[row + x * 4:row + x * 4 + 4] = c.to_bytes(4, 'little')
    return bytes(out)


def regenerate_fixture():
    r = subprocess.run(['py', '-3', os.path.join('scripts', 'mkfat32_testimg.py')],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print('[wlmulti] fixture generation failed:')
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
                    '-SerialLog', 'build/serial_wlmulti.log',
                    '-ArgFile', 'build/qemu_wlmulti.args',
                    '-BootWaitSeconds', '150', '-MonitorPort', '45831',
                    '-Keys', KEYS, '-KeyDelayMs', '1200',
                    '-AfterKeysSeconds', '30',
                    '-MonitorCmds', MONITOR_CMDS,
                    '-AfterMonitorSeconds', '60'],
                   cwd=ROOT, check=False)
    return open(LOG, encoding='utf-8', errors='replace').read()


def host_files():
    """Parse the two exported frames straight out of the FAT32 image."""
    out = {}
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import verify_fat32_write as v
    img = open(IMG, 'rb').read()
    fs = v.parse(img)
    for name in ('WLTWO.BIN', 'WLTWO2.BIN'):
        if name not in fs['entries']:
            out[name] = None
            continue
        first, size = fs['entries'][name]
        out[name] = fs['read_chain'](first, size)[:size]
    return out


def main():
    if not os.path.exists(os.path.join(ROOT, 'build', 'sutureos.iso')):
        print('[wlmulti] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2
    if not regenerate_fixture():
        print('[wlmulti] RESULT: FAIL (no fixture)')
        return 1
    log = run_qemu()
    if '[wltwo] second ring-3 Wayland client' not in log:
        print('[wlmulti] the typed command never reached the shell: the boot was '
              'slower than BootWaitSeconds (QEMU TCG under load). Re-run.')

    f1 = pattern_a(False)
    f2 = pattern_a(True)
    fb = pattern_b()
    h1, h2, hb = fnv1a(f1), fnv1a(f2), fnv1a(fb)

    commits = re.findall(r'\[wl\] commit surface=(\d+) buffer=(\d+) (\d+)x(\d+) '
                         r'stride=(\d+) hash=([0-9a-f]{8})', log)
    kept_lines = re.findall(r'\[wl\] kept surfaces:.*', log)
    last_kept = kept_lines[-1] if kept_lines else ''
    kept = re.findall(r'\[(\d+)\] conn=(\d+) surface=(\d+) (\d+)x(\d+) stride=(\d+) '
                      r'scale=(\d+) serial=(\d+) hash=([0-9a-f]{8})', last_kept)

    kb_keys = re.findall(r'\[wl\] keyboard key id=(\d+) serial=\d+ time=\d+ '
                         r'key=(\d+) \(sc=0x([0-9a-f]+)\) state=(\d+)', log)
    kb_press = [k for k in kb_keys if k[3] == '1']
    kb_release = [k for k in kb_keys if k[3] == '0']
    kb_codes = {k[1] for k in kb_press}

    motions = re.findall(r'\[wl\] pointer motion id=(\d+) time=\d+ (-?\d+),(-?\d+)',
                         log)
    p_enter = re.search(r'\[wl\] pointer enter id=(\d+) surface=(\d+) (-?\d+),(-?\d+)',
                        log)
    p_press = re.findall(r'\[wl\] pointer button id=\d+ button=0x([0-9a-f]+) state=1',
                         log)
    p_release = re.findall(r'\[wl\] pointer button id=\d+ button=0x([0-9a-f]+) state=0',
                           log)
    p_frames = len(re.findall(r'\[wl\] pointer frame id=\d+', log))
    p_leave = re.search(r'\[wl\] pointer leave id=(\d+) surface=(\d+)', log)

    kb_enter = re.search(r'\[wl\] keyboard enter id=(\d+) surface=(\d+) serial=(\d+)',
                         log)
    kb_mods = len(re.findall(r'\[wl\] keyboard modifiers id=\d+', log))
    scale_set = re.search(r'\[wl\] surface\.set_buffer_scale surface=(\d+) scale=(\d+)',
                          log)
    state_lines = re.findall(r'\[wl\] surface state surface=(\d+) scale=(\d+) '
                             r'logical=(\d+)x(\d+) opaque_area=(\d+) input_area=(\d+) '
                             r'subsurface_id=(\d+)', log)
    region_adds = re.findall(r'\[wl\] region id=(\d+) add (-?\d+),(-?\d+) (\d+)x(\d+) '
                             r'-> rects=(\d+) subs=(\d+) area=(\d+)', log)
    region_subs = re.findall(r'\[wl\] region id=(\d+) subtract (-?\d+),(-?\d+) '
                             r'(\d+)x(\d+) -> rects=(\d+) subs=(\d+) area=(\d+)', log)
    get_sub = re.search(r'\[wl\] subcompositor\.get_subsurface id=(\d+) surface=(\d+) '
                        r'parent=(\d+) pos=0,0 sync=1', log)
    set_pos = re.search(r'\[wl\] subsurface\.set_position id=(\d+) surface=(\d+) '
                        r'parent=(\d+) pos=(-?\d+),(-?\d+)', log)
    sub_state = re.search(r'\[wl\] subsurface state id=(\d+) surface=(\d+) '
                          r'parent=(\d+) pos=(-?\d+),(-?\d+) sync=(\d+) below=(\d+)',
                          log)
    multi_conns = len(re.findall(r'\[wl\] client connected name="wayland-0"', log))
    ready = re.search(r'\[wl\] minimal Wayland server ready.*?(\d+) globals', log)
    client_pass = '[wltwo] RESULT PASS' in log
    maplap = re.search(r'\[wltwo\]\s+mmap windows: A=0x([0-9a-f]+)\+\d+ '
                       r'B=0x([0-9a-f]+)\+\d+ disjoint=(\d) a_intact=(\d)', log)
    maplap_ok = ('[wltwo]   ok   the two MAP_SHARED windows do not overlap' in log
                 and '[wltwo]   ok   mapping the second pool did not clobber the '
                     'first pool\'s bytes' in log)
    client_summary = re.search(r'\[wltwo\] input: keyboard press=(\d+) release=(\d+) '
                               r'keymap=(\d+) enter=(\d+) modifiers=(\d+) \| pointer '
                               r'enter=(\d+) motion=(\d+) button=(\d+)/(\d+) '
                               r'frame=(\d+) leave=(\d+)', log)
    client_frames = re.search(r'\[wltwo\] frames: A (\d+)x(\d+) scale=(\d+) '
                              r'hash=([0-9a-f]{8}).*?B (\d+)x(\d+) scale=(\d+) '
                              r'hash=([0-9a-f]{8})', log)

    files = {}
    host_detail = ''
    try:
        files = host_files()
        for name, size, expect in (('WLTWO.BIN', A_BYTES, f2),
                                   ('WLTWO2.BIN', B_BYTES, fb)):
            got = files.get(name)
            if got is None:
                host_detail += '%s missing; ' % name
            elif len(got) != size:
                host_detail += '%s is %d bytes, expected %d; ' % (name, len(got),
                                                                 size)
            elif got != expect:
                bad = next(i for i in range(size) if got[i] != expect[i])
                host_detail += '%s differs at byte %d; ' % (name, bad)
            else:
                host_detail += '%s byte-exact (%d B, FNV %08x); ' % (
                    name, size, fnv1a(got))
    except Exception as exc:                     # noqa: BLE001 - report, never hide
        host_detail = 'host FAT32 parse failed: %r' % (exc,)

    a_commits = [c for c in commits if c[2] == str(A_W) and c[3] == str(A_H)
                 and c[4] == str(A_STRIDE)]
    b_commits = [c for c in commits if c[2] == str(B_W) and c[3] == str(B_H)
                 and c[4] == str(B_STRIDE)]

    checks = [
        ('server ready, 4 globals advertised',
         ready is not None and ready.group(1) == '4',
         ready.group(0) if ready else 'missing ready line'),
        ('two concurrent client connections', multi_conns >= 2,
         '%d connect(s)' % multi_conns),
        ('commits for both clients (B once, A frame1/frame2/lifecycle)',
         len(commits) >= 3, '%d commit line(s)' % len(commits)),
        ('surface A committed 160x100 stride=640 (frame 1, 2 and lifecycle)',
         len(a_commits) >= 2, '%d A commit(s)' % len(a_commits)),
        ('surface B committed 96x64 stride=384',
         len(b_commits) == 1, '%d B commit(s)' % len(b_commits)),
        ('A commit #1 hash == host-recomputed frame 1',
         len(a_commits) >= 2 and int(a_commits[0][5], 16) == h1,
         a_commits[0][5] if a_commits else 'missing'),
        ('A commit #2 hash == host-recomputed frame 2 (pixels changed)',
         len(a_commits) >= 2 and int(a_commits[1][5], 16) == h2 and h2 != h1,
         a_commits[1][5] if len(a_commits) >= 2 else 'missing'),
        ('B commit hash == host-recomputed B pattern',
         len(b_commits) == 1 and int(b_commits[0][5], 16) == hb,
         b_commits[0][5] if b_commits else 'missing'),
        ('BOTH surfaces retained at once, with different hashes',
         len(kept) == 2 and kept[0][8] != kept[1][8] and
         {kept[0][8], kept[1][8]} == {('%08x' % h2), ('%08x' % hb)},
         last_kept if last_kept else 'no kept-surfaces line'),
        ('the two retained surfaces carry scale 2 and scale 1',
         len(kept) == 2 and {c[6] for c in kept} == {str(A_SCALE), str(B_SCALE)},
         ' '.join('%s' % (c[6],) for c in kept)),
        ('commit log records the connection slot per retained surface',
         len(kept) == 2 and len({c[1] for c in kept}) == 2,
         'conns %s' % ','.join(c[1] for c in kept)),
        ('wl_surface.set_buffer_scale(2) recorded',
         scale_set is not None and scale_set.group(2) == '2',
         scale_set.group(0) if scale_set else 'missing'),
        ('commit reports scale 2 and the logical size 80x50',
         any(s[1] == '2' and s[2] == '80' and s[3] == '50' for s in state_lines),
         '; '.join('%sx%s@%s' % (s[2], s[3], s[1]) for s in state_lines)),
        ('wl_region add+add gives area 9600 (two rects)',
         len(region_adds) >= 2 and region_adds[0][7] == '8000' and
         region_adds[1][7] == '9600',
         ' '.join('%sx%s->%s' % (r[3], r[4], r[7]) for r in region_adds)),
        ('wl_region subtract(10,10 10x10) leaves area 9500',
         len(region_subs) >= 1 and region_subs[0][7] == '9500',
         region_subs[0][0] if region_subs else 'no subtract line'),
        ('opaque/input region area 9500 reported at commit',
         any(s[4] == '9500' and s[5] == '9500' for s in state_lines),
         '; '.join('%s/%s' % (s[4], s[5]) for s in state_lines)),
        ('wl_subcompositor.get_subsurface linked child 9 to parent 12',
         get_sub is not None and get_sub.group(2) == '9' and get_sub.group(3) == '12',
         get_sub.group(0) if get_sub else 'missing'),
        ('wl_subsurface.set_position(24,16) recorded',
         set_pos is not None and (set_pos.group(4), set_pos.group(5)) == ('24', '16'),
         set_pos.group(0) if set_pos else 'missing'),
        ('commit reports the subsurface offset 24,16',
         sub_state is not None and
         (sub_state.group(4), sub_state.group(5)) == ('24', '16'),
         sub_state.group(0) if sub_state else 'missing'),
        ('wl_keyboard.keymap sent as XKB_V1 over SCM_RIGHTS',
         '[wltwo]   event keyboard.keymap format=1 size=' in log and
         '[wl] keyboard keymap id=' in log),
        ('wl_keyboard.enter for the committed surface',
         kb_enter is not None, kb_enter.group(0) if kb_enter else 'missing'),
        ('wl_keyboard.modifiers emitted', kb_mods >= 1, '%d line(s)' % kb_mods),
        ('real typing produced >= 6 key presses', len(kb_press) >= 6,
         '%d press(es)' % len(kb_press)),
        ('every press has a matching release', len(kb_release) >= len(kb_press),
         '%d release(s)' % len(kb_release)),
        ('>= 3 distinct evdev keycodes seen', len(kb_codes) >= 3,
         'codes %s' % ','.join(sorted(kb_codes))),
        ('keycodes are evdev (scancode + 8)',
         all(int(k[1]) == (int(k[2], 16) & 0x7F) + 8 for k in kb_keys),
         kb_keys[0][1] if kb_keys else 'none'),
        ('wl_pointer.enter on the committed surface', p_enter is not None,
         p_enter.group(0) if p_enter else 'missing'),
        ('>= 3 wl_pointer.motion events streamed', len(motions) >= 3,
         '%d motion(s)' % len(motions)),
        ('wl_pointer.button press delivered', len(p_press) >= 1,
         'press %s' % ','.join(p_press)),
        ('wl_pointer.button release delivered', len(p_release) >= 1,
         'release %s' % ','.join(p_release)),
        ('wl_pointer.frame groups emitted (pointer bound at v7)', p_frames >= 1,
         '%d frame(s)' % p_frames),
        ('wl_pointer.leave after the client released the pointer',
         p_leave is not None, p_leave.group(0) if p_leave else 'missing'),
        ('client reported RESULT PASS', client_pass),
        ('client counters agree with the server log',
         client_summary is not None and
         int(client_summary.group(1)) == len(kb_press) and
         int(client_summary.group(2)) == len(kb_release) and
         int(client_summary.group(7)) == len(motions),
         client_summary.group(0) if client_summary else 'missing'),
        ('client frame hashes match the host patterns',
         client_frames is not None and
         client_frames.group(4) == '%08x' % h2 and
         client_frames.group(8) == '%08x' % hb,
         client_frames.group(0) if client_frames else 'missing'),
        ('mmap: the acceptance client mapped two pools without overlap',
         maplap is not None and maplap.group(3) == '1',
         maplap.group(0) if maplap else 'missing'),
        ('mmap: the second mapping did not clobber the first (data re-hashed)',
         maplap is not None and maplap.group(4) == '1' and maplap_ok,
         maplap.group(0) if maplap else 'missing'),
        ('HOST: WLTWO.BIN/WLTWO2.BIN byte-exact against the recomputed patterns',
         all(files.get(n) == e for n, e in (('WLTWO.BIN', f2),
                                            ('WLTWO2.BIN', fb))),
         host_detail),
        ('kernel self-tests still 16/16',
         '[stitch] selftests: 16/16 passed' in log),
        ('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')),
    ]

    print('[wlmulti] host frame-1 A hash %08x, frame-2 A hash %08x, B hash %08x'
          % (h1, h2, hb))
    print('[wlmulti] server commit hashes: %s'
          % ', '.join('%s(%sx%s)' % (c[5], c[2], c[3]) for c in commits))
    print('[wlmulti] host check: %s' % host_detail.strip())

    failed = [c for c in checks if not c[1]]
    for name, cond, *rest in checks:
        detail = ('  (%s)' % rest[0]) if rest and rest[0] else ''
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, detail))
    print('[wlmulti] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[wlmulti] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
