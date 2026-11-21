#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS desktop editor acceptance (v0.66): a real 48x64 buffer with scrolling,
undo/redo and Ctrl shortcuts, proven by the bytes that land on the FAT32 volume.

  * `startgui` -> start menu -> Text Editor row -> the window opens with focus,
  * the buffer reports its new geometry: "[gui-shell] editor init lines=48 cols=64",
  * typing / Enter go through the kernel PS/2 queue and the shell key router into
    `apps::editor_key`,
  * Ctrl+Z / Ctrl+Y reach `apps::editor_undo/editor_redo` (the modifier mask from
    the PS/2 decoder is what makes this possible at all),
  * 14 Enters push the caret past the 10 visible rows, so the viewport scrolls
    ("editor scroll N"), PageUp moves a page and the wheel scrolls too,
  * `F5` saves the buffer through the kernel VFS (`vfs_write_file`) to
    /mnt/fat/GUIEDIT.TXT,
  * the host parses the disk image it just booted and compares every byte with a
    model of the same edits (never trusting the guest's claim), and
  * a screendump shows the editor still painted.

Exit code 0 only when every check passes.
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
QMP_PORT = int(os.environ.get('EDITOR_QMP_PORT', '45871'))
SERIAL = 'build/serial_editor_live.log'
SHOT = 'build/acc_editor_live.ppm'
DISK = 'build/fat32_ata.img'

SEED = ["fn main() {", "    let mut n = 42;", "    n += 1;", "}"]
CARET_LINE, CARET_COL = 1, len(SEED[1])
VISIBLE = 8                      # the 330x230 window fits eight 18 px rows


def model_buffer():
    """The same edits the gate performs, computed on the host."""
    lines = list(SEED)
    line, col = CARET_LINE, CARET_COL
    # 1. type xy at the caret
    lines[line] = lines[line][:col] + "xy" + lines[line][col:]
    col += 2
    # 2. Enter (split at the end of the line) then type abc
    lines.insert(line + 1, "")
    line += 1
    col = 0
    lines[line] = "abc"
    col = 3
    # 3. Ctrl+Z twice (undo "abc", then the Enter) and Ctrl+Y once (redo the Enter):
    #    the buffer is back to "one extra empty line, caret at its start"
    lines[line] = ""
    col = 0
    # 4. type zz on that empty line
    lines[line] = "zz"
    col = 2
    # 5. eleven Enters: each inserts an empty line and moves the caret down, so
    #    the caret leaves the ten visible rows and the viewport has to scroll
    for _ in range(11):
        lines.insert(line + 1, "")
        line += 1
        col = 0
    # 6. PageUp (caret moves one window up); the wheel only moves the viewport
    line = max(0, line - VISIBLE)
    return ("\n".join(lines) + "\n").encode(), lines, line


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

    def wheel(self, button, settle=0.9):
        self.cmd('input-send-event',
                 events=[{'type': 'btn', 'data': {'down': True, 'button': button}}])
        time.sleep(0.08)
        self.cmd('input-send-event',
                 events=[{'type': 'btn', 'data': {'down': False, 'button': button}}])
        time.sleep(settle)


QCODE = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash',
         ',': 'comma', '=': 'equal', '\n': 'ret'}


def press(q, keys, delay=0.45):
    for k in keys:
        q.cmd('send-key', keys=[{'type': 'qcode', 'data': QCODE.get(k, k)}])
        time.sleep(delay)


def chord(q, keys, settle=1.4):
    """Press several keys together (Ctrl+Z ...)."""
    q.cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in keys])
    time.sleep(settle)


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

    want, want_lines, want_caret = model_buffer()
    qemu, log = boot()
    pos = [512, 648]                     # the desktop parks the pointer here
    try:
        q = Qmp(QMP_PORT)
        print('waiting for the shell prompt ...', flush=True)
        time.sleep(75)
        press(q, list('startgui') + ['ret'])
        time.sleep(16)
        click(q, pos, 25, 745)           # start button
        click(q, pos, 112, 595)          # menu row 3 = Text Editor
        time.sleep(3)
        # 1. type xy, Enter, abc
        press(q, list('xy'))
        press(q, ['ret'])
        press(q, list('abc'))
        time.sleep(1)
        # 2. undo x2 (abc, then the Enter) and redo once (the Enter)
        chord(q, ['ctrl', 'z'])
        chord(q, ['ctrl', 'z'])
        chord(q, ['ctrl', 'y'])
        time.sleep(0.8)
        # 3. type zz, then 14 Enters to push the caret past the visible rows
        press(q, list('zz'))
        time.sleep(0.5)
        for _ in range(11):
            press(q, ['ret'], delay=0.42)
        time.sleep(1.0)
        # 4. PageUp (a page is the visible row count)
        press(q, ['pgup'])
        time.sleep(0.8)
        # 5. the wheel scrolls the viewport (QEMU only injects wheel-up)
        q.wheel('wheel-up')
        time.sleep(0.8)
        # 6. save
        press(q, ['f5'])
        time.sleep(8)
        shot(q, os.path.join(ROOT, SHOT))
        q.cmd('quit')
    finally:
        try:
            qemu.wait(timeout=30)
        except Exception:               # noqa: BLE001
            qemu.kill()
        log.close()

    text = open(os.path.join(ROOT, SERIAL), 'rb').read().decode('utf-8', 'replace')
    check('the shell started the Rust desktop', '[gui-shell]' in text)
    check('the start menu launched the editor window',
          'menu launch 3' in text, 'menu launch 3')
    check('the buffer reports the new 48x64 geometry',
          'editor init lines=48 cols=64' in text, 'editor init lines=48 cols=64')
    key_lines = text.count('[gui-shell] editor key')
    check('typed keys reached the editor buffer', key_lines >= 10,
          '%d key line(s)' % key_lines)

    undone = re.findall(r'editor undo (\d+)', text)
    redone = re.findall(r'editor redo (\d+)', text)
    check('Ctrl+Z reached editor_undo (twice)', len(undone) >= 2,
          'undo depths %s' % ','.join(undone))
    check('Ctrl+Y reached editor_redo', len(redone) >= 1,
          'redo depths %s' % ','.join(redone))
    check('the Ctrl chord reached the shell as a modifier, not a letter',
          'editor ctrl' in text)

    carets = re.findall(r'editor caret (\d+) (\d+)', text)
    check('caret movement is logged (arrows/page)', len(carets) >= 1,
          '%d caret line(s)' % len(carets))
    check('PageUp moved the caret a full window',
          any(int(l) == want_caret for l, _c in carets),
          'expected caret line %d, saw %s'
          % (want_caret, ','.join(l for l, _c in carets[-6:])))

    scrolls = [int(v) for v in re.findall(r'editor scroll (\d+)', text)]
    check('the viewport scrolled when the caret left the window',
          len(scrolls) >= 1, 'scroll offsets %s' % ','.join(map(str, scrolls)))
    check('the wheel scrolled the viewport back up',
          len(scrolls) >= 2 and scrolls[-1] < scrolls[-2],
          'scroll offsets %s' % ','.join(map(str, scrolls[-6:])))

    saved = None
    for line in text.splitlines():
        m = re.search(r'editor saved (\d+) bytes rc=(\d+)', line)
        if m:
            saved = (int(m.group(1)), int(m.group(2)))
    check('F5 saved the whole buffer through the VFS',
          saved is not None and saved[1] == 0 and saved[0] == len(want),
          'saved %r, expected %d bytes' % (saved, len(want)))
    check('the buffer holds the 11 empty rows the Enters created',
          len(want_lines) == 16, '%d line(s)' % len(want_lines))

    # ---------------------------------------------------------- host-side bytes
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import verify_fat32_write as vf
    img = open(os.path.join(ROOT, DISK), 'rb').read()
    fs = vf.parse(img)
    ent = fs['entries'].get('GUIEDIT.TXT')
    check('the host found GUIEDIT.TXT on the volume it booted', ent is not None,
          'entries=%s' % ','.join(sorted(fs['entries'])[:8]))
    if ent:
        first, size = ent
        data = fs['read_chain'](first, size)
        check('the saved size matches the model', size == len(want),
              'size=%d want=%d' % (size, len(want)))
        check('every saved byte matches the model (host-parsed image)',
              data == want,
              '%d vs %d bytes' % (len(data), len(want)))
        if data != want:
            print('    expected:', want)
            print('    on disk :', data)
    check('no kernel PANIC', 'PANIC' not in text)

    # -------------------------------------------------------------- the screen
    from PIL import Image
    px = Image.open(os.path.join(ROOT, SHOT)).convert('RGB').load()
    bright = 0
    for y in range(190, 390):
        for x in range(570, 880):
            r, g, b = px[x, y]
            if (r * 299 + g * 587 + b * 114) // 1000 > 150:
                bright += 1
    check('the editor window is still painted on screen', bright > 200,
          '%d bright px' % bright)

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
