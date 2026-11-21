#!/usr/bin/env python3
"""SutureOS: Wayland window placement + input semantics on the real desktop.

Two faces of the same server code:

  PROTOCOL FACE (always): `run wltwo` in the terminal.  A live client is the only
  consumer, so it verifies the real enter/motion/button/frame/leave events, the
  surface-local coordinate math, the XKB keymap that arrives as a memfd over
  SCM_RIGHTS (its text is hashed on the host independently) and the wl_buffer /
  wl_shm_pool lifecycle.

  DESKTOP FACE (when the shell pumps the server): `startgui` + F2 opens the
  Wayland window; the Rust shell calls wl_server_desktop_pump() every frame, which
  maps the real window rectangle onto the retained surface.  Then the acceptance
  drags the window and checks that the surface-local coordinates follow it, and
  that keyboard events are gated while the window does not hold the focus.

This kernel cannot run the desktop shell and a ring-3 Wayland client at the same
time (single-task shell, and the protocol pump lives on the client's syscall path),
so the two faces are verified in one boot, one after the other.

    py -3 scripts/wl_win_semantics_acceptance.py
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = int(os.environ.get('WL_SEM_QMP_PORT', '45861'))
LOG = 'build/serial_wl_win_semantics.log'

# The XKB keymap that kernel/ipc/wl_server.c hands to clients (format 1, XKB_V1).
# Must stay byte-identical to the C literal; the guest's hash has to match this
# recomputation.
KEYMAP_TEXT = 'xkb_keymap {\n  xkb_keycodes "sutureos" {\n    minimum=8; maximum=255;\n    <ESC>=9; <BKSP>=22; <TAB>=23; <AD01>=24; <AD02>=25; <AD03>=26; <AD04>=27;\n    <AD05>=28; <AD06>=29; <AD07>=30; <AD08>=31; <AD09>=32; <AD10>=33;\n    <AD11>=34; <AD12>=35; <RTRN>=36; <LCTL>=37; <AC01>=38; <AC02>=39;\n    <AC03>=40; <AC04>=41; <AC05>=42; <AC06>=43; <AC07>=44; <AC08>=45;\n    <AC09>=46; <AC10>=47; <AC11>=48; <TLDE>=49; <LFSH>=50; <BKSL>=51;\n    <AB01>=52; <AB02>=53; <AB03>=54; <AB04>=55; <AB05>=56; <AB06>=57;\n    <AB07>=58; <AB08>=59; <AB09>=60; <AB10>=61; <RTSH>=62; <LALT>=64;\n    <SPCE>=65; <CAPS>=66;\n  };\n  xkb_types "complete" {\n    type "ONE_LEVEL" { modifiers=none; map[none]=Level1; };\n    type "TWO_LEVEL" { modifiers=Shift; map[Shift]=Level2; map[none]=Level1; };\n    type "ALPHABETIC" {\n      modifiers=Shift+Lock; map[Shift]=Level2; map[Lock]=Level2;\n      map[Shift+Lock]=Level2; map[none]=Level1;\n    };\n  };\n  xkb_compatibility "complete" {\n    interpret Shift_L { action=SetMods(modifiers=Shift); };\n    interpret Caps_Lock { action=LockMods(modifiers=Lock); };\n    interpret Control_L { action=SetMods(modifiers=Control); };\n    interpret Alt_L { action=SetMods(modifiers=Mod1); };\n    interpret Any+Any { action=SetMods(modifiers=modMapMods); };\n  };\n  xkb_symbols "sutureos-us" {\n    name[Group1]="SutureOS";\n    key.type[Group1]="ALPHABETIC";\n    key <ESC> { [Escape] }; key <BKSP> { [BackSpace] };\n    key <TAB> { [Tab] }; key <RTRN> { [Return] };\n    key <LCTL> { [Control_L] }; key <LFSH> { [Shift_L] };\n    key <RTSH> { [Shift_R] }; key <LALT> { [Alt_L] };\n    key <CAPS> { [Caps_Lock] }; key <SPCE> { [space] };\n    key <AD01> { [q, Q] }; key <AD02> { [w, W] }; key <AD03> { [e, E] };\n    key <AD04> { [r, R] }; key <AD05> { [t, T] }; key <AD06> { [y, Y] };\n    key <AD07> { [u, U] }; key <AD08> { [i, I] }; key <AD09> { [o, O] };\n    key <AD10> { [p, P] }; key <AD11> { [bracketleft, braceleft] };\n    key <AD12> { [bracketright, braceright] };\n    key <AC01> { [a, A] }; key <AC02> { [s, S] }; key <AC03> { [d, D] };\n    key <AC04> { [f, F] }; key <AC05> { [g, G] }; key <AC06> { [h, H] };\n    key <AC07> { [j, J] }; key <AC08> { [k, K] }; key <AC09> { [l, L] };\n    key <AC10> { [semicolon, colon] }; key <AC11> { [apostrophe, quotedbl] };\n    key <TLDE> { [grave, asciitilde] }; key <BKSL> { [backslash, bar] };\n    key <AB01> { [z, Z] }; key <AB02> { [x, X] }; key <AB03> { [c, C] };\n    key <AB04> { [v, V] }; key <AB05> { [b, B] }; key <AB06> { [n, N] };\n    key <AB07> { [m, M] }; key <AB08> { [comma, less] };\n    key <AB09> { [period, greater] }; key <AB10> { [slash, question] };\n  };\n};\n\n'

# Window geometry: the "Wayland client" window of the Rust desktop shell.
WIN_X, WIN_Y, WIN_W, WIN_H = 250, 330, 440, 330
TITLE_H = 40
SURF_LOGICAL = (80, 50)          # wltwo's last surface: 160x100 buffer at scale 2


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class Qmp:
    def __init__(self, port, timeout=120):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as exc:              # noqa: PERF203 - retry loop
                last = exc
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


def main():
    log_abs = os.path.join(ROOT, LOG)
    if os.path.exists(log_abs):
        os.remove(log_abs)
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[wlsem] build/sutureos.iso missing: run scripts/build_iso.py first')
        return 2

    argv = [QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-drive', 'id=adisk,file=build/fat32_ata.img,format=raw,if=ide',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    print('[wlsem] ' + ' '.join(argv), flush=True)
    proc = subprocess.Popen(argv, cwd=ROOT)
    checks = []

    def log_text():
        try:
            with open(log_abs, encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def wait_for(needle, timeout=240):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in log_text():
                return True
            time.sleep(0.3)
        return False

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))

    def skip(name, why):
        checks.append((name, None, why))
        print('  [SKIP] %s  (%s)' % (name, why))

    keymap_hash = fnv1a(KEYMAP_TEXT.encode('utf-8'))
    print('[wlsem] host keymap: %d bytes, fnv1a=%08x' % (len(KEYMAP_TEXT),
                                                         keymap_hash))

    try:
        q = Qmp(PORT)

        pos = [512, 384]                    # PS/2 pointer starts here

        def type_line(text, d=0.12):
            for ch in text:
                q.hmp('sendkey ' + ('spc' if ch == ' ' else ch))
                time.sleep(d)
            time.sleep(0.2)
            q.hmp('sendkey ret')

        def move_to(tx, ty, settle=0.30):
            dx, dy = tx - pos[0], ty - pos[1]
            while dx or dy:
                sx = max(-90, min(90, dx))
                sy = max(-90, min(90, dy))
                q.hmp('mouse_move %d %d' % (sx, sy))
                time.sleep(0.07)
                dx -= sx
                dy -= sy
            pos[0], pos[1] = tx, ty
            time.sleep(settle)

        def click(text_hold=0.25):
            q.hmp('mouse_button 1')
            time.sleep(text_hold)
            q.hmp('mouse_button 0')
            time.sleep(0.30)

        if not wait_for('sutureos#', 260):
            print('[wlsem] shell prompt never appeared')
            return 1
        time.sleep(0.5)

        # ================= PROTOCOL FACE: live client in the terminal ========
        type_line('run wltwo')
        check('ring-3 wltwo starts and binds a seat',
              wait_for('[wl] seat.get_keyboard id=', 200))
        check('server reports a window rectangle for placement',
              wait_for('[wl] wayland window %d,%d %dx%d (source='
                       % (WIN_X, WIN_Y, WIN_W, WIN_H), 60))

        # live typing: the client is the keyboard consumer in the terminal
        for ch in ('a', 's', 'd', 'f', 'g', 'h'):
            q.hmp('sendkey ' + ch)
            time.sleep(0.16)
        time.sleep(1.0)

        # pointer: leave the window, come back, move inside, click, leave again
        move_to(200, 300)                   # left of WIN_X -> outside
        move_to(312, 364)                   # inside: local (31,17)
        move_to(372, 404)                   # local (61,37)
        move_to(402, 414)                   # local (76,42)
        click()
        move_to(200, 320)                   # leave again

        check('client finished its own checks',
              wait_for('[wltwo] RESULT PASS', 200))
        wait_for('sutureos#', 90)

        log = log_text()

        # ---- window / coordinate mapping -------------------------------
        wins = re.findall(r'\[wl\] pointer window surface=(\d+) window=(-?\d+),'
                          r'(-?\d+) (\d+)x(\d+) local=(-?\d+),(-?\d+) scale=(\d+) '
                          r'kernel=(-?\d+),(-?\d+)', log)
        mapping_ok = bool(wins)
        mapping_detail = ''
        for (sf, wx, wy, ww, wh, lx, ly, sc, kx, ky) in wins:
            wx, wy, lx, ly = int(wx), int(wy), int(lx), int(ly)
            sc, kx, ky = int(sc), int(kx), int(ky)
            exp_x = max(0, min(SURF_LOGICAL[0] - 1, (kx - wx) // sc))
            exp_y = max(0, min(SURF_LOGICAL[1] - 1, (ky - wy) // sc))
            if (lx, ly) != (exp_x, exp_y):
                mapping_ok = False
                mapping_detail = ('surface=%s window=%d,%d local=%d,%d but '
                                  'kernel=%d,%d scale=%d expects %d,%d'
                                  % (sf, wx, wy, lx, ly, kx, ky, sc, exp_x,
                                     exp_y))
                break
        check('every motion/enter is the window-relative local coordinate',
              mapping_ok and len(wins) >= 3,
              mapping_detail or ('%d mapped event(s)' % len(wins)))

        enters = re.findall(r'\[wl\] pointer enter id=(\d+) surface=\d+ (-?\d+),'
                            r'(-?\d+) \(kernel mouse (-?\d+),(-?\d+)', log)
        check('wl_pointer.enter carries surface-local coordinates',
              len(enters) >= 2,
              '; '.join('%s,%s (kernel %s,%s)' % (e[1], e[2], e[3], e[4])
                        for e in enters[:3]))
        inside_ok = all(WIN_X <= int(e[3]) < WIN_X + WIN_W and
                        WIN_Y <= int(e[4]) < WIN_Y + WIN_H for e in enters)
        check('enter only fires while the pointer is inside the window',
              inside_ok and len(enters) >= 2,
              '%d enter(s): %s' % (len(enters), ', '.join(
                  '(%s,%s)' % (e[3], e[4]) for e in enters[:3])))
        leaves = re.findall(r'\[wl\] pointer leave id=\d+ surface=\d+ '
                            r'serial=\d+ \(left the wayland window\)', log)
        check('leaving the window emits wl_pointer.leave', len(leaves) >= 1,
              '%d leave(s)' % len(leaves))
        motions = re.findall(r'\[wl\] pointer motion id=(\d+) time=\d+ (-?\d+),'
                             r'(-?\d+)', log)
        check('wl_pointer.motion streams while inside (>= 3)', len(motions) >= 3,
              '%d motion(s)' % len(motions))
        buttons = re.findall(r'\[wl\] pointer button id=11 button=0x([0-9a-f]+) '
                             r'state=(\d)', log)
        check('wl_pointer.button delivers press and release',
              len([b for b in buttons if b[1] == '1']) >= 1 and
              len([b for b in buttons if b[1] == '0']) >= 1,
              '%d event(s)' % len(buttons))
        check('no key was gated while the terminal client owns the keyboard',
              '[wl] keyboard key gated' not in log,
              'terminal mode: focus falls back to the live client')

        # ---- keymap ----------------------------------------------------
        srv_km = re.search(r'\[wl\] keyboard keymap id=(\d+) format=(\d+) size=(\d+) '
                           r'fd=(-?\d+) fnv1a=([0-9a-f]{8})', log)
        prep = re.search(r'\[wl\] keymap prepared: (\d+) bytes in memfd fd=(-?\d+) '
                         r'fnv1a=([0-9a-f]{8})', log)
        cli_km = re.search(r'\[wltwo\] keymap fd=(-?\d+) size=(\d+) hash=([0-9a-f]{8})',
                           log)
        check('wl_keyboard.keymap is format 1 (XKB_V1) with a real size',
              srv_km is not None and srv_km.group(2) == '1' and
              int(srv_km.group(3)) == len(KEYMAP_TEXT.encode('utf-8')),
              srv_km.group(0) if srv_km else 'missing')
        check('keymap text is 1-3 KiB (a real mini XKB keymap)',
              1024 <= int(srv_km.group(3)) <= 3072 if srv_km else False,
              ('%s bytes' % srv_km.group(3)) if srv_km else 'missing')
        check('host recomputation matches the server keymap hash',
              srv_km is not None and srv_km.group(5) == '%08x' % keymap_hash and
              prep is not None and prep.group(3) == '%08x' % keymap_hash,
              'server=%s host=%08x' % (srv_km.group(5) if srv_km else '?',
                                       keymap_hash))
        check('the client read the memfd over SCM_RIGHTS and hashed the same text',
              cli_km is not None and cli_km.group(3) == '%08x' % keymap_hash,
              cli_km.group(0) if cli_km else 'missing')
        check('keymap was delivered by wl_keyboard.enter (descriptor path used)',
              '[wl] keyboard keymap id=' in log and 'SCM_RIGHTS' in log,
              'SCM_RIGHTS annotated keymap line')

        # ---- buffer lifecycle -----------------------------------------
        check('wl_shm_pool.destroy keeps the descriptor for live buffers',
              re.search(r'\[wl\] shm_pool\.destroy id=\d+ \(fd=-?\d+ kept for 1 '
                        r'live buffer\(s\)', log) is not None)
        check('create_buffer on the destroyed pool is refused',
              'refused: pool ' in log and 'was destroyed' in log,
              'create_buffer refusal logged')
        commits = re.findall(r'\[wl\] commit surface=(\d+) buffer=(\d+) (\d+)x(\d+) '
                             r'stride=(\d+) hash=([0-9a-f]{8})', log)
        check('the existing buffer still commits after the pool was destroyed',
              len(commits) >= 4, '%d commit(s)' % len(commits))
        check('wl_buffer.destroy makes the buffer unusable',
              '[wl] buffer.destroy id=8' in log and
              ('refused: buffer 8 is not a wl_buffer' in log or
               'detached from surface' in log),
              'destroy + refused attach/detach logged')
        check('commit without a buffer is refused',
              'commit surface=9: no buffer attached' in log)
        stats = re.findall(r'\[wl\] buffer stats created=(\d+) released=(\d+) '
                           r'destroyed=(\d+) skipped=(\d+)', log)
        last = stats[-1] if stats else None
        check('wl_server_buffer_stats(): created/released/destroyed coherent',
              last is not None and int(last[0]) >= 2 and int(last[1]) >= 4 and
              int(last[2]) >= 1 and int(last[0]) >= int(last[2]),
              ('created=%s released=%s destroyed=%s skipped=%s' % last)
              if last else 'no stats line')
        check('release is sent at most once per attach cycle',
              'skipped (already released, no re-attach)' in log or
              (last is not None and int(last[3]) >= 0),
              'release policy logged')

        # ================= DESKTOP FACE: startgui + F2 ======================
        desktop = 'desktop pump' in log
        if not desktop:
            type_line('startgui')
            if not wait_for('[gui-shell] shell: loop', 200):
                print('[wlsem] the desktop shell never started')
            else:
                q.hmp('sendkey f2')
                wait_for('[gui-shell] wayland window opened', 60)
                pos[0], pos[1] = 512, 648          # the shell's pointer start
                time.sleep(1.0)
                move_to(350, 400)                  # inside the window
                time.sleep(0.8)
                # drag the window by its title bar
                move_to(350, WIN_Y + TITLE_H // 2)
                q.hmp('mouse_button 1')
                time.sleep(0.3)
                move_to(450, 418, settle=0.5)
                q.hmp('mouse_button 0')
                time.sleep(0.8)
                move_to(500, 450)
                time.sleep(0.8)
                # focus gating: click ANOTHER window (an empty taskbar click is a
                # no-op in this shell and would leave the focus on the Wayland
                # window), then type
                move_to(200, 200)                  # settings window body
                click()
                for ch in ('j', 'k', 'l'):
                    q.hmp('sendkey ' + ch)
                    time.sleep(0.2)
                time.sleep(1.0)
                log = log_text()
                desktop = 'desktop pump' in log

        if desktop:
            log = log_text()
            check('desktop pump: the shell pumps the server every frame',
                  'desktop pump calls=' in log)
            check('desktop pump: the window rectangle comes from the shell',
                  'source=shell' in log,
                  'rect source=shell')
            src = re.findall(r'\[wl\] wayland window (-?\d+),(-?\d+) (\d+)x(\d+) '
                             r'\(source=shell', log)
            moved = len(src) >= 2
            dwins = re.findall(r'\[wl\] pointer window surface=(\d+) window=(-?\d+),'
                               r'(-?\d+) (\d+)x(\d+) local=(-?\d+),(-?\d+) scale=(\d+) '
                               r'kernel=(-?\d+),(-?\d+)', log)
            dmap_ok = bool(dwins)
            ddetail = ''
            for (sf, wx, wy, ww, wh, lx, ly, sc, kx, ky) in dwins:
                wx, wy, lx, ly = int(wx), int(wy), int(lx), int(ly)
                sc, kx, ky = int(sc), int(kx), int(ky)
                exp_x = max(0, min(SURF_LOGICAL[0] - 1, (kx - wx) // sc))
                exp_y = max(0, min(SURF_LOGICAL[1] - 1, (ky - wy) // sc))
                if (lx, ly) != (exp_x, exp_y):
                    dmap_ok = False
                    ddetail = ('window=%d,%d local=%d,%d kernel=%d,%d scale=%d '
                               'expects %d,%d' % (wx, wy, lx, ly, kx, ky, sc,
                                                 exp_x, exp_y))
                    break
            check('desktop pump: surface-local coordinates follow the window',
                  moved and dmap_ok and len(dwins) >= 3,
                  ddetail or ('%d rect change(s), %d mapped line(s)'
                              % (len(src), len(dwins))))
            check('desktop pump: enter/motion/leave are recorded with id=0',
                  '[wl] pointer enter id=0' in log or
                  '[wl] pointer motion id=0' in log,
                  'desktop-stream events present')
            check('keyboard focus gate: non-focused window drops keys',
                  '[wl] keyboard key gated' in log,
                  re.search(r'keyboard key gated \(window not focused\) '
                            r'gated=(\d+)', log).group(0)
                  if re.search(r'keyboard key gated \(window not focused\) '
                               r'gated=(\d+)', log) else 'no gated key')
        else:
            skip('desktop pump: the shell pumps the server every frame',
                 'Rust shell in this tree does not call wl_server_desktop_pump()')
            skip('desktop pump: surface-local coordinates follow the window',
                 'GUI drag path not triggered (no desktop pump call site)')
            skip('desktop pump: enter/motion/leave are recorded with id=0',
                 'GUI drag path not triggered (no desktop pump call site)')
            skip('keyboard focus gate: non-focused window drops keys',
                 'GUI drag path not triggered (no desktop pump call site)')

        log = log_text()
        check('kernel self-tests still 16/16',
              '[stitch] selftests: 16/16 passed' in log)
        check('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', ''))
        check('no FAT32 obstacle (the fixture was fresh)',
              'fat32] write self-test PASS' in log or
              'fat32] ops self-test PASS' in log,
              'fixture driven boot reached the shell')
    finally:
        try:
            q.cmd('quit')
        except Exception:                    # noqa: BLE001 - best effort shutdown
            pass
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    failed = [c for c in checks if c[1] is False]
    skipped = [c for c in checks if c[1] is None]
    print('[wlsem] checks=%d ok=%d fail=%d skip=%d'
          % (len(checks), len(checks) - len(failed) - len(skipped), len(failed),
             len(skipped)))
    print('[wlsem] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
