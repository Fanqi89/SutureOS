#!/usr/bin/env python3
"""SutureOS VMware SVGA II (vmsvga) 2D backend acceptance.

Phase 1 - the backend itself, on a machine with *no VGA*:
  QEMU is started with `-vga none -device vmware-svga`, so the VMware SVGA II
  PCI function (15ad:0405) is the only display output.  The shell command
  `vmsvga on` is injected over QMP **one character at a time** (a batched
  sendkey loses keys), and several QMP screendumps are taken.  numpy then
  asserts on the dumps:

    * the dump geometry equals the geometry the driver reports it programmed
      (so the scanout really is our frame, not a firmware leftover),
    * the non-black pixel ratio is ~1.0,
    * all eight colour bands of the animation are pixel-exact,
    * the pure-white square is exactly 64x64 = 4096 px and its x position
      differs between two dumps taken ~1.2 s apart (the frame really moves),
    * after `vmsvga off` the `[vmsvga] update #` counter stops growing and the
      last frame stays on screen (the scanout is deliberately left enabled),
    * `vmsvga status` reports state off with errors=0.

Phase 2 - default-path regression (same ISO, `-vga std`, no vmware-svga):
  the boot must not print a single `[vmsvga]` line, the boot self-tests must
  still be 16/16, the framebuffer console must still be drawn, and there must
  be no PANIC.  That is what proves the frozen boot presentation is untouched.

Usage:
  py -3 scripts/vmsvga_acceptance.py                # both phases
  py -3 scripts/vmsvga_acceptance.py --no-assert    # only collect artefacts
  py -3 scripts/vmsvga_acceptance.py --skip-regression
Exit code 0 = all checks passed, 1 = an assertion failed, 2 = environment
problem (no QEMU / no numpy / ISO missing).

Artefacts: build/serial_vmsvga.log, build/vmsvga_shot_a.ppm, _b.ppm, _off.ppm,
build/serial_vmsvga_std.log, build/vmsvga_std.ppm
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

LOG = 'build/serial_vmsvga.log'
LOG_STD = 'build/serial_vmsvga_std.log'
SHOT_A = 'build/vmsvga_shot_a.ppm'
SHOT_B = 'build/vmsvga_shot_b.ppm'
SHOT_OFF = 'build/vmsvga_shot_off.ppm'
SHOT_STD = 'build/vmsvga_std.ppm'
PORT = 45773          # briefed monitor port for the vmsvga phase
PORT_STD = 45774

# Must stay in sync with g_band_color[] in kernel/drivers/vmsvga.c.
BANDS = [
    (0x30, 0x20, 0x18), (0xA0, 0x30, 0x20), (0xA0, 0x70, 0x20), (0x60, 0x80, 0x20),
    (0x20, 0x80, 0x40), (0x20, 0x70, 0x80), (0x20, 0x40, 0x80), (0x60, 0x20, 0x80),
]
SQ = 64


class Qmp:
    def __init__(self, port, timeout=45):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
                break
            except OSError as e:            # noqa: PERF203 - retry loop by design
                last = e
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


def read_ppm(path):
    """Minimal P6 reader -> numpy array (h, w, 3) uint8.  No Pillow needed."""
    import numpy as np

    with open(path, 'rb') as fh:
        data = fh.read()
    fields, pos = [], 0
    while len(fields) < 4:
        end = data.index(b'\n', pos)
        line = data[pos:end]
        pos = end + 1
        if line.startswith(b'#'):
            continue
        fields.extend(line.split())
    if fields[0] != b'P6':
        raise RuntimeError('%s is not a P6 PPM (%r)' % (path, fields[0]))
    w, h = int(fields[1]), int(fields[2])
    pixels = np.frombuffer(data[pos:pos + w * h * 3], dtype=np.uint8)
    if pixels.size != w * h * 3:
        raise RuntimeError('%s is truncated: %d of %d bytes'
                           % (path, pixels.size, w * h * 3))
    return pixels.reshape(h, w, 3)


def white_bbox(img):
    """Bounding box of the pure-white square, or None."""
    import numpy as np

    m = (img[:, :, 0] == 255) & (img[:, :, 1] == 255) & (img[:, :, 2] == 255)
    ys, xs = np.nonzero(m)
    if not len(xs):
        return None
    return (int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max()), int(len(xs)))


def band_pixels_ok(img):
    """Every band centre must be exactly the colour the driver paints."""
    h, w, _ = img.shape
    bad = []
    for i, want in enumerate(BANDS):
        y = int(h * (i + 0.5) / 8)
        got = tuple(int(v) for v in img[y, 8])       # x=8: never the square
        if got != want:
            bad.append('band %d at y=%d: want %s got %s' % (i, y, want, got))
    return bad


def count(log, needle):
    return log.count(needle)


def launch(args, log_name='build/qemu_vmsvga.out'):
    print('[vmsvga] QEMU ' + ' '.join(args), flush=True)
    # Keep QEMU's own stdout/stderr: a device-model assert or a fatal error is
    # the fastest explanation for a dropped QMP connection.
    try:
        fh = open(os.path.join(ROOT, log_name), 'wb')
    except OSError:
        fh = subprocess.DEVNULL
    return subprocess.Popen(args, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT)


def stop(proc, q):
    try:
        if q is not None:
            q.cmd('quit')
    except Exception:                        # noqa: BLE001 - teardown best effort
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()


def log_text(path):
    try:
        with open(os.path.join(ROOT, path), encoding='utf-8', errors='replace') as fh:
            return fh.read()
    except OSError:
        return ''


def wait_for(fn, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(0.25)
    raise RuntimeError('timed out waiting for %s' % what)


def dump(q, path, settle=0.5):
    """screendump with a RELATIVE path (QEMU runs with cwd = project root)."""
    if os.path.exists(os.path.join(ROOT, path)):
        os.remove(os.path.join(ROOT, path))
    q.hmp('screendump ' + path)
    time.sleep(settle)
    if not os.path.exists(os.path.join(ROOT, path)):
        raise RuntimeError('screendump produced no file: %s' % path)


def phase_vmsvga(checks):
    """Phase 1: the backend on a machine without any VGA."""
    for p in (LOG, SHOT_A, SHOT_B, SHOT_OFF):
        f = os.path.join(ROOT, p)
        if os.path.exists(f):
            os.remove(f)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-vga', 'none', '-device', 'vmware-svga',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = launch(argv)
    q = None
    try:
        q = Qmp(PORT)

        # A batched sendkey drops keys on this workload: one character at a
        # time, 75 ms apart (briefed).
        def key(k, d=0.075):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key('spc' if ch == ' ' else ch)

        def cmdline(text):
            for _ in range(24):
                key('backspace', 0.01)
            typ(text)
            key('ret')

        wait_for(lambda: 'sutureos# ' in log_text(LOG), 240, 'the shell prompt')
        checks.append(('boot: self-tests still 16/16',
                       'selftests: 16/16 passed' in log_text(LOG), ''))
        checks.append(('boot: no PANIC before the backend is used',
                       'PANIC' not in log_text(LOG), ''))
        # The opt-in contract: nothing in the driver runs at boot, so an image
        # built with it still boots (and stays) completely silent about it.
        checks.append(('boot: not one [vmsvga] line before the opt-in',
                       '[vmsvga]' not in log_text(LOG),
                       str(log_text(LOG).count('[vmsvga]')) + ' occurrences'))

        # `vmsvga` with no argument must answer honestly before bring-up.
        cmdline('vmsvga')
        wait_for(lambda: '[vmsvga] state: not initialized' in log_text(LOG), 40,
                 'vmsvga status before bring-up')
        checks.append(('serial: `vmsvga` before bring-up reports not-initialized '
                       'and still finds the PCI function',
                       re.search(r'\[vmsvga\] state: not initialized', log_text(LOG))
                       is not None
                       and 'a VMware SVGA II PCI function (15ad:0405) is present'
                       in log_text(LOG), ''))

        # --- vmsvga on -----------------------------------------------------
        cmdline('vmsvga on')
        wait_for(lambda: '[vmsvga] on (' in log_text(LOG), 120, '[vmsvga] on')

        log = log_text(LOG)
        checks.append(('serial: VMware SVGA II PCI function found (15ad:0405)',
                       re.search(r'\[vmsvga\] pci \d+:\d+\.\d+ id=15ad:0405 '
                                 r'bar0=0x[0-9a-f]+ bar1=0x[0-9a-f]+ bar2=0x[0-9a-f]+',
                                 log) is not None, ''))
        m = re.search(r'\[vmsvga\] id=0x([0-9a-f]+) accepted', log)
        checks.append(('serial: SVGA_REG_ID round trip (index/value ports)',
                       m is not None, m.group(0) if m else 'no id line'))
        m = re.search(r'\[vmsvga\] id=0x([0-9a-f]+) regs w=(\d+) h=(\d+) bpp=(\d+) '
                      r'fb_offset=(\d+) fb_size=(\d+) fifo=0x([0-9a-f]+) size=(\d+)',
                      log)
        checks.append(('serial: register file read back (w/h/bpp/fb/fifo)',
                       m is not None, m.group(0) if m else 'no regs line'))
        res_w, res_h = (int(m.group(2)), int(m.group(3))) if m else (0, 0)
        fb_size = int(m.group(6)) if m else 0
        fifo_size = int(m.group(8)) if m else 0
        checks.append(('serial: FIFO configured in bytes and SYNC ok',
                       re.search(r'\[vmsvga\] fifo cfg min=(\d+) max=(\d+) '
                                 r'units=bytes next=(\d+) sync=ok', log) is not None,
                       (re.search(r'\[vmsvga\] fifo cfg[^\n]*', log).group(0)
                        if 'fifo cfg' in log else 'no fifo cfg line')))
        checks.append(('serial: VRAM/FIFO windows identity-mapped',
                       'mapped vram=0x' in log and ' fifo=0x' in log, ''))
        checks.append(('serial: ENABLE latched and scanout programmed',
                       '[vmsvga] on (' in log
                       and 'rejected' not in log, '%dx%d fb=%d fifo=%d'
                       % (res_w, res_h, fb_size, fifo_size)))

        n0 = count(log, '[vmsvga] update #')
        wait_for(lambda: count(log_text(LOG), '[vmsvga] update #') >= n0 + 5, 90,
                 'five periodic updates')
        checks.append(('serial: periodic SVGA_CMD_UPDATE running',
                       count(log_text(LOG), '[vmsvga] update #') >= n0 + 5,
                       '%d updates' % count(log_text(LOG), '[vmsvga] update #')))

        # `vmsvga on` twice must not stack a second bring-up.
        cmdline('vmsvga on')
        wait_for(lambda: '[vmsvga] already on' in log_text(LOG), 40,
                 'idempotent second vmsvga on')
        checks.append(('serial: second `vmsvga on` is idempotent',
                       '[vmsvga] already on' in log_text(LOG)
                       and log_text(LOG).count('mapped vram=') == 1,
                       'mapped vram= x%d' % log_text(LOG).count('mapped vram=')))

        # --- two dumps, the square must be somewhere else in the second ---
        dump(q, SHOT_A, 0.5)
        time.sleep(1.2)
        dump(q, SHOT_B, 0.5)
        box_a = white_bbox(read_ppm(os.path.join(ROOT, SHOT_A)))
        box_b = white_bbox(read_ppm(os.path.join(ROOT, SHOT_B)))
        if box_a and box_b and box_a[0] == box_b[0]:
            for _ in range(8):               # kept the same frame position: retry
                time.sleep(0.4)
                dump(q, SHOT_B, 0.4)
                box_b = white_bbox(read_ppm(os.path.join(ROOT, SHOT_B)))
                if box_b and box_b[0] != box_a[0]:
                    break

        # --- vmsvga off ----------------------------------------------------
        cmdline('vmsvga off')
        wait_for(lambda: '[vmsvga] off after' in log_text(LOG), 40, '[vmsvga] off')
        time.sleep(1.5)
        off_count_1 = count(log_text(LOG), '[vmsvga] update #')
        time.sleep(2.5)
        off_count_2 = count(log_text(LOG), '[vmsvga] update #')
        checks.append(('serial: [vmsvga] off accepted',
                       '[vmsvga] off after' in log_text(LOG), ''))
        checks.append(('serial: update counter stopped after off',
                       off_count_1 == off_count_2,
                       'after 1.5s=%d, after 4.0s=%d' % (off_count_1, off_count_2)))
        # `vmsvga status` (and `vmsvga`) must report the live state honestly.
        cmdline('vmsvga status')
        wait_for(lambda: re.search(r'\[vmsvga\] state: off', log_text(LOG)) is not None,
                 40, 'vmsvga status output')
        m = re.search(r'\[vmsvga\] fifo=[^\n]*', log_text(LOG))
        checks.append(('serial: `vmsvga status` reports state off / errors=0',
                       m is not None and 'errors=0' in m.group(0),
                       m.group(0).strip() if m else 'no fifo line'))
        # A second `vmsvga off` must be a no-op, not an error.
        cmdline('vmsvga off')
        wait_for(lambda: '[vmsvga] off (already off)' in log_text(LOG), 40,
                 'idempotent second vmsvga off')
        checks.append(('serial: second `vmsvga off` is a no-op',
                       '[vmsvga] off (already off)' in log_text(LOG), ''))
        dump(q, SHOT_OFF, 0.5)
        # Let the serial line finish before quitting (a quit mid-write truncates
        # the last log line).
        time.sleep(1.0)
    finally:
        stop(proc, q)

    # --- static serial assertions ----------------------------------------
    log = log_text(LOG)
    checks.append(('serial: no PANIC', 'PANIC' not in log, ''))
    bad = [s for s in ('sync TIMEOUT', 'presentation failed', 'did not stick',
                       'on failed', 'not usable') if s in log]
    checks.append(('serial: no SVGA command / register failure', not bad,
                   ', '.join(bad)))
    idx_off = log.find('[vmsvga] off after')
    after_off = log[idx_off:] if idx_off >= 0 else ''
    checks.append(('serial: no update line after off',
                   '[vmsvga] update #' not in after_off, ''))

    # --- pixel assertions -------------------------------------------------
    img_a = read_ppm(os.path.join(ROOT, SHOT_A))
    img_b = read_ppm(os.path.join(ROOT, SHOT_B))
    img_off = read_ppm(os.path.join(ROOT, SHOT_OFF))
    h, w, _ = img_a.shape
    checks.append(('pixels: dump geometry equals the programmed geometry',
                   (w, h) == (res_w, res_h),
                   'dump %dx%d, driver %dx%d' % (w, h, res_w, res_h)))

    nonblack = float((img_a.sum(axis=2) > 24).mean())
    checks.append(('pixels: non-black ratio >= 0.95', nonblack >= 0.95,
                   '%.4f' % nonblack))

    bad = band_pixels_ok(img_a)
    checks.append(('pixels: all 8 gradient bands exact', not bad, '; '.join(bad)))

    box_a = white_bbox(img_a)
    checks.append(('pixels: white square is exactly 64x64',
                   box_a is not None and box_a[1] - box_a[0] + 1 == SQ
                   and box_a[3] - box_a[2] + 1 == SQ and box_a[4] == SQ * SQ,
                   'bbox=%s' % (box_a,)))
    box_b = white_bbox(img_b)
    checks.append(('pixels: white square moved between the two dumps',
                   box_a is not None and box_b is not None and box_a[0] != box_b[0],
                   'x: %s -> %s' % (box_a[0] if box_a else None,
                                    box_b[0] if box_b else None)))
    checks.append(('pixels: second dump still has the 8 bands',
                   not band_pixels_ok(img_b), '; '.join(band_pixels_ok(img_b))))

    nonblack_off = float((img_off.sum(axis=2) > 24).mean())
    # `vmsvga off` deliberately hands the screen back to the terminal (the
    # frame is not kept, unlike the virtio-gpu backend whose scanout stays
    # attached): the terminal repaints from its own grid, so the 8 bands must
    # be gone and terminal text must be visible again.
    bands_off = band_pixels_ok(img_off)
    checks.append(('pixels: after off the terminal has the screen back',
                   len(bands_off) > 0 and nonblack_off > 0.004,
                   'bands still present: %d, lit=%.4f'
                   % (8 - len(bands_off), nonblack_off)))


def phase_default_boot(checks):
    """Phase 2: the frozen default path (`-vga std`, no vmware-svga)."""
    for p in (LOG_STD, SHOT_STD):
        f = os.path.join(ROOT, p)
        if os.path.exists(f):
            os.remove(f)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-vga', 'std',
            '-serial', 'file:' + LOG_STD,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT_STD,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = launch(argv, 'build/qemu_vmsvga_std.out')
    q = None
    try:
        q = Qmp(PORT_STD)
        wait_for(lambda: 'sutureos# ' in log_text(LOG_STD), 240, 'the shell prompt')
        time.sleep(0.7)
        dump(q, SHOT_STD, 0.5)
        time.sleep(0.7)
        pre_cmd_log = log_text(LOG_STD)

        # Honest-failure path: with no VMware SVGA II present, `vmsvga on` must
        # say so and leave the screen with the terminal (no takeover, no blank).
        def key(k, d=0.075):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def cmdline(text):
            for _ in range(24):
                key('backspace', 0.01)
            for ch in text:
                key('spc' if ch == ' ' else ch)
            key('ret')

        cmdline('vmsvga on')
        wait_for(lambda: 'could not start the VMware SVGA II 2D backend'
                 in log_text(LOG_STD), 40, 'the honest failure of vmsvga on')
        cmdline('vmsvga')
        wait_for(lambda: 'no VMware SVGA II PCI function found' in log_text(LOG_STD),
                 40, 'vmsvga status with no device')
        dump(q, 'build/vmsvga_std_after_fail.ppm', 0.5)
    finally:
        stop(proc, q)

    log = log_text(LOG_STD)
    checks.append(('default boot (-vga std): not one [vmsvga] line',
                   '[vmsvga]' not in pre_cmd_log,
                   str(pre_cmd_log.count('[vmsvga]')) + ' occurrences'))
    checks.append(('default boot (-vga std): framebuffer console still up',
                   'fbcon initialized' in log and 'no framebuffer' not in log, ''))
    checks.append(('default boot (-vga std): self-tests 16/16',
                   'selftests: 16/16 passed' in log, ''))
    checks.append(('default boot (-vga std): no PANIC', 'PANIC' not in log, ''))

    img = read_ppm(os.path.join(ROOT, SHOT_STD))
    lit = float((img.sum(axis=2) > 24).mean())
    checks.append(('default boot (-vga std): terminal text visible on screen',
                   lit > 0.005, 'lit=%.4f' % lit))

    # --- honest failure when the device is not there -----------------------
    checks.append(('no device: `vmsvga on` refuses honestly (no fake success)',
                   'no VMware SVGA II on PCI (id 15ad:0405)' in log
                   and 'could not start the VMware SVGA II 2D backend' in log, ''))
    checks.append(('no device: `vmsvga` status says no PCI function found',
                   'no VMware SVGA II PCI function found' in log, ''))
    img_fail = read_ppm(os.path.join(ROOT, 'build/vmsvga_std_after_fail.ppm'))
    lit_fail = float((img_fail.sum(axis=2) > 24).mean())
    bands_after_fail = band_pixels_ok(img_fail)
    checks.append(('no device: the screen is NOT taken over by the failed attempt',
                   lit_fail > 0.002 and len(bands_after_fail) == len(BANDS),
                   'lit=%.4f, band samples still background=%d/%d'
                   % (lit_fail, len(bands_after_fail), len(BANDS))))


def main():
    assert_dir = '--no-assert' not in sys.argv
    skip_regression = '--skip-regression' in sys.argv

    if not os.path.exists(QEMU):
        print('[vmsvga] QEMU not found at %s (environment problem)' % QEMU)
        return 2
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[vmsvga] build/sutureos.iso is missing: run scripts/build_iso.py first')
        return 2
    try:
        import numpy  # noqa: F401
    except ImportError:
        print('[vmsvga] numpy is required: py -3 -m pip install numpy')
        return 2

    checks = []
    try:
        phase_vmsvga(checks)
        if not skip_regression:
            phase_default_boot(checks)
    except RuntimeError as e:
        print('[vmsvga] FAILED: %s' % e)
        return 1

    ok = True
    print('\n[vmsvga] checks:')
    for name, cond, detail in checks:
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))
        if not cond:
            ok = False
    passed = sum(1 for _, c, _ in checks if c)
    print('\n[vmsvga] %d/%d checks passed' % (passed, len(checks)))
    if not assert_dir:
        print('[vmsvga] --no-assert: artefacts only')
        return 0
    print('[vmsvga] RESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
