#!/usr/bin/env python3
"""Run the whole GUI / desktop / Wayland acceptance suite in one pass.

Every gate in this project prints a machine readable tail:

    [<tag>] checks=N ok=N fail=N
    [<tag>] RESULT: PASS | FAIL

so this runner only has to execute each script in its own process (each one
boots its own QEMU instance, which is why they must not run in parallel: they
would fight over the host CPU and over `C:/sutureos_iso_temp`), collect that
tail, and print a summary.  It is deliberately dumb: no rewriting, no retries.
A flake has to be re-run by hand so that it stays visible.

  py -3 scripts/gui_sweep.py                 # full suite
  py -3 scripts/gui_sweep.py --only wl_,swap  # name substring filter
  py -3 scripts/gui_sweep.py --timeout 1800   # per-script timeout (seconds)

Exit code 0 only when every selected script reported RESULT: PASS.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (script, short tag).  Order matters only for readability: the cheap static
# checks come first so a broken build fails fast.
SUITE = [
    ('slint_gui_acceptance.py', 'static'),
    ('slint_gui_interactive_acceptance.py', 'interactive'),
    ('slint_gui_shell_acceptance.py', 'shell'),
    ('gui_shell_behave_acceptance.py', 'shellbehave'),
    ('guishell_swap_acceptance.py', 'swap'),
    ('guishell_damage_acceptance.py', 'damage'),
    ('apps_taskbar_acceptance.py', 'apps_taskbar'),
    ('gui_editor_live_acceptance.py', 'editorlive'),
    ('gui_menu_launch_acceptance.py', 'menulaunch'),
    ('gui_image_browse_acceptance.py', 'imgbrowse'),
    ('gui_calc_acceptance.py', 'calc'),
    ('gui_wheel_acceptance.py', 'wheel'),
    ('gui_vsync_present_acceptance.py', 'vsync_present'),
    ('gui_keyowners_acceptance.py', 'keyowners'),
    ('gui_menu_scroll_acceptance.py', 'menuscroll'),
    ('desktop_acceptance.py', 'desktop'),
    ('gui_apps_content_acceptance.py', 'apps_content'),
    ('gui_baseline_shot.py', 'appearance_new'),
    ('gui_baseline_shot_classic.py', 'appearance_classic'),
    ('logo_acceptance.py', 'logo'),
    ('wl_server_acceptance.py', 'wl_server'),
    ('wl_protocol_acceptance.py', 'wl_protocol'),
    ('wl_multi_acceptance.py', 'wl_multi'),
    ('wl_win_semantics_acceptance.py', 'wl_semantics'),
    ('wl_show_acceptance.py', 'wl_show'),
    ('wl_desktop_window_acceptance.py', 'wl_window'),
    ('wlprobe_acceptance.py', 'wlprobe'),
    ('gui_cjk_acceptance.py', 'gui_cjk'),
    ('imgview_acceptance.py', 'imgview'),
    ('editor_acceptance.py', 'editor'),
    ('unix_acceptance.py', 'unix'),
    ('shm_acceptance.py', 'shm'),
    ('fdpass_acceptance.py', 'fdpass'),
]

RE_TAIL = re.compile(r'checks=(\d+)\s+ok=(\d+)\s+fail=(\d+)')
RE_RESULT = re.compile(r'RESULT:\s*(PASS|FAIL)')
# a few scripts use `RESULT: PASS (checks=N)` instead of the checks/ok/fail tail
RE_RESULT_CHECKS = re.compile(r'RESULT:\s*(PASS|FAIL)\s*\(checks=(\d+)\)')


def fresh_test_volume():
    """Rebuild the FAT32 fixture QEMU boots with (see the module docstring)."""
    gen = os.path.join(ROOT, 'scripts', 'mkfat32_testimg.py')
    subprocess.run([sys.executable, gen], cwd=ROOT, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    src = os.path.join(ROOT, 'build', 'fat32.img')
    dst = os.path.join(ROOT, 'build', 'fat32_ata.img')
    if os.path.exists(src):
        shutil.copyfile(src, dst)


def run_one(script, timeout):
    fresh_test_volume()
    path = os.path.join(ROOT, 'scripts', script)
    if not os.path.exists(path):
        return ('missing', None, None, None, 0.0, 'script not found')
    t0 = time.time()
    try:
        p = subprocess.run([sys.executable, path], cwd=ROOT, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode('utf-8', 'replace')
        rc = p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode('utf-8', 'replace')
        rc = 'timeout'
    dt = time.time() - t0
    checks = ok = fail = None
    for m in RE_TAIL.finditer(out):
        checks, ok, fail = int(m.group(1)), int(m.group(2)), int(m.group(3))
    for m in RE_RESULT_CHECKS.finditer(out):
        if checks is None:
            checks = int(m.group(2))
            ok = checks if m.group(1) == 'PASS' else None
            fail = 0 if m.group(1) == 'PASS' else checks
    res = None
    for m in RE_RESULT.finditer(out):
        res = m.group(1)
    if res is None:
        res = 'PASS' if rc == 0 else 'FAIL'
    return (res, checks, ok, fail, dt, '' if rc == 0 else 'rc=%s' % rc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='', help='comma separated name substrings')
    ap.add_argument('--timeout', type=int, default=2400)
    args = ap.parse_args()

    want = [s.strip() for s in args.only.split(',') if s.strip()]
    sel = [(s, t) for (s, t) in SUITE
           if not want or any(w in s or w == t for w in want)]
    print('gui-sweep: %d script(s), timeout %ds each' % (len(sel), args.timeout))

    rows = []
    for script, tag in sel:
        print('--- %-42s (%s)' % (script, tag), flush=True)
        res, checks, ok, fail, dt, note = run_one(script, args.timeout)
        rows.append((tag, script, res, checks, ok, fail, dt, note))
        counts = ('%s/%s' % (ok, checks)) if checks is not None else '-'
        print('    %-6s %-8s %6.1fs %s' % (res, counts, dt, note), flush=True)

    print('\n=== gui-sweep summary ===')
    bad = 0
    for tag, script, res, checks, ok, fail, dt, note in rows:
        if res != 'PASS':
            bad += 1
        print('%-14s %-6s %-8s %6.1fs %s'
              % (tag, res, ('%s/%s' % (ok, checks)) if checks is not None else '-',
                 dt, note))
    print('=== %d/%d PASS ===' % (len(rows) - bad, len(rows)))
    with open(os.path.join(ROOT, 'build', 'gui_sweep_report.txt'), 'w',
              encoding='utf-8') as f:
        for tag, script, res, checks, ok, fail, dt, note in rows:
            f.write('%s\t%s\t%s\t%s\t%s\t%s\t%.1f\t%s\n'
                    % (tag, script, res, checks, ok, fail, dt, note))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
