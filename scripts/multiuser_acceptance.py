#!/usr/bin/env python3
"""SutureOS multiuser acceptance: accounts, login/su/switching, isolation.

Real QEMU + QMP: the script types a fixed command sequence into the shell (the
keyboard path the console really uses), then checks the transcript AND verifies
the on-disk result independently on the host:

  * the guest copies /etc/passwd, /etc/shadow and /etc/group to the FAT32 test
    disk at two points (after the accounts exist, and after both userdels);
  * the host parses that raw FAT32 image (scripts/verify_fat32_write.py) and
    - checks the /etc/passwd line format and the uid allocation (>= 1000),
    - recomputes PBKDF2-HMAC-SHA256 from the salt+iterations in /etc/shadow and
      compares it with the stored hash (proves the guest's hashing and the
      documented line format are real, not just plausible-looking text),
    - confirms that the deleted accounts are gone from both files.

Coverage: useradd (with and without a password), users, passwd (self-service
with the current password, root's -d), login/su (correct, wrong password,
locked, passwordless), sudo policy (deny for an unknown user, allow for a wheel
member), /etc write isolation, and the boot self-test + FAT32 self-test
regression lines from the same serial log.

  py -3 scripts/build.py && py -3 scripts/build_iso.py
  py -3 scripts/mkfat32_testimg.py
  py -3 scripts/multiuser_acceptance.py
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build', 'serial_multiuser.log')
ARGS = os.path.join(ROOT, 'build', 'qemu_multiuser.args')
DISK = os.path.join(ROOT, 'build', 'fat32_mu.img')

KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '_': 'shift-minus',
          ':': 'shift-semicolon'}

ALICE_PW = 'alicepw'
ALICE_PW2 = 'alicepw2'

# The exact interaction script. Every entry is one line the "operator" types;
# password prompts consume the following lines, so the order matters.
# Lines that are only a password/answer are commented with the prompt they answer.
SCRIPT = [
    # ---- phase 0: the security layer's own self-test still passes ---
    'sudotest',
    # ---- phase A: baseline, useradd, /etc line format --------------
    'users',
    'useradd alice',
    ALICE_PW,                       # New password:
    ALICE_PW,                       # Retype new password:
    'users',
    'cat /etc/passwd',
    'cat /etc/shadow',
    # ---- phase B: locked (empty password) account ------------------
    'useradd carol',
    '',                             # New password: (empty => account LOCKED)
    'users',
    'su carol',                     # locked: refused, still root
    'whoami',
    'cp /etc/shadow /mnt/fat/SH0.TXT',   # snapshot 0: carol's entry is `!` (locked)
    'passwd -d carol',              # root clears the password => passwordless
    'users',
    'cp /etc/passwd /mnt/fat/PW1.TXT',   # snapshot 1 (root: alice hashed,
    'cp /etc/shadow /mnt/fat/SH1.TXT',   #  carol passwordless, locked copied first)
    'cp /etc/group /mnt/fat/GR1.TXT',
    # ---- phase C: passwordless login, wrong passwords, isolation ---
    'su carol',                     # passwordless: no prompt
    'whoami',
    'id',
    'su user',                      # non-root: Password: prompt
    'nope',                         # wrong password
    'whoami',                       # still carol
    'login alice',                  # login command, wrong password
    'badpw',                        # Password:
    'whoami',                       # still carol
    'sudo -l',                      # carol has no sudoers rule at all
    'sudo whoami',                  # denied before any password prompt
    'write /etc/passwd hacked',     # isolation: /etc is root-only
    'rm /etc/shadow',               # isolation again
    'useradd bob',                  # not root: refused
    'cp /etc/passwd /mnt/fat/CAROL.TXT',  # reads stay allowed for carol
    # ---- phase D: back to root, sudo allow path for `user` ---------
    'su -',                         # Password: (root's password)
    'root',
    'whoami',
    'su user',                      # root -> user: no password
    'whoami',
    'sudo -l',                      # allowed: wheel rule lists commands
    'user',                         # [sudo] password for user:
    'sudo whoami',                  # cached ticket: runs as root, no prompt
    'whoami',                       # back to `user` (euid restored)
    'su -',                         # Password:
    'root',
    # ---- phase E: self-service passwd with the current password ----
    'su alice',                     # root -> alice: no password
    'whoami',
    'passwd',                       # Current password:
    'wrongold',                     # wrong current password => refused
    'whoami',                       # still alice
    'passwd',                       # Current password:
    ALICE_PW,
    ALICE_PW2,                      # New password:
    ALICE_PW2,                      # Retype new password:
    'whoami',
    'su -',                         # Password:
    'root',
    # ---- phase F: the new password works between two other accounts -
    'su carol',                     # root -> carol: no password
    'whoami',
    'su alice',                     # carol -> alice: Password:
    ALICE_PW2,                      # the NEW password must work
    'whoami',
    'su -',                         # Password:
    'root',
    # ---- phase G: userdel and the final snapshot -------------------
    'userdel alice',
    'users',
    'cat /etc/shadow',
    'userdel root',                 # refused
    'userdel carol',                # allowed
    'users',
    'cp /etc/passwd /mnt/fat/PW2.TXT',   # snapshot 2: alice and carol gone
    'cp /etc/shadow /mnt/fat/SH2.TXT',
    'sync',
]


def keys_for(line):
    out = []
    for ch in line:
        if ch in KEYMAP:
            out.append(KEYMAP[ch])
        elif ch.isupper():
            out.append('shift-' + ch.lower())
        else:
            out.append(ch)
    out.append('ret')
    return ';'.join(out)


def run_qemu():
    shutil.copy2(os.path.join(ROOT, 'build', 'fat32.img'), DISK)
    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown '
                 '-drive id=adisk,file=build/fat32_mu.img,format=raw,if=ide\n')
    keys = ';'.join(keys_for(line) for line in SCRIPT)
    cmd = ['powershell', '-NoProfile', '-File',
           os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
           '-SerialLog', 'build/serial_multiuser.log',
           '-ArgFile', 'build/qemu_multiuser.args',
           '-BootWaitSeconds', '55', '-KeyDelayMs', '150',
           '-Keys', keys, '-AfterKeysSeconds', '6']
    print('running QEMU (%d input lines, %d key events)...'
          % (len(SCRIPT), keys.count(';') + 1))
    subprocess.run(cmd, cwd=ROOT, check=False)
    return open(LOG, encoding='utf-8', errors='replace').read()


# ============================ host-side /etc checks ============================

def fat_read(fs, name):
    ent = fs['entries'].get(name)
    if not ent:
        return None
    return fs['read_chain'](ent[0], ent[1])


def parse_passwd(text):
    rows = []
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        f = line.split(':')
        if len(f) != 7:
            raise ValueError('passwd line is not 7 fields: %r' % line)
        rows.append({'name': f[0], 'passwd': f[1], 'uid': int(f[2]), 'gid': int(f[3]),
                     'gecos': f[4], 'home': f[5], 'shell': f[6], 'raw': line})
    return rows


def parse_shadow(text):
    rows = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        f = line.split(':')
        if len(f) < 2:
            raise ValueError('shadow line has no password field: %r' % line)
        rows[f[0]] = {'spec': f[1], 'flags': f[2] if len(f) > 2 else '', 'raw': line}
    return rows


def check_pbkdf2(spec, password):
    """Recompute the documented hash format on the host, return (ok, detail)."""
    m = re.match(r'^\$sutureos\$pbkdf2-sha256\$(\d+)\$([0-9a-f]{32})\$([0-9a-f]{64})$',
                 spec)
    if not m:
        return False, 'format mismatch: %r' % spec[:40]
    iters = int(m.group(1))
    salt = bytes.fromhex(m.group(2))
    want = bytes.fromhex(m.group(3))
    got = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, iters, 32)
    return got == want, 'iters=%d salt=%s hash=%s' % (iters, m.group(2)[:8], m.group(3)[:16])


def main():
    log = run_qemu()

    checks = []

    def add(name, cond, detail=''):
        checks.append((name, bool(cond), detail))

    # ---------------- guest transcript: regression ------------------
    add('boot self-test regression 13/14',
        '[stitch] selftests: 13/14 passed' in log)
    add('the sudo/creds/userdb self-test still passes (sudotest)',
        'sudo selftest: RESULT PASS' in log)
    add('FAT32 self-tests still pass',
        '[fat32] write self-test PASS' in log and '[fat32] ops self-test PASS' in log and
        '[fat32] lfn-write self-test PASS' in log)

    # ---------------- guest transcript: accounts -------------------
    add('users lists the seeded accounts',
        '2 accounts in /etc/passwd' in log and
        'root uid=0 gid=0 groups=root password=password current' in log and
        'user uid=1000 gid=1000 groups=wheel,user password=password' in log)
    add('useradd alice allocates the next uid (1001)',
        'useradd: created alice uid=1001 gid=1001 home=/home/alice password=password' in log)
    add('users shows alice with uid >= 1000 and a password',
        re.search(r'^alice uid=(\d+) gid=(\d+) groups=alice password=password',
                  log, re.M) is not None and
        int(re.search(r'^alice uid=(\d+)', log, re.M).group(1)) >= 1000)
    add('/etc/passwd line format for alice',
        'alice:x:1001:1001:alice:/home/alice:/bin/sh' in log)
    add('/etc/shadow shows the documented hash format for alice',
        re.search(r'alice:\$sutureos\$pbkdf2-sha256\$2000\$[0-9a-f]{32}\$[0-9a-f]{64}',
                  log) is not None)
    add('useradd with an empty password leaves the account locked',
        'useradd: created carol uid=1002 gid=1002 home=/home/carol password=locked' in log)
    add('a locked account cannot su (root keeps its identity)',
        'su: account carol is locked' in log)
    add('passwd -d makes the account passwordless',
        re.search(r'^carol uid=1002 gid=1002 groups=carol password=none', log, re.M)
        is not None)
    add('passwordless su works without a prompt',
        'su: switched to carol (uid=1002)' in log and
        'carol@sutureos$ whoami' in log)
    add('whoami reports carol after the passwordless switch',
        re.search(r'carol@sutureos\$ whoami\s*\ncarol\s*\n', log) is not None)
    add('id prints the new uid/gid/groups',
        'uid=1002(carol) gid=1002(carol) groups=1002(carol)' in log)
    add('a wrong su password is refused',
        'su: Authentication failure' in log)
    add('a failed su keeps the previous identity',
        re.search(r'carol@sutureos\$ whoami\s*\ncarol\s*\ncarol@sutureos\$ login alice',
                  log) is not None)
    add('login with a wrong password is refused and keeps the session',
        'Login incorrect' in log)
    add('sudo -l denies an account with no sudoers rule',
        re.search(r'carol@sutureos\$ sudo -l\s*\ncarol is not in the sudoers file', log)
        is not None)
    add('a sudo command is denied before any password prompt',
        re.search(r'carol@sutureos\$ sudo whoami\s*\ncarol is not in the sudoers file',
                  log) is not None)
    add('a normal user cannot write /etc/passwd',
        'write: /etc/passwd: Permission denied' in log)
    add('a normal user cannot remove /etc/shadow',
        'rm: /etc/shadow: Permission denied' in log)
    add('useradd is root-only',
        'useradd: permission denied (root only)' in log)
    add('a normal user can still READ /etc/passwd (cp worked)',
        'cp: /etc/passwd -> /mnt/fat/CAROL.TXT' in log)
    add('sudo -l ALLOWS a wheel member (sudoers really is consulted)',
        'User user may run the following commands on' in log)
    add('sudo -l prompts for the wheel member password and then lists the rules',
        re.search(r'user@sutureos\$ sudo -l\s*\n\[sudo\] password for user:', log)
        is not None)
    add('sudo runs the command as root, then the ticket avoids a second prompt',
        re.search(r'user@sutureos\$ sudo whoami\s*\nroot\s*\n', log) is not None and
        re.search(r'user@sutureos\$ whoami\s*\nuser\s*\n', log) is not None)
    add('su - with the root password starts a root login shell',
        'su: login shell for root, cwd /root' in log)
    add('root may su to another user without a password',
        'su: switched to alice (uid=1001)' in log)
    add('self-service passwd requires and checks the current password',
        'passwd: Authentication failure' in log)
    add('self-service passwd updates the hash with the right current password',
        'passwd: password updated successfully for alice' in log)
    add('the NEW password works for su from another account',
        re.search(r'carol@sutureos\$ su alice\s*\nPassword:\s*\n.*?'
                  r'\[login\] session identity = alice \(uid=1001\).*?'
                  r'alice@sutureos\$ whoami\s*\nalice',
                  log, re.S) is not None)
    add('userdel removes alice', 'userdel: removed alice' in log)
    add('userdel refuses to remove root', 'userdel: refusing to remove root' in log)
    add('userdel can remove a passwordless account', 'userdel: removed carol' in log)

    tail = log[log.rindex('userdel: removed alice'):] if 'userdel: removed alice' in log else log
    add('after userdel the account list and /etc/shadow have no alice',
        'alice uid=' not in tail and 'alice:x:' not in tail and 'alice:$' not in tail)

    # ---------------- host-side: parse the copied /etc files ------------
    try:
        sys.path.insert(0, os.path.join(ROOT, 'scripts'))
        import verify_fat32_write as vf
        img = open(DISK, 'rb').read()
        fs = vf.parse(img)

        def text_of(name):
            data = fat_read(fs, name)
            return None if data is None else data.decode('utf-8', errors='replace')

        pw1, sh1, gr1 = text_of('PW1.TXT'), text_of('SH1.TXT'), text_of('GR1.TXT')
        sh0 = text_of('SH0.TXT')
        pw2, sh2 = text_of('PW2.TXT'), text_of('SH2.TXT')
        carol_cp = text_of('CAROL.TXT')

        add('host: the guest copied /etc/passwd+shadow+group to FAT32 (three snapshots)',
            all(x is not None for x in (pw1, sh0, sh1, gr1, pw2, sh2)))

        rows1 = parse_passwd(pw1 or '')
        names1 = [r['name'] for r in rows1]
        add('host: /etc/passwd is well formed (7 fields per line)', True,
            '%d entries: %s' % (len(rows1), ','.join(names1)))
        add('host: root/user/alice/carol present after the two useradds',
            all(n in names1 for n in ('root', 'user', 'alice', 'carol')))
        alice = [r for r in rows1 if r['name'] == 'alice']
        add('host: alice uid >= 1000, unique, equal to its private gid',
            bool(alice) and alice[0]['uid'] >= 1000 and
            len({r['uid'] for r in rows1}) == len(rows1) and
            alice[0]['uid'] == alice[0]['gid'])
        add('host: the passwd field is "x" (hash lives in /etc/shadow)',
            all(r['passwd'] == 'x' for r in rows1))
        add('host: home/shell columns are absolute paths',
            all(r['home'].startswith('/') and r['shell'].startswith('/') for r in rows1))

        sh1_rows = parse_shadow(sh1 or '')
        add('host: /etc/shadow snapshot 1 has root/user/alice/carol',
            all(n in sh1_rows for n in ('root', 'user', 'alice', 'carol')))
        if 'alice' in sh1_rows:
            ok, detail = check_pbkdf2(sh1_rows['alice']['spec'], ALICE_PW)
            add('host: alice hash == PBKDF2(passphrase) recomputed on the host', ok, detail)
        else:
            add('host: alice hash == PBKDF2(passphrase) recomputed on the host', False)
        if 'root' in sh1_rows:
            ok, detail = check_pbkdf2(sh1_rows['root']['spec'], 'root')
            add('host: root hash still verifies (rewrite kept it intact)', ok, detail)
        else:
            add('host: root hash still verifies (rewrite kept it intact)', False)
        sh0_rows = parse_shadow(sh0 or '')
        add('host: useradd with an empty password wrote a LOCKED shadow entry',
            'carol' in sh0_rows and sh0_rows['carol']['spec'].startswith('!'),
            repr(sh0_rows.get('carol', {}).get('spec')))
        add('host: passwd -d wrote the EMPTY shadow field (passwordless, documented)',
            'carol' in sh1_rows and sh1_rows['carol']['spec'] == '',
            repr(sh1_rows.get('carol', {}).get('spec')))

        gr1_rows = [l.split(':') for l in (gr1 or '').splitlines()
                    if l and not l.startswith('#')]
        gr1_names = {f[0]: f for f in gr1_rows if len(f) >= 4}
        add('host: /etc/group has root/wheel/user/alice/carol',
            all(n in gr1_names for n in ('root', 'wheel', 'user', 'alice', 'carol')),
            ','.join(gr1_names))
        add('host: alice group gid matches the passwd gid',
            'alice' in gr1_names and bool(alice) and
            int(gr1_names['alice'][2]) == alice[0]['gid'])
        add('host: user is still a wheel member',
            'wheel' in gr1_names and 'user' in gr1_names['wheel'][3].split(','))

        rows2 = parse_passwd(pw2 or '')
        names2 = [r['name'] for r in rows2]
        sh2_rows = parse_shadow(sh2 or '')
        add('host: after both userdels alice and carol are gone from /etc/passwd',
            'alice' not in names2 and 'carol' not in names2, ','.join(names2))
        add('host: after both userdels alice and carol are gone from /etc/shadow',
            'alice' not in sh2_rows and 'carol' not in sh2_rows,
            ','.join(sorted(sh2_rows)))
        add('host: root and user survived the account churn',
            'root' in names2 and 'user' in names2)
        if 'user' in sh2_rows:
            ok, detail = check_pbkdf2(sh2_rows['user']['spec'], 'user')
            add('host: user hash still verifies after four rewrites', ok, detail)
        else:
            add('host: user hash still verifies after four rewrites', False)
        add('host: the copy taken BY the normal user equals /etc/passwd (read allowed)',
            carol_cp is not None and
            [r['name'] for r in parse_passwd(carol_cp)] == names1)
    except Exception as exc:                       # noqa: BLE001
        add('host-side FAT32 /etc verification', False, repr(exc))

    # ---------------- report ----------------
    print('')
    print('--- checks ---')
    nfail = 0
    for item in checks:
        name, cond = item[0], item[1]
        detail = ('  (%s)' % item[2]) if len(item) > 2 and item[2] else ''
        print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, detail))
        if not cond:
            nfail += 1
    nok = len(checks) - nfail
    print('')
    print('RESULT: %s (checks=%d ok=%d fail=%d)'
          % ('PASS' if nfail == 0 else 'FAIL', len(checks), nok, nfail))
    if nfail:
        print('')
        print('--- serial log tail (first failure triage) ---')
        for line in log.splitlines()[-100:]:
            print('  ' + line)
    return 0 if nfail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
