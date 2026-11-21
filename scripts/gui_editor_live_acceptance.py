#!/usr/bin/env python3
"""SutureOS GUI editor acceptance (v0.61): type in the desktop editor, press F5,
and check the file that lands on the FAT32 volume byte for byte.

This is the first gate where GUI keyboard input changes real *data*:
  * `startgui` -> start menu -> Text Editor row -> the window opens with focus,
  * six keys go through the kernel PS/2 queue and the shell's key router into
    `apps::editor_key` (proof: `[gui-shell] editor key` lines),
  * `F5` saves the buffer through the kernel VFS (`vfs_write_file`) to
    /mnt/fat/GUIEDIT.TXT (proof: `[gui-shell] editor saved N bytes rc=0`),
  * the host parses the disk image it just booted and compares the bytes with the
    expected snapshot (never trusting the guest's own claim), and
  * a screendump shows the editor window still painted (the keystroke path also
    invalidated the window layer cache and injected damage).

Exit code 0 only when every check passes.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QMP_PORT = int(os.environ.get('EDITOR_QMP_PORT', '45871'))
SERIAL = 'build/serial_editor_live.log'
SHOT = 'build/acc_editor_live.ppm'
DISK = 'build/fat32_ata.img'

TYPED = 'hi 42'
BASE_LINES = ["fn main() {", "    let mut n = 42;", "    n += 1;", "}"]
CURSOR_LINE, CURSOR_COL = 1, len(BASE_LINES[1])


def expected_snapshot():
    lines = list(BASE_LINES)
    lines[CURSOR_LINE] = (lines[CURSOR_LINE][:CURSOR_COL] + TYPED
                          + lines[CURSOR_LINE][CURSOR_COL:])
    return ("\n".join(lines) + "\n").encode()


class Qmp:
    def __init__(self, port, timeout=120):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), 1.0)
                self.f = self.s.makefile('rwb', buffering=0)
                self._read()
                self.cmd('qmp_capabilities')
                return
            except Exception as e:      # noqa: BLE001
                last = e
                time.sleep(0.3)
        raise RuntimeError('QMP connect failed: %r' % (last,))

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            if b'"event"' in line:
                continue
            rep = json.loads(line)
            if 'error' in rep:
                raise RuntimeError('QMP error: %r' % (rep['error'],))
            return rep

    def cmd(self, name, **args):
        self.f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        return self._read()

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})


QCODE = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash',
         ',': 'comma', '=': 'equal', '\\n': 'ret'}


def press(q, keys, delay=0.14):
    for k in keys:
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': QCODE.get(k, k)}])
        time.sleep(delay)


def move_to(q, pos, tx, ty, settle=0.35):
    dx, dy = tx - pos[0], ty - pos[1]
    while dx or dy:
        sx = max(-90, min(90, dx))
        sy = max(-90, min(90, dy))
        q.hmp('mouse_move %d %d' % (sx, sy))
        dx -= sx
        dy -= sy
        time.sleep(0.06)
    pos[0], pos[1] = tx, ty
    time.sleep(settle)


def click(q, pos, tx, ty):
    move_to(q, pos, tx, ty)
    q.hmp('mouse_button 1')
    time.sleep(0.20)
    q.hmp('mouse_button 0')
    time.sleep(1.6)


def shot(q, path):
    if os.path.exists(path):
        os.remove(path)
    q.cmd('screendump', filename=path)
    for _ in range(80):
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            time.sleep(0.4)
            return
        time.sleep(0.1)
    raise RuntimeError('no screendump')


def boot():
    for p in (SERIAL, SHOT):
        if os.path.exists(p):
            os.remove(p)
    log = open('build/qemu_editor_live.log', 'wb')
    qemu = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-vga', 'std',
        '-serial', 'file:' + SERIAL,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % QMP_PORT,
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return qemu, log


def main():
    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''), flush=True)

    # A dirty volume makes the kernel's FAT32 self-test stall the boot, so the
    # fixture is regenerated here as well (the sweep does the same).
    subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')],
                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import shutil
    shutil.copyfile(os.path.join(ROOT, 'build', 'fat32.img'),
                    os.path.join(ROOT, DISK))

    qemu, log = boot()
    pos = [512, 648]                     # the desktop parks the pointer here
    try:
        q = Qmp(QMP_PORT)
        print('waiting for the shell prompt ...', flush=True)
        time.sleep(75)
        press(q, list('startgui') + ['ret'])
        time.sleep(16)
        # start menu -> 4th row (Text Editor): MENU_X+100, MENU_Y+46+3*44+19
        click(q, pos, 25, 745)           # start button
        click(q, pos, 112, 683)          # Text Editor row
        time.sleep(3)
        press(q, list(TYPED))
        press(q, ['f5'])
        time.sleep(6)
        shot(q, os.path.join(ROOT, SHOT))
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:               # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    want = expected_snapshot()
    check('the shell started the Rust desktop', '[gui-shell]' in text)
    check('the start menu launched the editor window',
          'menu launch 3' in text, 'menu launch 3')
    key_lines = text.count('[gui-shell] editor key')
    check('typed keys reached the editor buffer', key_lines >= len(TYPED),
          '%d key line(s)' % key_lines)
    m = None
    for line in text.splitlines():
        if 'editor saved' in line:
            m = line
    check('F5 saved the buffer through the kernel VFS', m is not None,
          m or 'no save line')
    saved_ok = False
    if m:
        try:
            parts = m.split()
            n = int(parts[parts.index('bytes') - 1])
            rc = int(m.split('rc=')[1])
            saved_ok = (n == len(want) and rc == 0)
            check('the save line reports the right size and rc', saved_ok,
                  'n=%d want=%d rc=%d' % (n, len(want), rc))
        except Exception as e:          # noqa: BLE001
            check('the save line reports the right size and rc', False, repr(e))
    check('no kernel PANIC', 'PANIC' not in text)

    # ---- host side: parse the image QEMU wrote and compare the bytes ----
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import verify_fat32_write as vf
    img = open(os.path.join(ROOT, DISK), 'rb').read()
    fs = vf.parse(img)
    ent = fs['entries'].get('GUIEDIT.TXT')
    check('GUIEDIT.TXT exists on the volume', ent is not None,
          'entries=%s' % ','.join(sorted(fs['entries'])[:8]))
    if ent:
        first, size = ent
        data = fs['read_chain'](first, size)
        check('file size matches the buffer', size == len(want),
              'size=%d want=%d' % (size, len(want)))
        check('file bytes are byte-exact', data == want, data[:60].decode('latin1'))
        print('    expected:', want)
        print('    on disk :', data)
    check('screendump captured', os.path.exists(os.path.join(ROOT, SHOT)))

    # ---- the editor window is above the task manager: the task manager's live
    # heartbeat dot (0x7cf0c8) used to be painted after every window layer and
    # therefore showed through the editor body at (858, 342). It must be gone now.
    try:
        from PIL import Image
        im = Image.open(os.path.join(ROOT, SHOT)).convert('RGB')
        px = im.load()
        cyan = 0
        for yy in range(330, 356):
            for xx in range(844, 872):
                r, g, b = px[xx, yy]
                if g > 190 and r < 170 and b > 150:
                    cyan += 1
        check('live heartbeat dot does not bleed through the window above it',
              cyan == 0, '%d cyan px' % cyan)
        # the typed text must be on screen: bright glyph pixels on the caret line
        bright = 0
        for yy in range(215, 232):
            for xx in range(600, 880):
                r, g, b = px[xx, yy]
                if r > 190 and g > 200 and b > 210:
                    bright += 1
        check('the typed line is visible in the editor window', bright > 40,
              '%d bright px' % bright)
    except Exception as e:              # noqa: BLE001
        check('screenshot analysis ran', False, repr(e))

    passed = sum(1 for _, ok, _ in checks if ok)
    print('[editorlive] checks=%d ok=%d fail=%d'
          % (len(checks), passed, len(checks) - passed))
    print('[editorlive] RESULT:', 'PASS' if passed == len(checks) else 'FAIL')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
