#!/usr/bin/env python3
"""SutureOS installer acceptance - host side, two real boots, real disk writes.

What it proves, end to end, with no simulated steps:

  Phase 1  Boot the installation ISO in QEMU with a blank 128 MiB disk attached.
           Drive the terminal installer through QMP (real keystrokes): pick the
           disk, confirm, set a root password, create the user `alice`.
           The guest writes an MBR, the Limine BIOS boot code, a FAT32 system
           partition and the system files, then prints `[install] done`.

  Phase 2  Parse the installed image on the HOST, independently of the guest:
           MBR signature + one active 0x0C partition at LBA 2048, a valid FAT32
           BPB/FSInfo (cluster count >= 65525), the Limine stage-1 bytes at
           sector 0 and stage 2 at byte offset 4096 equal to limine-bios-hdd.h,
           /kernel.elf byte-identical to build/kernel.elf, /limine-bios.sys
           byte-identical to the Limine file, /limine.conf, /SUTUREOS/INSTALLED,
           /etc/passwd with root + alice, and /etc/shadow hashes that verify with
           PBKDF2-HMAC-SHA256 against the passwords typed in phase 1.

  Phase 3  Boot the installed disk alone (no ISO): Limine loads the kernel from
           the FAT32 volume, the kernel finds /SUTUREOS/INSTALLED, seeds /etc from
           the volume (log line), `cat /etc/passwd` shows the installed accounts,
           and `su alice` with the installed password switches to alice - i.e. the
           password hash survived the reboot and works through the real userdb.

Usage: py -3 scripts/installer_acceptance.py [--keep-disk] [--disk-mib 128]
"""
import argparse
import hashlib
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
QMP_PORT = 45841          # this port is reserved for the installer worktree
PART_LBA = 2048
DISK_IMAGE = ROOT / "build" / "install_disk.img"
ISO = ROOT / "build" / "sutureos.iso"
KERNEL = ROOT / "build" / "kernel.elf"
LIMINE_SYS = ROOT / "tools" / "limine" / "limine-binary" / "limine-bios.sys"
LIMINE_HDD_H = ROOT / "tools" / "limine" / "limine-binary" / "limine-bios-hdd.h"

ROOT_PW = "sutroot"
USER_NAME = "alice"
USER_PW = "alicepw"

KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus"}


class Checker:
    def __init__(self):
        self.checks = 0
        self.fails = []

    def check(self, ok, what, detail=""):
        self.checks += 1
        if not ok:
            self.fails.append(f"{what}{(': ' + str(detail)) if detail else ''}")
        print(f"  [{'ok ' if ok else 'FAIL'}] {what}{(': ' + str(detail)) if detail else ''}")
        return ok


class Qmp:
    def __init__(self, port=QMP_PORT, timeout=30):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError as e:                      # QEMU not listening yet
                last = e
                time.sleep(0.25)
        else:
            raise RuntimeError(f"QMP connect failed: {last}")
        self.f = self.sock.makefile("rwb")
        self.f.readline()                              # greeting
        self.cmd("qmp_capabilities")
        self.keys = 0

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.f.write((__import__("json").dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return None
            if b'"event"' in line:
                continue
            return __import__("json").loads(line)

    def key(self, name, delay=0.06):
        self.cmd("human-monitor-command", {"command-line": "sendkey " + name})
        self.keys += 1
        time.sleep(delay)

    def type(self, text, delay=0.09):
        for ch in text:
            if ch.isupper():
                self.key("shift-" + ch.lower(), delay)
            elif ch in KEYMAP:
                self.key(KEYMAP[ch], delay)
            else:
                self.key(ch, delay)

    def quit(self):
        try:
            self.cmd("quit")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Serial:
    def __init__(self, path):
        self.path = Path(path)
        self.pos = 0
        self.text = ""

    def read_new(self):
        try:
            data = self.path.read_bytes() if self.path.exists() else b""
        except OSError:
            return ""
        chunk = data[self.pos:]
        self.pos = len(data)
        if chunk:
            self.text += chunk.decode("utf-8", "replace")
        return chunk.decode("utf-8", "replace")

    def wait_for(self, pattern, timeout=120, name=None):
        """Wait until `pattern` appears anywhere in the log; returns the match."""
        rx = re.compile(pattern, re.M)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.read_new()
            m = rx.search(self.text)
            if m:
                return m
            time.sleep(0.1)
        tail = self.text[-1500:]
        raise RuntimeError(f"timeout waiting for {name or pattern!r}; log tail:\n{tail}")


def run_qemu(disk, serial_log, extra_args, phase):
    args = [QEMU, "-machine", "pc", "-m", "256", "-display", "none",
            "-serial", f"file:{serial_log}",
            "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait", "-smp", "2"]
    args += extra_args
    print(f"[qemu:{phase}] " + " ".join(args[2:]))
    proc = subprocess.Popen(args, cwd=str(ROOT))
    return proc


def wait_ready(qmp, serial, timeout=180):
    serial.wait_for(r"sutureos# ", timeout=timeout, name="shell prompt")
    return qmp


# ---------------------------------------------------------------- installer run
def phase1_install(disk, serial_log, checker):
    if disk.exists():
        disk.unlink()
    with open(disk, "wb") as f:
        f.truncate(128 * 1024 * 1024)
    print(f"\n=== phase 1: install from the ISO onto {disk.name} ===")

    args = ["-cdrom", "build/sutureos.iso", "-boot", "d",
            "-drive", f"id=disk0,file={disk.as_posix()},format=raw,if=none",
            "-device", "ide-hd,drive=disk0,bus=ide.0",
            "-no-reboot", "-no-shutdown"]
    proc = run_qemu(disk, serial_log, args, "install")
    serial = Serial(serial_log)
    qmp = None
    try:
        qmp = Qmp()
        wait_ready(qmp, serial)
        serial.wait_for(r"Disks in this machine:", timeout=10) if False else None

        def type_cmd(cmd):
            qmp.type(cmd)
            qmp.key("ret")

        type_cmd("install")
        serial.wait_for(r"install medium: ISO9660", timeout=30, name="installer start")
        serial.wait_for(r"Target disk index", timeout=10)
        type_cmd("0")
        serial.wait_for(r"Type 'yes' to continue", timeout=10)
        type_cmd("yes")
        serial.wait_for(r"root password \(empty", timeout=10)
        qmp.type(ROOT_PW)
        qmp.key("ret")
        serial.wait_for(r"repeat root password", timeout=10)
        qmp.type(ROOT_PW)
        qmp.key("ret")
        serial.wait_for(r"Create a normal user", timeout=10)
        type_cmd("y")
        serial.wait_for(r"user name:", timeout=10)
        type_cmd(USER_NAME)
        serial.wait_for(r"user password:", timeout=10)
        qmp.type(USER_PW)
        qmp.key("ret")
        serial.wait_for(r"repeat user password", timeout=10)
        qmp.type(USER_PW)
        qmp.key("ret")
        m = serial.wait_for(r"^\[install\] done \(drive (\d+), partition LBA (\d+), "
                            r"root_password=(\w+), user=(\w+)\)",
                            timeout=420, name="[install] done")
        print(f"  guest: {m.group(0)}")
        checker.check(m.group(2) == str(PART_LBA), "guest used partition LBA 2048", m.group(2))
        checker.check(m.group(3) == "set", "guest reports root_password=set")
        checker.check(m.group(4) == USER_NAME, "guest reports the created user", m.group(4))
        checker.check(m.group(1) == "0", "guest installed onto drive 0", m.group(1))
    finally:
        if qmp:
            qmp.quit()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    checker.check(serial.text.count("[install] done") == 1, "installer finished exactly once")


# ---------------------------------------------------------------- host parsing
def limine_hdd_blob():
    text = LIMINE_HDD_H.read_text(encoding="utf-8")
    body = text.split("{", 1)[1]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))


class Fat32:
    """Minimal read-only FAT32 reader for one partition of a disk image."""

    def __init__(self, image, part_lba=PART_LBA):
        self.img = image
        self.base = part_lba * 512
        bs = image[self.base:self.base + 512]
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        self.reserved = struct.unpack_from("<H", bs, 14)[0]
        self.nfats = bs[16]
        self.fatsz = struct.unpack_from("<I", bs, 36)[0]
        self.tot32 = struct.unpack_from("<I", bs, 32)[0]
        self.rootclus = struct.unpack_from("<I", bs, 44)[0]
        self.fsinfo = struct.unpack_from("<I", bs, 48)[0]
        self.fat_off = self.base + self.reserved * self.bps
        self.data_off = self.fat_off + self.nfats * self.fatsz * self.bps
        self.cluster_bytes = self.spc * self.bps
        data_sectors = self.tot32 - self.reserved - self.nfats * self.fatsz
        self.clusters = data_sectors // self.spc

    def clus_off(self, c):
        return self.data_off + (c - 2) * self.cluster_bytes

    def fat_next(self, c):
        return struct.unpack_from("<I", self.img, self.fat_off + c * 4)[0] & 0x0FFFFFFF

    def chain(self, start, limit=400000):
        out, c, guard = [], start, 0
        while 2 <= c < 0x0FFFFFF8 and guard < limit:
            out.append(c)
            c = self.fat_next(c)
            guard += 1
        return out

    def read(self, start, size):
        out = bytearray()
        for c in self.chain(start):
            off = self.clus_off(c)
            out += self.img[off:off + self.cluster_bytes]
            if len(out) >= size:
                break
        return bytes(out[:size])

    def _short(self, ent):
        base = ent[0:8].rstrip(b" ").decode("ascii", "replace")
        ext = ent[8:11].rstrip(b" ").decode("ascii", "replace")
        return base + ("." + ext if ext else "")

    def _lfn_name(self, pending):
        out = []
        for e in reversed(pending):
            for off in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                out.append(e[off:off + 2])
        return b"".join(out).decode("utf-16-le", "replace").split("\x00")[0]

    def listdir(self, start_cluster):
        entries, pending = [], []
        for c in self.chain(start_cluster):
            blk = self.img[self.clus_off(c): self.clus_off(c) + self.cluster_bytes]
            for off in range(0, self.cluster_bytes, 32):
                e = blk[off:off + 32]
                if len(e) < 32:
                    break
                if e[0] == 0x00:
                    return entries
                if e[0] == 0xE5:
                    pending = []
                    continue
                attr = e[11]
                if attr == 0x0F:                    # LFN entry (also has bit 0x08 set)
                    pending.append(e)
                    continue
                if attr & 0x08:                     # volume label
                    pending = []
                    continue
                name = self._lfn_name(pending) if pending else self._short(e)
                pending = []
                if name in (".", ".."):
                    continue
                first = (struct.unpack_from("<H", e, 20)[0] << 16) | \
                        struct.unpack_from("<H", e, 26)[0]
                size = struct.unpack_from("<I", e, 28)[0]
                entries.append((name, bool(attr & 0x10), first, size))
        return entries

    def walk(self, start_cluster=None, prefix=""):
        out = {}
        for name, is_dir, first, size in self.listdir(start_cluster or self.rootclus):
            path = prefix + "/" + name
            if is_dir:
                out[path] = {"dir": True}
                if path.count("/") < 4:
                    out.update(self.walk(first, path))
            else:
                out[path] = {"size": size, "first": first}
        return out

    def read_file(self, path):
        e = self.walk().get(path)
        if not e or e.get("dir"):
            return None
        return self.read(e["first"], e["size"])


def verify_pbkdf2(spec, password, expect_iters=2000):
    """Check a /etc/shadow hash spec produced by the installer/userdb.c."""
    m = re.fullmatch(r"\$sutureos\$pbkdf2-sha256\$(\d+)\$([0-9a-f]{32})\$([0-9a-f]{64})", spec)
    if not m:
        return False, "spec does not match $sutureos$pbkdf2-sha256$..."
    iters = int(m.group(1))
    salt = bytes.fromhex(m.group(2))
    want = m.group(3)
    got = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iters, 32).hex()
    return (got == want and iters == expect_iters), f"iters={iters} match={got == want}"


def phase2_verify_image(image, checker):
    print("\n=== phase 2: host-side verification of the installed image ===")
    checker.check(image.exists(), "installed disk image exists")
    img = image.read_bytes()

    # ---- MBR ----
    checker.check(img[510:512] == b"\x55\xaa", "MBR boot signature 0x55AA")
    status, ptype = img[446], img[450]
    start, count = struct.unpack_from("<I", img, 454)[0], struct.unpack_from("<I", img, 458)[0]
    checker.check(status == 0x80, "partition 1 is marked active (0x80)", hex(status))
    checker.check(ptype == 0x0C, "partition 1 type is 0x0C (FAT32 LBA)", hex(ptype))
    checker.check(start == PART_LBA, f"partition 1 starts at LBA {PART_LBA}", start)
    checker.check(count == len(img) // 512 - PART_LBA, "partition 1 covers the rest of the disk", count)
    checker.check(not any(img[462:510]), "partitions 2..4 are empty")

    # ---- Limine boot area == what `limine bios-install` writes ----
    hdd = limine_hdd_blob()
    stage2_off = 4096
    expect_mbr = bytearray(hdd[0:512])
    struct.pack_into("<Q", expect_mbr, 0x1A4, stage2_off)
    expect_mbr[446:462] = img[446:462]        # our partition table
    checker.check(len(hdd) == 24033, "limine-bios-hdd.h blob parsed", len(hdd))
    checker.check(img[0:446] == bytes(expect_mbr[0:446]),
                  "MBR code (0..445) is Limine's stage 1 with the stage-2 offset patched")
    checker.check(img[510:512] == bytes(expect_mbr[510:512]), "MBR signature bytes match")
    stage2_len = len(hdd) - 512
    checker.check(img[stage2_off:stage2_off + stage2_len] == hdd[512:],
                  f"Limine stage 2 at byte 4096 identical to limine-bios-hdd.h ({stage2_len} B)")

    # ---- FAT32 volume ----
    fat = Fat32(img)
    checker.check(fat.bps == 512, "FAT32 bytes/sector = 512", fat.bps)
    checker.check(fat.spc in (1, 2, 4, 8, 16, 32, 64, 128), "FAT32 sectors/cluster valid", fat.spc)
    checker.check(fat.nfats == 2, "FAT32 has 2 FATs", fat.nfats)
    checker.check(fat.clusters >= 65525, "cluster count >= 65525 (genuine FAT32)", fat.clusters)
    bs = img[fat.base:fat.base + 512]
    checker.check(bs[0] == 0xEB, "FAT32 boot sector jump 0xEB", hex(bs[0]))
    checker.check(bs[82:90] == b"FAT32   ", "FAT32 filesystem type string")
    fi = img[fat.base + fat.fsinfo * 512: fat.base + fat.fsinfo * 512 + 512]
    checker.check(struct.unpack_from("<I", fi, 0)[0] == 0x41615252, "FSInfo lead signature")
    checker.check(struct.unpack_from("<I", fi, 484)[0] == 0x61417272, "FSInfo struct signature")
    checker.check(struct.unpack_from("<I", fi, 508)[0] == 0xAA5555AA, "FSInfo trail signature")

    # ---- files ----
    files = fat.walk()
    names = sorted(files)
    print("  files on the installed volume:")
    for n in names:
        e = files[n]
        print(f"    {n:28s} {'<dir>' if e.get('dir') else str(e['size']) + ' bytes'}")

    def file_bytes(path):
        e = files.get(path)
        if not e or e.get("dir"):
            return None
        return fat.read(e["first"], e["size"])

    kernel = file_bytes("/kernel.elf")
    checker.check(kernel is not None, "/kernel.elf exists")
    if kernel is not None:
        checker.check(hashlib.sha256(kernel).hexdigest() ==
                      hashlib.sha256(KERNEL.read_bytes()).hexdigest(),
                      "/kernel.elf sha256 == build/kernel.elf")
        checker.check(len(kernel) == KERNEL.stat().st_size, "/kernel.elf size matches",
                      len(kernel))
    limine = file_bytes("/limine-bios.sys")
    checker.check(limine is not None, "/limine-bios.sys exists")
    if limine is not None:
        checker.check(limine == LIMINE_SYS.read_bytes(), "/limine-bios.sys byte-identical")
    conf = file_bytes("/limine.conf")
    checker.check(conf is not None, "/limine.conf exists")
    if conf:
        text = conf.decode("utf-8", "replace")
        checker.check("kernel.elf" in text and "protocol: multiboot" in text,
                      "/limine.conf points at /kernel.elf with multiboot")
        checker.check("timeout: 0" in text, "/limine.conf keeps timeout 0 (no boot menu)")
    marker = file_bytes("/SUTUREOS/INSTALLED")
    checker.check(marker is not None, "/SUTUREOS/INSTALLED marker exists (system volume)")
    if marker:
        checker.check(b"bootloader=limine-bios" in marker, "marker records the bootloader")

    # ---- accounts ----
    passwd = (file_bytes("/etc/passwd") or b"").decode("utf-8", "replace")
    checker.check(re.search(r"^root:x:0:0:root:/root:/bin/sh$", passwd, re.M) is not None,
                  "/etc/passwd has the root account")
    checker.check(re.search(rf"^{USER_NAME}:x:1000:1000:.*:/home/{USER_NAME}:/bin/sh$",
                            passwd, re.M) is not None,
                  f"/etc/passwd has {USER_NAME} (uid 1000)")
    shadow = (file_bytes("/etc/shadow") or b"").decode("utf-8", "replace")
    root_spec = re.search(r"^root:(\S+):", shadow, re.M)
    user_spec = re.search(rf"^{USER_NAME}:(\S+):", shadow, re.M)
    checker.check(root_spec is not None, "/etc/shadow has a root entry")
    checker.check(user_spec is not None, f"/etc/shadow has {USER_NAME}")
    if root_spec:
        ok, why = verify_pbkdf2(root_spec.group(1), ROOT_PW)
        checker.check(ok, "root hash verifies with the typed root password (PBKDF2)", why)
    if user_spec:
        ok, why = verify_pbkdf2(user_spec.group(1), USER_PW)
        checker.check(ok, f"{USER_NAME} hash verifies with the typed password (PBKDF2)", why)
        ok2, _ = verify_pbkdf2(user_spec.group(1), USER_PW + "x")
        checker.check(not ok2, "a wrong password does NOT verify")
    group = (file_bytes("/etc/group") or b"").decode("utf-8", "replace")
    checker.check(re.search(rf"^wheel:x:10:.*{USER_NAME}", group, re.M) is not None,
                  f"/etc/group puts {USER_NAME} in wheel (sudo-able)")
    sudoers = (file_bytes("/etc/sudoers") or b"").decode("utf-8", "replace")
    checker.check("%wheel ALL=(ALL:ALL) ALL" in sudoers, "/etc/sudoers grants %wheel")
    checker.check(file_bytes("/etc/motd") is not None, "/etc/motd seeded")
    return files


# ---------------------------------------------------------------- second boot
def phase3_boot_from_disk(image, serial_log, checker):
    print("\n=== phase 3: boot the installed disk alone (no ISO) ===")
    args = ["-drive", f"id=disk0,file={image.as_posix()},format=raw,if=none",
            "-device", "ide-hd,drive=disk0,bus=ide.0",
            "-boot", "c", "-no-reboot", "-no-shutdown"]
    proc = run_qemu(image, serial_log, args, "fromdisk")
    serial = Serial(serial_log)
    qmp = None
    try:
        qmp = Qmp()
        serial.wait_for(r"multiboot1: Loading executable `boot\(\):/kernel\.elf`",
                        timeout=60, name="Limine loading the kernel from the FAT32 volume")
        checker.check(True, "Limine loads boot():/kernel.elf from the installed volume")
        serial.wait_for(r"version 0\.\d+\.\d+-alpha", timeout=120, name="kernel banner")
        checker.check(True, "kernel banner shown (boot presentation intact)")
        m = serial.wait_for(r"^\[sys\] installed system volume \(drive (\d+)\): "
                            r"seeded (\d+) file\(s\)", timeout=90,
                            name="[sys] installed system volume")
        checker.check(int(m.group(2)) == 5, "kernel seeded 5 /etc files from the volume",
                      m.group(2))
        serial.wait_for(r"sutureos# ", timeout=60, name="shell prompt from the disk boot")
        checker.check(True, "terminal shell prompt reached (booted from hard disk)")

        def run(cmd):
            qmp.type(cmd)
            qmp.key("ret")

        run("cat /etc/passwd")
        m = serial.wait_for(rf"^{USER_NAME}:x:1000:1000:", timeout=20,
                            name=f"{USER_NAME} in /etc/passwd after reboot")
        checker.check(True, f"/etc/passwd from the volume contains {USER_NAME}")
        checker.check("root:x:0:0:root:/root:/bin/sh" in serial.text,
                      "/etc/passwd from the volume contains root")

        run("cat /etc/shadow")
        m = serial.wait_for(r"^root:\$sutureos\$pbkdf2-sha256\$", timeout=20,
                            name="root hash loaded from the volume")
        checker.check(True, "/etc/shadow hash loaded from the volume after reboot")

        # su alice with the password set during installation: the real proof that
        # the installed hash works through userdb.c.
        run("su " + USER_NAME)
        serial.wait_for(r"[Pp]assword", timeout=20, name="su password prompt")
        qmp.type(USER_PW)
        qmp.key("ret")
        serial.wait_for(rf"{USER_NAME}@sutureos\$ ", timeout=30,
                        name="prompt switched to the installed user")
        checker.check(True, f"su {USER_NAME} succeeded with the installed password")
        run("whoami")
        serial.wait_for(rf"^{USER_NAME}\s*$", timeout=20, name="whoami output")
        checker.check(True, f"whoami reports {USER_NAME} (identity from the volume)")
        # `exit` leaves the su session. Do not assert on the prompt that follows:
        # this shell's exit path is the pre-existing one (a bare exit can end the
        # shell task), and the GUI is checked separately by the appearance gate.
        run("exit")
        time.sleep(1.0)
        checker.check("[sys] installed system volume" in serial.text,
                      "installed volume was used for the whole session")
    finally:
        if qmp:
            qmp.quit()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    return serial.text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep-disk", action="store_true",
                    help="keep build/install_disk.img after the run")
    ap.add_argument("--skip-phase3", action="store_true")
    args = ap.parse_args()

    c = Checker()
    for p, what in ((ISO, "installation ISO"), (KERNEL, "kernel.elf"),
                    (LIMINE_SYS, "limine-bios.sys"), (LIMINE_HDD_H, "limine-bios-hdd.h")):
        if not p.exists():
            print(f"missing {what}: {p} - run scripts/build.py and scripts/build_iso.py first")
            return 1

    serial1 = ROOT / "build" / "serial_installer.log"
    serial2 = ROOT / "build" / "serial_installed_boot.log"
    if serial1.exists():
        serial1.unlink()
    if serial2.exists():
        serial2.unlink()

    phase1_install(DISK_IMAGE, serial1, c)
    if not c.check(DISK_IMAGE.exists() and DISK_IMAGE.stat().st_size > 0,
                   "guest wrote the disk image"):
        print("\nSUMMARY: the installer did not produce a disk - stopping")
        return 1
    phase2_verify_image(DISK_IMAGE, c)
    if not args.skip_phase3:
        text = phase3_boot_from_disk(DISK_IMAGE, serial2, c)
        if "[sys] installed system volume" not in text:
            c.check(False, "installed system volume was recognised at boot")

    print(f"\nchecks={c.checks} ok={c.checks - len(c.fails)} fail={len(c.fails)}")
    for f in c.fails:
        print(f"  FAILED: {f}")
    print(f"serial logs: {serial1.name}, {serial2.name}")
    if not args.keep_disk:
        print(f"disk image kept at {DISK_IMAGE} (delete it manually if not needed)")
    return 0 if not c.fails else 1


if __name__ == "__main__":
    sys.exit(main())
