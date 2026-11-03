# SutureOS multiuser support (v0.31.0 batch)

Accounts, passwords, `login`/`su`, and a GUI **Switch User** dialog on top of the
existing policy layer (`kernel/security/userdb.c`, `creds.c`, `sudo.c`) plus the
new session layer (`kernel/security/login.c`).

> Honest boundary: every task still runs in ring 0 (see the note in
> `kernel/security/creds.h`). The multiuser layer implements *policy* — who the
> session claims to be and what that identity is allowed to do — it is not a
> hardware-enforced security boundary. There is no per-file ownership in the VFS
> yet, so the `/etc` and `/root` write rule is enforced at the shell command
> layer (documented below), not by the filesystem.

## Password states (the "empty password" decision)

`/etc/shadow` is the single source of truth. Its password field has three
explicit states — no fourth "accidental" state exists:

| shadow field | state | meaning |
|---|---|---|
| `$sutureos$pbkdf2-sha256$<iters>$<salt>$<hash>` | `password` | a password is required; `login`, `su` and the GUI prompt for it |
| *(empty)* — line looks like `name::0` | `none` | **no password required**: `login`, `su` and the GUI accept the account without prompting. This is the state an installer writes when it leaves the root password empty. It is *not* "any password works": `sudo` still denies it, because there is no password to prove. |
| `!` (or `*`) — line looks like `name:!:0` | `locked` | every password login is refused for every identity. `useradd` leaves the account in this state when no password is entered ("留空=禁用密码"). |
| no shadow line at all | `locked` | treated like a corrupt/missing entry |

Rationale: an empty field that means "no password" is a documented, testable
state (`userdb_pwstate()`), so a passwordless root console is a deliberate
installer choice instead of a hole that any string happens to satisfy. The
alternative reading — "empty = disabled" — would make the installer's blank root
password unloggable, so it is *not* used for the empty field; disabling is
spelled `!` instead.

## Commands

| command | who | behaviour |
|---|---|---|
| `users` | everyone | lists `name uid gid groups password=<state>`, current account marked `current` |
| `useradd <name>` | root | validates the name (`[a-z][a-z0-9_-]*`, ≤31, reserved words refused), allocates the lowest free uid ≥ 1000, creates a private group + `/home/<name>`, prompts for a password (empty ⇒ locked), rewrites `/etc/passwd`, `/etc/shadow`, `/etc/group` |
| `userdel <name>` | root | refuses `root` and the current session's user; removes the account, its private group and its supplementary memberships; keeps the home directory |
| `passwd [-l\|-u\|-d] [user]` | self / root | normal users may only change their own password and must first prove the current one; root may change anyone's. `-l` lock, `-u` unlock, `-d` delete password ⇒ passwordless |
| `login [user]` | everyone | authenticates and adopts the identity; failed authentication never changes the current identity |
| `su [-] [user]` | everyone | same engine as `login`: root needs no password, a passwordless target needs none, a locked target is refused, `su -` also switches the working directory to the target's home |
| `sudo …` | per `/etc/sudoers` | unchanged; the policy check runs before any password prompt, so an unknown account is denied without asking |

`useradd`/`userdel`/`passwd -l/-u/-d` are root-only. A failed `login`/`su`
never half-switches: the prompt and credentials stay on the previous identity.

## Isolation (policy, enforced today)

* `mount`/`umount`/`shutdown`/`reboot`/`kill` already require euid 0.
* The shell refuses `write`/`append`/`rm`/`mkdir`/`rmdir`/`touch`/`mv` (and the
  destination of `cp`) under `/etc` and `/root` for non-root sessions:
  `Permission denied (only root may modify /etc or /root)`. Reads stay open, so
  `cat /etc/passwd` and `cp /etc/passwd /mnt/fat/...` work for normal users.
* `sudo -l` prints the rules that apply to the *current* user; an account with
  no rule gets `… is not in the sudoers file`.

## GUI "Switch User" (lazily created)

Start menu → **Switch User**. The dialog is self-drawn in `kernel/gui/apps.c`
(like Image Viewer / Text Editor) and only created when selected, so the frozen
default desktop stays pixel-identical (proven by
`scripts/gui_appearance_check.py` inside `scripts/switch_user_acceptance.py`).

Keys: type the account name, **Enter**, type the password, **Enter**. The
compositor owns Tab (window focus) and Esc (quit desktop), so the dialog uses
Enter to advance instead of Tab.

## API for the taskbar / other consumers

```c
#include "login.h"
const char *login_current_user(void);   /* "-" when the uid has no account   */
uint32_t    login_current_uid(void);
const char *userdb_pwstate_text(const char *name);  /* password/none/locked */
```

The GUI session identity changes only through `login_switch()`, which also
refreshes the shell prompt and drops the cached sudo ticket, so a taskbar label
can simply render `login_current_user()` every frame.

## Acceptance

```
py -3 scripts/build.py
py -3 scripts/build_iso.py
py -3 scripts/mkfat32_testimg.py
py -3 scripts/multiuser_acceptance.py     # shell/account/isolation checks + host /etc parse
py -3 scripts/switch_user_acceptance.py   # GUI dialog + default-desktop appearance gate
```

`multiuser_acceptance.py` types a real command sequence over QMP, then parses the
FAT32 image the guest wrote and **recomputes PBKDF2-HMAC-SHA256 on the host** from
the salt/iterations in `/etc/shadow` — so the stored hash is verified to belong to
the password that was typed, not merely to look plausible.

## Known limitations

* No PAM, no `/etc/login.defs`, no password aging/lockout policy, no groups
  command (`groupadd`/`usermod`), no `userdel -r` (home directories are kept).
* Salts come from `userdb_new_salt()` (TSC/counter mixed through SHA-256), not a
  CSPRNG.
* No per-file ownership/permission bits in the VFS; isolation is the shell policy
  above plus the ring-0 boundary note.
* The GUI dialog is keyboard-only; there is no click-to-focus-field or caret
  placement inside the text boxes.
* `sudo` denies passwordless accounts (nothing to prove) — intentional, documented
  above.
