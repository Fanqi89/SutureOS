# Persistent user data (`/SUTUREOS/PERSIST`)

`kernel/fs/persist.c` keeps the user's data on the installed system volume, so
`/etc`, `/home` and `/root` survive a reboot or a power-off. The ramfs is still
`/` (the boot presentation and the GUI do not change); the on-disk tree is a
**two-way mirror** of those three directories:

```
/SUTUREOS/PERSIST/VERSION        "sutureos-persist 1"  (format version)
/SUTUREOS/PERSIST/etc/*          passwd, shadow, group, sudoers, motd, ...
/SUTUREOS/PERSIST/home/<user>/*  each user's home
/SUTUREOS/PERSIST/root/*         root's files
```

## When it runs

| event | what happens |
|---|---|
| install (`install`) | installer creates the skeleton and seeds `PERSIST/etc` with the `/etc` it just wrote, then mounts the new volume at `/mnt/sys` so the rest of that live session is already persistent |
| boot of an installed system | `kmain` mounts the FAT32 system volume at `/mnt/sys`, `persist_detect()` sees it, `persist_restore()` copies `PERSIST/{etc,home,root}` into the ramfs, then `userdb_init()`/`sudo_init()` re-parse `/etc`. Log: `[persist] restored N file(s) from /mnt/sys/SUTUREOS/PERSIST` + `[sys] installed system volume (drive N): restored N file(s) from /mnt/sys/SUTUREOS/PERSIST` |
| `sync` | `persist_flush()` first (ramfs -> volume), then the normal per-mount sync |
| `shutdown` / `reboot` | `persist_flush()` + sync every mount **before** ACPI S5 / reset |
| leaving the GUI (`startgui` -> `exit`) | one `persist_flush()`, so editor/GUI writes are not lost |
| `persist` | status (volume, format version, restored/flushed counters) |
| `persist flush` | force a flush now |

`persist_flush()` ends with `vfs_sync("/mnt/sys")`, so the FAT32 FAT/FSInfo
updates reach the disk before the machine powers off - a shutdown right after
`sync` cannot lose data. Log line: `[persist] flush N files`.

## Compatibility / fallback

An installed system volume that has no `PERSIST` directory (written by the first
installer) still boots: `kmain` falls back to the old flat `/mnt/sys/etc` seeding
and the next flush creates the `PERSIST` skeleton. A live-ISO session with no
installed disk has no `/mnt/sys`, so `persist_detect()` returns 0 and every
trigger does nothing - the live system writes nothing and prints no extra lines.

## Honest limits

* The mirror is a copy: deleting a file in the ramfs does not delete it from the
  volume, so the next boot would bring it back. Only account/`/etc` edits and
  file *additions/overwrites* are reliably persistent.
* Bounded: depth 6, 64 entries per directory, 256 files per flush, 128 KiB per
  file. Anything bigger is skipped silently (the ramfs file limit is 256 KiB).
* The mirror holds no permissions/ownership metadata (the VFS has none); `/etc`
  protection is still the shell/cred policy layer, not a storage boundary.
* No journaling/atomic rename: a power loss in the middle of a flush can leave a
  half-written file. Shutdown/reboot flush first, so this needs a hard power cut.
* Only the first FAT32 system volume is used (`/mnt/sys`); a second disk is not
  mirrored.
* Verified with `scripts/persistence_acceptance.py` (32/32): install -> useradd
  bob -> write `/home/bob/NOTE.TXT` + `/etc/motd` -> `sync` -> `shutdown` -> host
  parses the volume (`PERSIST` files + PBKDF2 hash) -> boot the disk alone ->
  `[persist] restored 6 file(s)`, `/etc/motd` is the changed text, `users` lists
  bob, `cat /home/bob/NOTE.TXT` is intact, `su bob` works with the old password.
