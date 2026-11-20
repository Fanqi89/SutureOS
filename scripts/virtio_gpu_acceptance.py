#!/usr/bin/env python3
"""SutureOS VirtIO-GPU 2D backend acceptance (B16).

Phase 1 - the backend itself, with *no VGA at all*:
  QEMU is started with `-vga none -device virtio-gpu-pci`, so the kernel boots
  with a serial console ("[init] no framebuffer - serial console only") and the
  virtio-gpu scanout is the only display output.  The shell command `vga2d on`
  is injected over QMP, then several QMP screendumps are taken.  numpy asserts
  on the dumps:

    * the dump geometry equals the geometry the driver reported for the
      resource it created (so the scanout really is our resource),
    * the non-black pixel ratio is ~1.0,
    * all eight colour bands of the animation are pixel-exact,
    * the pure-white square is exactly 64x64 (4096 px) and its x position
      differs between two dumps taken ~1.2 s apart (the frame really moves),
    * after `vga2d off` the `[vga2d] flush #` counter stops growing and the
      last frame is still on screen (the scanout is deliberately left
      attached).

Phase 2 - default-path regression (same ISO, `-vga std`, no virtio-gpu):
  the boot must not print a single `[vga2d]` line, the self-tests must still be
  16/16 and the framebuffer console must still be drawn (non-black text on the
  screenshot).  This is what proves the frozen boot presentation is untouched.

Usage:
  py -3 scripts/virtio_gpu_acceptance.py                # both phases
  py -3 scripts/virtio_gpu_acceptance.py --no-assert    # only collect artefacts
  py -3 scripts/virtio_gpu_acceptance.py --skip-regression
Exit code 0 = all checks passed, 1 = an assertion failed, 2 = environment
problem (no QEMU / no numpy / ISO missing).

Artefacts: build/serial_vga2d.log, build/vga2d_shot_a.ppm, _b.ppm, _off.ppm,
build/serial_vga2d_std.log, build/vga2d_std.ppm
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

LOG = 'build/serial_vga2d.log'
LOG_STD = 'build/serial_vga2d_std.log'
SHOT_A = 'build/vga2d_shot_a.ppm'
SHOT_B = 'build/vga2d_shot_b.ppm'
SHOT_OFF = 'build/vga2d_shot_off.ppm'
SHOT_STD = 'build/vga2d_std.ppm'
PORT = 45711
PORT_STD = 45712

# Must stay in sync with g_band_color[] in kernel/drivers/virtio_gpu.c.
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


def launch(args):
    print('[vga2d] QEMU ' + ' '.join(args), flush=True)
    return subprocess.Popen(args, cwd=ROOT)


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


def dump(q, path, settle=0.35):
    """screendump with a RELATIVE path (QEMU runs with cwd = project root)."""
    if os.path.exists(os.path.join(ROOT, path)):
        os.remove(os.path.join(ROOT, path))
    q.hmp('screendump ' + path)
    time.sleep(settle)
    if not os.path.exists(os.path.join(ROOT, path)):
        raise RuntimeError('screendump produced no file: %s' % path)


def phase_virtio_gpu(checks):
    """Phase 1: the backend on a machine without any VGA."""
    for p in (LOG, SHOT_A, SHOT_B, SHOT_OFF):
        f = os.path.join(ROOT, p)
        if os.path.exists(f):
            os.remove(f)

    argv = [QEMU, '-machine', 'pc', '-m', '256', '-display', 'none',
            '-vga', 'none', '-device', 'virtio-gpu-pci',
            '-serial', 'file:' + LOG,
            '-qmp', 'tcp:127.0.0.1:%d,server,nowait' % PORT,
            '-cdrom', 'build/sutureos.iso', '-boot', 'd',
            '-smp', '2', '-no-reboot', '-no-shutdown']
    proc = launch(argv)
    q = None
    try:
        q = Qmp(PORT)

        def key(k, d=0.06):
            q.hmp('sendkey ' + k)
            time.sleep(d)

        def typ(text):
            for ch in text:
                key('spc' if ch == ' ' else ch)

        wait_for(lambda: 'sutureos# ' in log_text(LOG), 240, 'the shell prompt')
        checks.append(('boot: no framebuffer (serial console only)',
                       'no framebuffer - serial console only' in log_text(LOG), ''))
        checks.append(('boot: self-tests still 16/16',
                       'selftests: 16/16 passed' in log_text(LOG), ''))

        # --- vga2d on -----------------------------------------------------
        for _ in range(24):
            key('backspace', 0.01)
        typ('vga2d on')
        key('ret')
        wait_for(lambda: '[vga2d] on ' in log_text(LOG), 90, '[vga2d] on')

        log = log_text(LOG)
        checks.append(('serial: modern virtio-pci function found',
                       re.search(r'\[vga2d\] pci \d+:\d+\.\d+ id=1af4:1050 modern caps=[1-9]',
                                 log) is not None, ''))
        m = re.search(r'\[vga2d\] display (\d+)x(\d+) scanout=(\d+)', log)
        checks.append(('serial: GET_DISPLAY_INFO answered', m is not None,
                       m.group(0) if m else 'no display line'))
        m = re.search(r'\[vga2d\] resource created id=(\d+) (\d+)x(\d+) b8g8r8a8', log)
        checks.append(('serial: RESOURCE_CREATE_2D accepted', m is not None,
                       m.group(0) if m else 'no resource line'))
        res_w, res_h = (int(m.group(2)), int(m.group(3))) if m else (0, 0)
        checks.append(('serial: backing / scanout / on accepted',
                       'backing attached' in log and 'scanout 0 <- resource' in log
                       and '[vga2d] on ' in log, '%dx%d' % (res_w, res_h)))

        n0 = count(log, '[vga2d] flush #')
        wait_for(lambda: count(log_text(LOG), '[vga2d] flush #') >= n0 + 5, 60,
                 'five periodic flushes')
        checks.append(('serial: periodic TRANSFER+FLUSH running',
                       count(log_text(LOG), '[vga2d] flush #') >= n0 + 5,
                       '%d flushes' % count(log_text(LOG), '[vga2d] flush #')))

        # `vga2d on` twice must not stack a second resource/scanout.
        for _ in range(24):
            key('backspace', 0.01)
        typ('vga2d on')
        key('ret')
        wait_for(lambda: '[vga2d] already on' in log_text(LOG), 30,
                 'idempotent second vga2d on')
        checks.append(('serial: second `vga2d on` is idempotent',
                       '[vga2d] already on' in log_text(LOG)
                       and log_text(LOG).count('resource created') == 1,
                       'resource created x%d' % log_text(LOG).count('resource created')))

        # --- two dumps, the square must be somewhere else in the second ---
        dump(q, SHOT_A, 0.4)
        time.sleep(1.2)
        dump(q, SHOT_B, 0.4)
        box_a = white_bbox(read_ppm(os.path.join(ROOT, SHOT_A)))
        box_b = white_bbox(read_ppm(os.path.join(ROOT, SHOT_B)))
        if box_a and box_b and box_a[0] == box_b[0]:
            for _ in range(6):               # kept the same frame position: retry
                time.sleep(0.4)
                dump(q, SHOT_B, 0.3)
                box_b = white_bbox(read_ppm(os.path.join(ROOT, SHOT_B)))
                if box_b and box_b[0] != box_a[0]:
                    break

        # --- vga2d off ---------------------------------------------------
        for _ in range(24):
            key('backspace', 0.01)
        typ('vga2d off')
        key('ret')
        wait_for(lambda: '[vga2d] off' in log_text(LOG), 30, '[vga2d] off')
        time.sleep(1.5)
        off_count_1 = count(log_text(LOG), '[vga2d] flush #')
        time.sleep(2.5)
        off_count_2 = count(log_text(LOG), '[vga2d] flush #')
        checks.append(('serial: [vga2d] off accepted', '[vga2d] off' in log_text(LOG), ''))
        checks.append(('serial: flush counter stopped after off',
                       off_count_1 == off_count_2,
                       'after 1.5s=%d, after 4.0s=%d' % (off_count_1, off_count_2)))
        # `vga2d status` (no argument) must report the live state honestly.
        for _ in range(24):
            key('backspace', 0.01)
        typ('vga2d')
        key('ret')
        wait_for(lambda: '[vga2d] state: off' in log_text(LOG), 30,
                 'vga2d status output')
        # A second `vga2d off` must be a no-op, not an error.
        for _ in range(24):
            key('backspace', 0.01)
        typ('vga2d off')
        key('ret')
        wait_for(lambda: '[vga2d] off (already off)' in log_text(LOG), 30,
                 'idempotent second vga2d off')
        checks.append(('serial: second `vga2d off` is a no-op',
                       '[vga2d] off (already off)' in log_text(LOG), ''))
        dump(q, SHOT_OFF, 0.4)
        # Let the serial line finish before quitting (a quit mid-write truncates
        # the last log line).
        time.sleep(1.0)
    finally:
        stop(proc, q)

    # --- static serial assertions ----------------------------------------
    log = log_text(LOG)
    checks.append(('serial: no PANIC', 'PANIC' not in log, ''))
    bad = [s for s in ('resp BAD', 'timed out', 'presentation failed')
           if s in log]
    checks.append(('serial: no virtio-gpu command error', not bad,
                   ', '.join(bad)))
    idx_off = log.find('[vga2d] off')
    after_off = log[idx_off:] if idx_off >= 0 else ''
    checks.append(('serial: no flush line after off',
                   '[vga2d] flush #' not in after_off, ''))
    m = re.search(r'\[vga2d\] state: off .*\n', log)
    checks.append(('serial: `vga2d status` reports state off / errors 0',
                   m is not None and 'errors=0' in log[m.start():],
                   m.group(0).strip() if m else 'no state line'))

    # --- pixel assertions -------------------------------------------------
    img_a = read_ppm(os.path.join(ROOT, SHOT_A))
    img_b = read_ppm(os.path.join(ROOT, SHOT_B))
    img_off = read_ppm(os.path.join(ROOT, SHOT_OFF))
    h, w, _ = img_a.shape
    checks.append(('pixels: dump geometry equals the created resource',
                   (w, h) == (res_w, res_h),
                   'dump %dx%d, resource %dx%d' % (w, h, res_w, res_h)))

    import numpy as np

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
    checks.append(('pixels: last frame still displayed after off',
                   nonblack_off >= 0.95, '%.4f' % nonblack_off))


def phase_default_boot(checks):
    """Phase 2: the frozen default path (`-vga std`, no virtio-gpu) is unchanged."""
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
    proc = launch(argv)
    q = None
    try:
        q = Qmp(PORT_STD)
        wait_for(lambda: 'sutureos# ' in log_text(LOG_STD), 240, 'the shell prompt')
        time.sleep(0.7)
        dump(q, SHOT_STD, 0.4)
        time.sleep(0.7)
    finally:
        stop(proc, q)

    log = log_text(LOG_STD)
    checks.append(('default boot (-vga std): not one [vga2d] line',
                   '[vga2d]' not in log,
                   str(log.count('[vga2d]')) + ' occurrences'))
    checks.append(('default boot (-vga std): framebuffer console still up',
                   'fbcon initialized' in log and 'no framebuffer' not in log, ''))
    checks.append(('default boot (-vga std): self-tests 16/16',
                   'selftests: 16/16 passed' in log, ''))
    checks.append(('default boot (-vga std): no PANIC', 'PANIC' not in log, ''))

    img = read_ppm(os.path.join(ROOT, SHOT_STD))
    lit = float((img.sum(axis=2) > 24).mean())
    checks.append(('default boot (-vga std): terminal text visible on screen',
                   lit > 0.005, 'lit=%.4f' % lit))


def main():
    assert_dir = '--no-assert' not in sys.argv
    skip_regression = '--skip-regression' in sys.argv

    if not os.path.exists(QEMU):
        print('[vga2d] QEMU not found at %s (environment problem)' % QEMU)
        return 2
    if not os.path.exists(os.path.join(ROOT, 'build/sutureos.iso')):
        print('[vga2d] build/sutureos.iso is missing: run scripts/build_iso.py first')
        return 2
    try:
        import numpy  # noqa: F401
    except ImportError:
        print('[vga2d] numpy is required: py -3 -m pip install numpy')
        return 2

    checks = []
    try:
        phase_virtio_gpu(checks)
        if not skip_regression:
            phase_default_boot(checks)
    except RuntimeError as e:
        print('[vga2d] FAILED: %s' % e)
        return 1

    ok = True
    print('\n[vga2d] checks:')
    for name, cond, detail in checks:
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name,
                               ('  (%s)' % detail) if detail else ''))
        if not cond:
            ok = False
    passed = sum(1 for _, c, _ in checks if c)
    print('\n[vga2d] %d/%d checks passed' % (passed, len(checks)))
    if not assert_dir:
        print('[vga2d] --no-assert: artefacts only')
        return 0
    print('[vga2d] RESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
