#!/usr/bin/env python3
"""SutureOS data-persistence acceptance - two real boots, host verification.

Proves that user data survives a reboot/power-off of an installed system:

  Phase 1  Boot the installation ISO with a blank 256 MiB disk, install SutureOS
           through the terminal wizard (root password + no extra user), then in
           the same session: `useradd bob` (with a password), create
           /home/bob/NOTE.TXT, overwrite /etc/motd, run `sync` (which flushes the
           persistence mirror) and `shutdown` (which flushes again before powering
           off - QEMU must exit on its own).

  Phase 2  Parse the installed image on the HOST: the FAT32 system volume must
           carry SUTUREOS/PERSIST/VERSION, PERSIST/etc/{passwd,shadow,group,
           motd}, PERSIST/home/bob/NOTE.TXT; /etc/motd in the mirror must be the
           changed text and PERSIST/etc/shadow must have a bob hash that verifies
           with hashlib.pbkdf2_hmac.

  Phase 3  Boot the same disk again (no ISO). The kernel must restore the mirror
           (`[persist] restored N file(s)` / `[sys] ... restored N file(s)`),
           `cat /etc/motd` must show the changed text, `users` must list bob,
           `cat /home/bob/NOTE.TXT` must be the written text, and `su bob` with
           the installed password must switch the prompt to bob@sutureos$.

Usage: py -3 scripts/persistence_acceptance.py
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
QMP_PORT = 45861
PART_LBA = 2048
DISK_MIB = 256
DISK = ROOT / "build" / "persist_disk.img"
ISO = ROOT / "build" / "sutureos.iso"
KERNEL = ROOT / "build" / "kernel.elf"

ROOT_PW = "sutroot"
USER = "bob"
USER_PW = "bobpw"
NOTE_TEXT = "hello persistence"
MOTD_TEXT = "changed by user"

KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
          ":": "shift-semicolon", "=": "equal"}


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
    def __init__(self, port=QMP_PORT, timeout=40):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError as e:
                last = e
                time.sleep(0.25)
        else:
            raise RuntimeError(f"QMP connect failed: {last}")
        self.f = self.sock.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

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
        self.text += chunk.decode("utf-8", "replace")
        return chunk.decode("utf-8", "replace")

    def wait_for(self, pattern, timeout=120, name=None):
        rx = re.compile(pattern, re.M)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.read_new()
            m = rx.search(self.text)
            if m:
                return m
            time.sleep(0.1)
        raise RuntimeError(f"timeout waiting for {name or pattern!r}; tail:\n{self.text[-1500:]}")


def start_qemu(serial_log, extra):
    args = [QEMU, "-machine", "pc", "-m", "256", "-display", "none",
            "-serial", f"file:{serial_log}",
            "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait", "-smp", "2"] + extra
    print("[qemu] " + " ".join(args[2:]))
    return subprocess.Popen(args, cwd=str(ROOT))


def run_cmd(qmp, serial, cmd):
    qmp.type(cmd)
    qmp.key("ret")


# ---------------------------------------------------------------- phase 1
def phase1_install_and_change(serial_log, checker):
    if DISK.exists():
        DISK.unlink()
    with open(DISK, "wb") as f:
        f.truncate(DISK_MIB * 1024 * 1024)
    print(f"\n=== phase 1: install onto {DISK.name} ({DISK_MIB} MiB), then change user data ===")

    proc = start_qemu(serial_log, ["-cdrom", "build/sutureos.iso", "-boot", "d",
                                   "-drive", f"id=disk0,file={DISK.as_posix()},format=raw,if=none",
                                   "-device", "ide-hd,drive=disk0,bus=ide.0",
                                   "-no-reboot"])
    serial = Serial(serial_log)
    qmp = None
    try:
        qmp = Qmp()
        serial.wait_for(r"sutureos# ", timeout=240, name="live shell prompt")

        # --- installer ---
        run_cmd(qmp, serial, "install")
        serial.wait_for(r"Target disk index", timeout=60)
        run_cmd(qmp, serial, "0")
        serial.wait_for(r"Type 'yes' to continue", timeout=30)
        run_cmd(qmp, serial, "yes")
        serial.wait_for(r"root password \(empty", timeout=30)
        qmp.type(ROOT_PW); qmp.key("ret")
        serial.wait_for(r"repeat root password", timeout=30)
        qmp.type(ROOT_PW); qmp.key("ret")
        serial.wait_for(r"Create a normal user", timeout=30)
        run_cmd(qmp, serial, "n")
        m = serial.wait_for(r"^\[install\] writing /SUTUREOS/PERSIST", timeout=420,
                            name="PERSIST skeleton")
        checker.check(True, "installer writes the /SUTUREOS/PERSIST skeleton")
        serial.wait_for(r"^\[install\] done", timeout=420, name="[install] done")
        checker.check(True, "installation finished ([install] done)")
        serial.wait_for(r"sutureos# ", timeout=120, name="shell back after install")

        # --- add a user, write data, flush, power off ---
        run_cmd(qmp, serial, "useradd " + USER)
        serial.wait_for(r"New password \(empty", timeout=30)
        qmp.type(USER_PW); qmp.key("ret")
        serial.wait_for(r"Retype new password", timeout=30)
        qmp.type(USER_PW); qmp.key("ret")
        m = serial.wait_for(rf"useradd: created {USER} uid=(\d+) gid=(\d+) home=/home/{USER}",
                            timeout=60, name="useradd output")
        checker.check(True, f"useradd created {USER} (uid {m.group(1)})")

        run_cmd(qmp, serial, f"write /home/{USER}/NOTE.TXT {NOTE_TEXT}")
        serial.wait_for(r"write: \S+ \(\d+ bytes\)", timeout=30, name="write NOTE.TXT")
        run_cmd(qmp, serial, f"write /etc/motd {MOTD_TEXT}")
        serial.wait_for(r"write: /etc/motd \(\d+ bytes\)", timeout=30,
                        name="overwrite /etc/motd")
        run_cmd(qmp, serial, "sync")
        m = serial.wait_for(r"^\[persist\] flush (\d+) files", timeout=120,
                            name="[persist] flush on sync")
        flushed_sync = int(m.group(1))
        checker.check(flushed_sync >= 3, "sync flushed the mirror", f"{flushed_sync} files")
        serial.wait_for(r"^sync: \d+ ok", timeout=60, name="sync summary")

        # shutdown must flush again and then really power the machine off
        run_cmd(qmp, serial, "shutdown")
        serial.wait_for(r"^\[persist\] flush \d+ files", timeout=120,
                        name="[persist] flush on shutdown")
        checker.check(True, "shutdown flushes the mirror before powering off")
        try:
            proc.wait(timeout=60)
            powered_off = True
        except subprocess.TimeoutExpired:
            powered_off = False
        checker.check(powered_off, "QEMU powered off by shutdown (ACPI S5)")
        checker.check("PANIC" not in serial.text, "no PANIC during phase 1")
    finally:
        if qmp:
            qmp.quit()
        if proc.poll() is None:
            proc.kill()
    return serial.text


# ---------------------------------------------------------------- FAT32 reader
class Fat32:
    def __init__(self, image, part_lba=PART_LBA):
        self.img = image
        self.base = part_lba * 512
        bs = image[self.base:self.base + 512]
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        self.reserved = struct.unpack_from("<H", bs, 14)[0]
        self.nfats = bs[16]
        self.fatsz = struct.unpack_from("<I", bs, 36)[0]
        self.rootclus = struct.unpack_from("<I", bs, 44)[0]
        self.fat_off = self.base + self.reserved * self.bps
        self.data_off = self.fat_off + self.nfats * self.fatsz * self.bps
        self.cluster_bytes = self.spc * self.bps

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

    def _lfn(self, pending):
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
                if len(e) < 32 or e[0] == 0x00:
                    return entries
                if e[0] == 0xE5:
                    pending = []
                    continue
                attr = e[11]
                if attr == 0x0F:
                    pending.append(e)
                    continue
                if attr & 0x08:
                    pending = []
                    continue
                name = self._lfn(pending) if pending else self._short(e)
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
                if path.count("/") < 5:
                    out.update(self.walk(first, path))
            else:
                out[path] = {"size": size, "first": first}
        return out

    def file(self, path):
        entries = self.walk()
        e = entries.get(path)
        if not e or e.get("dir"):
            return None
        return self.read(e["first"], e["size"])


def verify_pbkdf2(spec, password, iters_expected=2000):
    m = re.fullmatch(r"\$sutureos\$pbkdf2-sha256\$(\d+)\$([0-9a-f]{32,})\$([0-9a-f]{64})", spec)
    if not m:
        return False, "spec mismatch"
    iters = int(m.group(1))
    salt = bytes.fromhex(m.group(2))
    got = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iters, 32).hex()
    return got == m.group(3) and iters == iters_expected, f"iters={iters}"


# ---------------------------------------------------------------- phase 2
def phase2_verify_host(checker):
    print("\n=== phase 2: host verification of the persistence mirror ===")
    checker.check(DISK.exists() and DISK.stat().st_size > 0, "installed disk image exists")
    img = DISK.read_bytes()
    fat = Fat32(img)
    files = fat.walk()

    persist = sorted(p for p in files if p.startswith("/SUTUREOS/PERSIST"))
    print("  files under /SUTUREOS/PERSIST:")
    for p in persist:
        e = files[p]
        print(f"    {p:44s} {'<dir>' if e.get('dir') else str(e['size']) + ' bytes'}")
    checker.check(any(p.startswith("/SUTUREOS/PERSIST/") for p in persist),
                  "PERSIST tree exists on the system volume")

    def text(path):
        data = fat.file(path)
        return data.decode("utf-8", "replace") if data is not None else None

    version = text("/SUTUREOS/PERSIST/VERSION")
    checker.check(version is not None and "sutureos-persist" in version,
                  "PERSIST/VERSION present", (version or "").strip())
    checker.check(version is not None and "1" in version, "PERSIST format version 1")

    motd = text("/SUTUREOS/PERSIST/etc/motd")
    checker.check(motd is not None and motd.strip() == MOTD_TEXT,
                  "PERSIST/etc/motd contains the text changed at runtime", repr(motd))

    note = text(f"/SUTUREOS/PERSIST/home/{USER}/NOTE.TXT")
    checker.check(note is not None and NOTE_TEXT in note,
                  f"PERSIST/home/{USER}/NOTE.TXT persisted", repr(note))

    passwd = text("/SUTUREOS/PERSIST/etc/passwd") or ""
    checker.check(re.search(rf"^{USER}:x:\d+:\d+:", passwd, re.M) is not None,
                  f"PERSIST/etc/passwd has {USER} (created with useradd)")
    checker.check("root:x:0:0:root:/root:/bin/sh" in passwd, "PERSIST/etc/passwd keeps root")

    shadow = text("/SUTUREOS/PERSIST/etc/shadow") or ""
    m = re.search(rf"^{USER}:(\S+):", shadow, re.M)
    if checker.check(m is not None, f"PERSIST/etc/shadow has a {USER} hash"):
        ok, why = verify_pbkdf2(m.group(1), USER_PW)
        checker.check(ok, f"{USER} hash verifies against the password typed in phase 1",
                      why)
    group = text("/SUTUREOS/PERSIST/etc/group") or ""
    checker.check(re.search(rf"^{USER}:", group, re.M) is not None,
                  f"PERSIST/etc/group has the {USER} private group")
    checker.check(text("/SUTUREOS/PERSIST/etc/sudoers") is not None,
                  "PERSIST/etc/sudoers present")
    checker.check(f"/SUTUREOS/PERSIST/etc/shadow" in files and
                  f"/SUTUREOS/PERSIST/etc/passwd" in files,
                  "PERSIST/etc carries the account files")
    return files


# ---------------------------------------------------------------- phase 3
def phase3_boot_again(serial_log, checker):
    print("\n=== phase 3: boot the same disk (no ISO) and check the restored data ===")
    proc = start_qemu(serial_log, ["-drive", f"id=disk0,file={DISK.as_posix()},format=raw,if=none",
                                   "-device", "ide-hd,drive=disk0,bus=ide.0",
                                   "-boot", "c", "-no-reboot", "-no-shutdown"])
    serial = Serial(serial_log)
    qmp = None
    try:
        qmp = Qmp()
        serial.wait_for(r"multiboot1: Loading executable `boot\(\):/kernel\.elf`",
                        timeout=90, name="Limine loads the kernel")
        checker.check(True, "Limine loads the kernel from the installed disk")
        m = serial.wait_for(r"^\[selftest\].*\n", timeout=120, name="boot log")
        serial.wait_for(r"selftests: (\d+)/(\d+) passed", timeout=60, name="selftest summary")
        mm = re.search(r"selftests: (\d+)/(\d+) passed", serial.text)
        # 13/14 is the project baseline: [selftest][moos_intr] has been a known
        # pre-existing failure for many releases (see docs/porting-roadmap.md).
        checker.check(mm is not None and mm.group(1) == "13" and mm.group(2) == "14",
                      "kernel self-tests at the known baseline (13/14)",
                      mm.group(0) if mm else "?")
        serial.wait_for(r"^\[persist\] restored (\d+) file", timeout=120,
                        name="[persist] restored")
        restored = int(re.search(r"\[persist\] restored (\d+) file", serial.text).group(1))
        checker.check(restored >= 3, "boot restored the persistence mirror",
                      f"{restored} files")
        serial.wait_for(r"^\[sys\] installed system volume .*restored", timeout=60,
                        name="[sys] restored line")
        checker.check(True, "[sys] reports the PERSIST restore (not the legacy fallback)")
        serial.wait_for(r"sutureos# ", timeout=180, name="shell prompt")

        run_cmd(qmp, serial, "cat /etc/motd")
        m = serial.wait_for(rf"^{re.escape(MOTD_TEXT)}", timeout=20, name="/etc/motd text")
        checker.check(True, "/etc/motd is the text changed before the reboot")

        run_cmd(qmp, serial, "users")
        serial.wait_for(rf"^{USER}\b", timeout=20, name="users lists bob")
        checker.check(True, f"`users` lists {USER} after the reboot")

        run_cmd(qmp, serial, f"cat /home/{USER}/NOTE.TXT")
        m = serial.wait_for(rf"{re.escape(NOTE_TEXT)}", timeout=20, name="NOTE.TXT content")
        checker.check(True, f"/home/{USER}/NOTE.TXT survived the reboot")

        # `persist` status must report the mirror and the format version. Run it
        # before `su`, because `exit` from the su session can end the shell task.
        run_cmd(qmp, serial, "persist")
        m = serial.wait_for(r"^persist: volume=/mnt/sys/SUTUREOS/PERSIST format=v(\d+)",
                            timeout=20, name="persist status")
        checker.check(m.group(1) == "1", "`persist` reports format v1", m.group(1))
        serial.wait_for(r"^persist: restored at boot=(\d+) file", timeout=20,
                        name="persist restore counter")
        checker.check(True, "`persist` reports the restored file count")

        run_cmd(qmp, serial, "su " + USER)
        serial.wait_for(r"[Pp]assword", timeout=20, name="su password prompt")
        qmp.type(USER_PW)
        qmp.key("ret")
        serial.wait_for(rf"{USER}@sutureos\$ ", timeout=30, name="user prompt")
        checker.check(True, f"su {USER} works with the password set before the reboot")
        run_cmd(qmp, serial, "whoami")
        serial.wait_for(rf"^{USER}\s*$", timeout=20, name="whoami")
        checker.check(True, f"whoami reports {USER}")
        run_cmd(qmp, serial, "exit")
        time.sleep(1.0)

        checker.check("PANIC" not in serial.text, "no PANIC during phase 3")
        return serial.text
    finally:
        if qmp:
            qmp.quit()
        if proc.poll() is None:
            proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-phase1", action="store_true",
                    help="reuse build/persist_disk.img from a previous run")
    args = ap.parse_args()

    c = Checker()
    if not ISO.exists() or not KERNEL.exists():
        print("missing build/sutureos.iso or build/kernel.elf - run scripts/build.py "
              "and scripts/build_iso.py first")
        return 1
    serial1 = ROOT / "build" / "serial_persist_phase1.log"
    serial2 = ROOT / "build" / "serial_persist_phase2.log"
    for p in (serial1, serial2):
        if p.exists():
            p.unlink()

    if not args.skip_phase1:
        phase1_install_and_change(serial1, c)
    phase2_verify_host(c)
    phase3_boot_again(serial2, c)

    print(f"\nchecks={c.checks} ok={c.checks - len(c.fails)} fail={len(c.fails)}")
    for f in c.fails:
        print(f"  FAILED: {f}")
    print(f"serial logs: {serial1.name}, {serial2.name}")
    return 0 if not c.fails else 1


if __name__ == "__main__":
    sys.exit(main())
