#!/usr/bin/env python3
"""Host-side acceptance for the ATAPI + ISO9660 port (no code from the kernel).

Two independent checks, both done purely host-side:

1. Fixture check: parse build/test_cd.iso with this script's own ECMA-119 reader
   (PVD -> root directory records -> extents) and compare every fixture file with
   the bytes on disk in build/cdfix/. Also asserts the disc is NOT El Torito
   bootable (that is what makes it a "data disc") and that the SutureOS boot ISO
   IS bootable (that is what the kernel must refuse to mount as a data disc).

2. Guest-log check: grep the QEMU serial log for the required lines and for the
   shell transcript, then compare the transcript's `cat` output with the host
   bytes. This is what proves the guest really read the disc through ATAPI rather
   than printing something plausible.

Usage:
  py -3 scripts/verify_iso9660_read.py
  py -3 scripts/verify_iso9660_read.py --iso build/test_cd.iso --fix build/cdfix \
        --guest-log build/serial_cd3.log --boot-iso build/sutureos.iso
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SECTOR = 2048


# ---------------------------------------------------------------- ECMA-119 reader
def le32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def read_volume(path: Path):
    """Return (pvd_info, has_boot_record, sector_reader) parsed independently of
    the guest. The whole image is loaded into memory (a test CD is small)."""
    image = path.read_bytes()

    def rd(lba):
        start = lba * SECTOR
        data = image[start:start + SECTOR]
        if len(data) != SECTOR:
            raise ValueError(f"short read at sector {lba}")
        return data

    info = None
    has_boot = False
    for lba in range(16, 48):
        vd = rd(lba)
        if vd[1:6] != b"CD001":
            raise ValueError(f"sector {lba}: not an ISO9660 volume descriptor")
        vtype = vd[0]
        if vtype == 0:
            has_boot = True
            continue
        if vtype == 0xFF:
            break
        if vtype != 1 or info is not None:
            continue
        if int.from_bytes(vd[128:130], "little") != SECTOR:
            raise ValueError("logical block size is not 2048")
        info = {
            "volume_id": vd[40:72].decode("ascii", "replace").rstrip(" \x00"),
            "volume_blocks": le32(vd, 80),
            "root_lba": le32(vd, 158),
            "root_size": le32(vd, 166),
            "image_size": len(image),
        }
    if info is None:
        raise ValueError("no primary volume descriptor")
    return info, has_boot, rd


def parse_dir(rd, lba, size):
    """Yield (name, extent_lba, size, is_dir) for every record, skipping . / .."""
    pos = 0
    entries = []
    while pos < size:
        blkno, off = divmod(pos, SECTOR)
        block = rd(lba + blkno)
        length = block[off]
        if length == 0:
            pos = (blkno + 1) * SECTOR
            continue
        if length < 34 or off + length > SECTOR:
            raise ValueError(f"bad directory record length {length} at offset {pos}")
        rec = block[off:off + length]
        name_len = rec[32]
        raw = rec[33:33 + name_len]
        if raw not in (b"\x00", b"\x01"):
            name = raw.split(b";")[0]
            if name.endswith(b"."):
                name = name[:-1]
            entries.append({
                "name": name.decode("ascii", "replace"),
                "lba": le32(rec, 2),
                "size": le32(rec, 10),
                "is_dir": bool(rec[25] & 0x02),
            })
        pos += length
    return entries


def lookup(rd, info, guest_path):
    """Resolve "/SUB/NESTED.TXT" the same way the guest driver does."""
    lba, size = info["root_lba"], info["root_size"]
    for comp in [c for c in guest_path.split("/") if c]:
        match = None
        for e in parse_dir(rd, lba, size):
            if e["name"].upper() == comp.upper():
                match = e
                break
        if match is None:
            return None
        lba, size = match["lba"], match["size"]
    return lba, size


def read_extent(rd, lba, offset, length):
    out = bytearray()
    while len(out) < length:
        absoff = offset + len(out)
        block = rd(lba + absoff // SECTOR)
        within = absoff % SECTOR
        take = min(SECTOR - within, length - len(out))
        out += block[within:within + take]
    return bytes(out)


# ---------------------------------------------------------------- checks
class Checker:
    def __init__(self):
        self.checks = 0
        self.fails = []

    def check(self, ok, what, detail=""):
        self.checks += 1
        if not ok:
            self.fails.append(f"{what}{(': ' + detail) if detail else ''}")
        print(f"  [{'ok ' if ok else 'FAIL'}] {what}{(': ' + detail) if detail else ''}")
        return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=str(ROOT / "build" / "test_cd.iso"))
    ap.add_argument("--fix", default=str(ROOT / "build" / "cdfix"))
    ap.add_argument("--guest-log", default=str(ROOT / "build" / "serial_cd3.log"))
    ap.add_argument("--boot-iso", default=str(ROOT / "build" / "sutureos.iso"))
    args = ap.parse_args()

    iso = Path(args.iso)
    fix = Path(args.fix)
    log_path = Path(args.guest_log)
    boot_iso = Path(args.boot_iso)
    c = Checker()

    print(f"fixture: {iso}")
    info, has_boot, rd = read_volume(iso)
    c.check(info["volume_id"] == "SUTURE_CD", "data disc volume id = SUTURE_CD",
            info["volume_id"])
    c.check(not has_boot, "data disc has no El Torito boot record (it is a data disc)")
    c.check(info["volume_blocks"] * SECTOR >= iso.stat().st_size,
            "PVD volume size covers the image",
            f'{info["volume_blocks"]}x2048 vs {iso.stat().st_size}B')

    # Every fixture file, via an independent parse of the image.
    host_files = sorted(p for p in fix.rglob("*") if p.is_file())
    for host in host_files:
        rel = host.relative_to(fix)
        guest_path = "/" + str(rel).replace("\\", "/")
        found = lookup(rd, info, guest_path)
        want = host.read_bytes()
        if not c.check(found is not None, f"find {guest_path}"):
            continue
        lba, size = found
        c.check(size == len(want), f"size {guest_path} = {len(want)}", str(size))
        got = read_extent(rd, lba, 0, min(size, len(want)))
        c.check(got == want, f"bytes {guest_path}")

    # Multi-block directory: 60 entries must be visible in one directory read.
    many = parse_dir(rd, *lookup(rd, info, "/DIRMANY"))
    c.check(len(many) == 60, "DIRMANY lists 60 entries", str(len(many)))
    c.check(many[-1]["name"] == "F59.TXT", "DIRMANY last entry F59.TXT", many[-1]["name"])

    # The boot ISO must be recognisable as bootable so the guest skips it.
    print(f"\nboot iso: {boot_iso}")
    b_info, b_has_boot, _ = read_volume(boot_iso)
    c.check(b_has_boot, "boot ISO has an El Torito boot record (guest must skip it)")
    c.check(b_info["volume_id"] == "SUTUREOS", "boot ISO volume id = SUTUREOS",
            b_info["volume_id"])

    # ------------------------------------------------------------ guest log
    print(f"\nguest log: {log_path}")
    c.check(log_path.exists(), "guest serial log exists")
    if not log_path.exists():
        return 1
    log = log_path.read_bytes().decode("utf-8", "replace")

    # The serial file uses CRLF line endings, hence the \r? before $.
    m = re.search(r'^\[atapi\] drive (\d+): CD-ROM "([^"]*)" (\d+) blocks x 2048B\r?$',
                  log, re.M)
    c.check(m is not None, "log has [atapi] CD-ROM line",
            m.group(0) if m else "missing")
    if m:
        c.check(int(m.group(3)) >= info["volume_blocks"],
                "atapi READ CAPACITY >= volume size", m.group(3))

    m = re.search(r"^\[iso9660\] mount drive=(\d+) vol=(\S+) files=(\d+) blocks=(\d+)\r?$",
                  log, re.M)
    c.check(m is not None, "log has [iso9660] mount line", m.group(0) if m else "missing")
    if m:
        c.check(m.group(2) == "SUTURE_CD", "mounted volume is the data disc", m.group(2))
        c.check(int(m.group(4)) == info["volume_blocks"], "mounted block count matches PVD",
                m.group(4))

    m = re.search(r"^\[iso9660\] selftest PASS \(file=(\S+), (\d+) bytes\)\r?$", log, re.M)
    c.check(m is not None, "log has [iso9660] selftest PASS", m.group(0) if m else "missing")
    if m:
        c.check(m.group(1) == "/README.TXT", "selftest file is /README.TXT", m.group(1))
        c.check(int(m.group(2)) == len((fix / "README.TXT").read_bytes()),
                "selftest byte count matches the host file", m.group(2))

    # Shell transcript: cat output must equal the host bytes.
    for guest_path, host_file in (("/mnt/cd/README.TXT", "README.TXT"),
                                  ("/mnt/cd/SUB/NESTED.TXT", "SUB/NESTED.TXT"),
                                  ("/mnt/cd/SUB/DEEP/THIRD.TXT", "SUB/DEEP/THIRD.TXT")):
        want = (fix / host_file).read_bytes()
        pat = re.compile(r"sutureos# cat " + re.escape(guest_path.lower()) + r"\r?\n(.*?)\((\d+) bytes\)",
                         re.S)
        mm = pat.search(log)
        if not c.check(mm is not None, f"guest `cat {guest_path}` present"):
            continue
        got = mm.group(1).encode("utf-8", "replace")
        # the terminal echoes CRLF; compare content, not line endings
        c.check(got.rstrip(b"\r\n") == want.rstrip(b"\r\n"),
                f"guest cat content matches host bytes for {guest_path}")
        c.check(int(mm.group(2)) == len(want),
                f"guest byte count for {guest_path} = {len(want)}", mm.group(2))

    # ls must show the fixture names and the SUB directory marker.
    mm = re.search(r"sutureos# ls /mnt/cd\r?\n(.*?)sutureos#", log, re.S)
    if c.check(mm is not None, "guest `ls /mnt/cd` present"):
        listing = mm.group(1)
        for want_name in ("README.TXT", "SUB", "BIG.TXT", "DIRMANY"):
            c.check(want_name in listing, f"ls /mnt/cd lists {want_name}")

    # multi-level listing
    mm = re.search(r"sutureos# ls /mnt/cd/sub/deep\r?\n(.*?)sutureos#", log, re.S)
    if c.check(mm is not None, "guest `ls /mnt/cd/sub/deep` present (3rd level)"):
        c.check("THIRD.TXT" in mm.group(1), "3rd-level listing shows THIRD.TXT")

    print(f"\nchecks={c.checks} ok={c.checks - len(c.fails)} fail={len(c.fails)}")
    for f in c.fails:
        print(f"  FAILED: {f}")
    return 0 if not c.fails else 1


if __name__ == "__main__":
    sys.exit(main())
