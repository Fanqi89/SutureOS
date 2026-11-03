# SutureOS installer (`install`)

The live ISO is an installation medium: from the terminal shell, `install` walks
through a small wizard and **really writes a system onto a hard disk** (MBR
partition table, Limine BIOS boot code, FAT32 system volume, system files). The
resulting disk boots on its own - no ISO, no host tool, no simulation.

Acceptance: `py -3 scripts/installer_acceptance.py` (boots the ISO, drives the
installer through QMP keystrokes, parses the installed image on the host, then
boots the disk alone and checks that the installed accounts and password hashes
survive the reboot). 56/56 checks.

## What the wizard does

```
=== SutureOS installer (terminal) ===
Disks in this machine:
  [0] drive 0  ata0  128 MiB  (262144 sectors)
Target disk index [q = quit]: 0
  !! EVERYTHING on this disk will be erased.
Type 'yes' to continue: yes
root password (empty = no password): ********
repeat root password: ********
Create a normal user? [y/N]: y
user name: alice
user password: ********
repeat user password: ********
[install] ... progress ...
[install] done (drive 0, partition LBA 2048, root_password=set, user=alice)
```

Safety:

* CD-ROMs, the ramdisk, read-only devices and disks smaller than 35 MiB are
  listed but can never be selected;
* nothing is written before the explicit `yes`;
* passwords are read without echo (`*` per character) and must be repeated.

## Disk layout

| where | contents |
|---|---|
| LBA 0 | MBR: Limine BIOS stage 1 + one partition entry (active, type `0x0C`) |
| byte 4096 | Limine BIOS stage 2 (`limine-bios-hdd.h` bytes 512.., 23 521 B) |
| LBA 2048 .. end | partition 1, FAT32, the system volume |

The 1 MiB gap before the partition is exactly where `limine bios-install` puts
stage 2, and the installer reproduces that write byte for byte: sector 0 is
`limine-bios-hdd.bin[0..511]`, the byte offset 4096 is patched in at `0x1A4`,
and the rest of the blob is written at byte 4096. `limine-bios.sys` (the file
Limine looks up on the boot partition) is copied from the ISO, and
`scripts/installer_acceptance.py` asserts the installed bytes equal the blob.

## Files on the system volume

```
/kernel.elf            the kernel, byte-identical to the live system's kernel
/limine-bios.sys       Limine stage 2 file (Limine needs it on the boot partition)
/limine.conf           boot entry: multiboot, path boot():/kernel.elf, timeout 0
/SUTUREOS/INSTALLED    marker: "this FAT32 volume is a SutureOS system"
/etc/passwd  /etc/shadow  /etc/group  /etc/sudoers  /etc/motd
```

`/limine.conf` keeps `timeout: 0` and `default_entry: 1`, so the boot presentation
is the same as the ISO: no Limine menu, kernel log first, then the banner and the
black terminal.

## Accounts and the shadow format

`/etc/shadow` uses the same format as the rest of the system (see
`kernel/security/userdb.h`):

```
name:$sutureos$pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>:<flags>
```

* `<iterations>` = 2000, `<salt-hex>` = 16 random-ish bytes (32 hex chars),
  `<hash-hex>` = PBKDF2-HMAC-SHA256(password, salt, iterations) (64 hex chars).
* **Empty password = `!` in the hash field** (Linux' locked-account marker):
  `root:!:0`. `userdb` parses that as "no usable hash", so `su`/`sudo` cannot
  authenticate until a password is set with `passwd root`. Choosing an empty
  password during installation therefore means "no password login for root", not
  "empty password accepted".
* The optional normal user is created with uid/gid 1000 and is added to the
  `wheel` group; `/etc/sudoers` grants `%wheel ALL=(ALL:ALL) ALL` (plus an
  explicit line for the user), so `sudo` works out of the box.

## Boot-time adoption of the system volume

`kernel/main/kmain.c` (after the ramfs root and the block layer are up):

1. `fatfs_mount_auto()` mounts the first FAT32 volume on each candidate disk -
   it tries the superfloppy layout (BPB at LBA 0, the historical/test-image
   layout) first and then the MBR partitions (`0x0B/0x0C/0x0E`), which is how a
   partitioned system volume is found;
2. if that volume carries `/SUTUREOS/INSTALLED`, the same volume is mounted a
   second time at `/mnt/sys` and `/etc/{passwd,shadow,group,sudoers,motd}` are
   copied into the ramfs `/etc`, then `userdb_init()`/`sudo_init()` re-parse
   them. Log line: `[sys] installed system volume (drive N): seeded 5 file(s)
   from /mnt/sys/etc` (one line, only on an installed system);
3. a plain data disk has no marker, so nothing is mounted twice and no extra log
   line appears - live ISO behaviour and boot presentation are unchanged.

## Implementation notes / limitations

* BIOS/MBR only: the installed disk boots through Limine's BIOS stages. There is
  no UEFI (`EFI System Partition`) or GRUB path on the installed disk, and no
  GPT support (a GPT disk would be overwritten with an MBR).
* One partition, FAT32 only, no LVM/encryption/swap, no package selection, no
  network install: `/kernel.elf` and `limine-bios.sys` are copied from the live
  ISO (`/boot/kernel.elf`, `/boot/limine/limine-bios.sys`). The ISO9660 driver
  reads the primary volume descriptor, so those two 8.3-mangled names
  (`KERNEL.ELF`, `LIMINE_B.SYS`) are what it looks for.
* Minimum disk: 35 MiB. The formatter picks the largest sensible cluster size
  (4 KiB, then 2 KiB/1 KiB/512 B) that still gives a genuine FAT32 volume
  (>= 65525 clusters), so small disks get 512-byte clusters.
* The terminal wizard is the only installer UI (no graphical installer); the
  frozen desktop, task bar and boot presentation are untouched.
* `install` can be re-run; it overwrites the MBR and the partition table and
  reformats the first partition. Nothing else on the disk is preserved.
* Files are written through the normal FAT32 write path. Two write-path
  optimisations were needed for a 2.5 MiB kernel on a 128 MiB volume and are
  documented in `kernel/fs/fatfs.c`: a per-file cluster cursor (the old code
  re-walked the FAT from the head for every 512-byte sector) and a full-cluster
  fast path that skips the read-modify-write. A pre-existing FAT32 bug was found
  and fixed on the way: the first-cluster field was parsed as 24 bits
  (`ent[26]` only), so every file whose first cluster was >= 256 read the wrong
  data - invisible on small test images, fatal for an installed system.
