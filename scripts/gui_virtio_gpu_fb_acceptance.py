#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SutureOS virtio-gpu framebuffer acceptance (v0.70).

The contributed virtio-gpu work lets the OS use a virtio-gpu scanout as the
*whole* system display when the firmware provides no linear framebuffer at all
(`-vga none -device virtio-gpu-pci`). This gate proves it end to end:

  1. boot: `[vgpu] framebuffer ready ... bpp=32` + `[init] fbcon initialized
     (virtio-gpu scanout)`, and never the old "serial console only" fallback;
  2. the screen really carries the terminal: screendump size == the reported
     framebuffer size, and the banner is drawn (bright pixels in the top band);
  3. `startgui` lands on the same scanout: `[gui-shell] shell: loop`, the
     desktop colours appear (window body / taskbar, not a black screen), ESC
     returns to the terminal;
  4. control: a `-vga std` boot must print **no** `[vgpu]`/`[vga2d]` line, so
     the frozen boot presentation is untouched.

Exit code 0 only when every check passes.
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
PORT = int(os.environ.get('VGPU_QMP_PORT', '48205'))
SERIAL = 'build/serial_vgpu_fb.log'
SERIAL2 = 'build/serial_vgpu_std.log'
DISK = 'build/fat32_ata.img'


class Qmp:
    def __init__(self, port, timeout=120):
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), 1.0)
                self.f = self.s.makefile('rwb', buffering=0)
                self.f.readline()
                self.cmd('qmp_capabilities')
                return
            except Exception as e:          # noqa: BLE001
                last = e
                time.sleep(0.3)
        raise RuntimeError('QMP connect failed: %r' % (last,))

    def cmd(self, name, **args):
        self.f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            rep = json.loads(line)
            if 'event' not in rep:
                if 'error' in rep:
                    raise RuntimeError('QMP error: %r' % (rep['error'],))
                return rep

    def shot(self, name):
        p = os.path.join(ROOT, 'build', name)
        if os.path.exists(p):
            os.remove(p)
        self.cmd('screendump', filename=p)
        for _ in range(120):
            if os.path.exists(p) and os.path.getsize(p) > 1000:
                time.sleep(0.25)
                return p
            time.sleep(0.05)
        raise RuntimeError('no screendump ' + name)

    def key(self, qcode, settle=0.7):
        self.cmd('send-key', keys=[{'type': 'qcode', 'data': qcode}])
        time.sleep(settle)

    def keys(self, text, delay=0.06):
        for ch in text:
            self.cmd('send-key', keys=[{'type': 'qcode', 'data': ch}])
            time.sleep(delay)
        self.key('ret')


def run(vga_args, serial_name, actions):
    if os.path.exists(os.path.join(ROOT, serial_name)):
        os.remove(os.path.join(ROOT, serial_name))
    log = open(os.path.join(ROOT, 'build', 'qemu_' + os.path.basename(serial_name) + '.out'), 'wb')
    p = subprocess.Popen([
        QEMU, '-machine', 'pc', '-m', '512', '-display', 'none', '-nic', 'none',
        '-cdrom', os.path.join(ROOT, 'build', 'sutureos.iso'), '-boot', 'd',
        '-drive', 'id=adisk,file=%s,format=raw,if=ide' % os.path.join(ROOT, DISK),
        '-serial', 'file:' + serial_name,
        '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
        '-smp', '2', '-no-reboot', '-no-shutdown',
    ] + vga_args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    try:
        q = Qmp(PORT)
        actions(q)
        q.cmd('quit')
    finally:
        try:
            p.wait(timeout=30)
        except Exception:                   # noqa: BLE001
            p.kill()
        log.close()
    return open(os.path.join(ROOT, serial_name), 'rb').read().decode('utf-8', 'replace')


def main():
    checks = []

    def check(name, cond, detail=''):
        checks.append((name, bool(cond), detail))
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''), flush=True)

    subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')],
                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import shutil
    shutil.copyfile(os.path.join(ROOT, 'build', 'fat32.img'), os.path.join(ROOT, DISK))

    from PIL import Image

    state = {}

    def actions_vgpu(q):
        time.sleep(50)
        state['term'] = Image.open(q.shot('vgpu_fb_term.ppm')).convert('RGB')
        q.keys('startgui')
        time.sleep(16)
        state['desk'] = Image.open(q.shot('vgpu_fb_desk.ppm')).convert('RGB')
        q.key('esc', settle=2.0)
        time.sleep(2)

    text = run(['-vga', 'none', '-device', 'virtio-gpu-pci'], SERIAL, actions_vgpu)

    # ---- 1. boot path -------------------------------------------------------
    check('the device was found and a 2D resource created',
          '[vga2d] resource created id=1' in text, 'resource created')
    import re
    m = re.search(r'\[vgpu\] display (\d+)x(\d+) -> framebuffer (\d+)x(\d+)', text)
    check('the reported display became the framebuffer',
          m is not None, m.group(0) if m else 'no [vgpu] display line')
    fw = fh = 0
    if m:
        fw, fh = int(m.group(3)), int(m.group(4))
    check('scanout 0 was pointed at the resource',
          '[vga2d] scanout 0 <- resource 1' in text, 'scanout line')
    check('the framebuffer is 32 bpp with a sane pitch',
          re.search(r'\[vgpu\] framebuffer ready: addr=0x[0-9a-f]+ %dx%d pitch=(\d+) bpp=32'
                    % (fw, fh), text) is not None,
          'framebuffer ready line')
    check('fbcon took the virtio-gpu scanout',
          '[init] fbcon initialized (virtio-gpu scanout)' in text, 'fbcon line')
    check('the serial-only fallback did not trigger',
          'no framebuffer - serial console only' not in text, 'fallback absent')

    # ---- 2. the terminal really is on that scanout --------------------------
    term = state.get('term')
    check('a screendump exists and matches the reported framebuffer size',
          term is not None and term.size == (fw, fh),
          '%s vs %s' % (term.size if term else None, (fw, fh)))
    if term is not None:
        px = term.load()
        bright = sum(1 for y in range(0, 200) for x in range(0, min(fw, 900))
                     if px[x, y][0] > 180 and px[x, y][1] > 180)
        check('the boot banner is drawn on the virtio scanout', bright > 500,
              '%d bright pixels in the top band' % bright)

    # ---- 3. the desktop lands on the same scanout ---------------------------
    check('startgui started the Rust shell', '[gui-shell] shell: loop' in text,
          'shell loop')
    check('the shell found a 32 bpp framebuffer',
          'need a 32bpp framebuffer' not in text, 'no fb error')
    desk = state.get('desk')
    if desk is not None and term is not None:
        px = desk.load()
        # the Rust shell draws its 1024x768 scene into the top-left of the
        # scanout; count saturated chrome pixels there (window body + taskbar)
        chrome = 0
        colours = set()
        for y in range(64, 768, 4):
            for x in range(0, 1024, 4):
                c = px[x, y]
                colours.add(c)
                if max(c) - min(c) > 24 or max(c) > 90:
                    chrome += 1
        check('the desktop is painted on the virtio scanout (not a black screen)',
              chrome > 4000, '%d chrome samples, %d distinct colours' % (chrome, len(colours)))
        check('the desktop kept its glass look (many distinct colours)',
              len(colours) > 200, '%d distinct colours' % len(colours))
    check('ESC returned from the shell to the terminal',
          'Desktop exited, returning to terminal' in text
          and '[gui-shell] shell: exit after' in text,
          'Desktop exited, returning to terminal')

    # ---- 4. control: -vga std keeps the old path ---------------------------
    def actions_std(q):
        time.sleep(50)

    text2 = run(['-vga', 'std'], SERIAL2, actions_std)
    check('a -vga std boot prints no [vgpu]/[vga2d] line',
          '[vgpu]' not in text2 and '[vga2d]' not in text2,
          'no virtio-gpu lines')
    check('-vga std still reaches the shell prompt',
          'Welcome to SutureOS Terminal' in text2, 'terminal banner')

    ok = sum(1 for _, c, _ in checks if c)
    print('\nchecks=%d ok=%d fail=%d' % (len(checks), ok, len(checks) - ok))
    print('RESULT: %s' % ('PASS' if ok == len(checks) else 'FAIL'))
    return 0 if ok == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
