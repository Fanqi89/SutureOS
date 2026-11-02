#!/usr/bin/env python3
"""Build the ISO9660 data-disc test fixture for the ATAPI/ISO9660 port.

Why a script instead of a few shell lines: PowerShell 5.1's `>` redirect writes
UTF-16LE with a BOM, so a file created that way is NOT the ASCII payload the
guest self-test expects and `cat` would show garbage. Everything here is written
as explicit UTF-8 bytes (ASCII content), then packed with MSYS2's xorriso
(ISO9660 + Rock Ridge + Joliet, no El Torito boot record -> a *data* disc,
exactly the opposite of SutureOS' bootable limine ISO).

Outputs (all under build/):
  build/cdfix/README.TXT        "hello from an ISO9660 data CD\\n"  (30 bytes)
  build/cdfix/SUB/NESTED.TXT    "nested file\\n"                     (12 bytes)
  build/cdfix/SUB/DEEP/THIRD.TXT  third-level file                   (for multi-level)
  build/test_cd.iso             the data CD image (-V SUTURE_CD)

Usage: py -3 scripts/mkiso9660_testcd.py [--out build/test_cd.iso]
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
XORRISO = Path(r"C:\msys64\usr\bin\xorriso.exe")

# The guest self-test hard-codes the README.TXT payload (kernel/fs/iso9660.c,
# ISO_SELFTEST_MARKER). Keep these two in sync; the host verifier below checks it.
README = b"hello from an ISO9660 data CD\n"
NESTED = b"nested file\n"
THIRD = b"third level file\n"
LONG_NAME = b"a long file name for the multi-block directory test\n" * 90  # > 2 KiB


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build" / "test_cd.iso"))
    ap.add_argument("--volume-id", default="SUTURE_CD")
    args = ap.parse_args()

    if not XORRISO.exists():
        print(f"xorriso not found: {XORRISO}")
        return 1

    fix = ROOT / "build" / "cdfix"
    if fix.exists():
        shutil.rmtree(fix)
    (fix / "SUB" / "DEEP").mkdir(parents=True)

    files = {
        fix / "README.TXT": README,
        fix / "SUB" / "NESTED.TXT": NESTED,
        fix / "SUB" / "DEEP" / "THIRD.TXT": THIRD,
        # 3000+ bytes so the file spans two 2048-byte sectors: proves reads cross
        # a block boundary (a one-sector-only reader would truncate it).
        fix / "BIG.TXT": LONG_NAME,
    }
    # 60 entries in one directory -> its records exceed one 2048-byte block, so
    # the reader must honour record lengths AND the per-block zero padding
    # (ECMA-119 9.3: length==0 means "rest of this sector is padding").
    (fix / "DIRMANY").mkdir()
    for i in range(60):
        files[fix / "DIRMANY" / f"F{i:02d}.TXT"] = b"file %02d\n" % i
    for path, data in files.items():
        path.write_bytes(data)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()

    # xorriso wants MSYS2-style paths (/c/...) for a path under C:\
    def msys(p: Path) -> str:
        s = str(p.resolve())
        if len(s) > 2 and s[1] == ":":
            return "/" + s[0].lower() + s[2:].replace("\\", "/")
        return s.replace("\\", "/")

    cmd = [
        str(XORRISO), "-as", "mkisofs",
        "-o", msys(out),
        "-V", args.volume_id,
        "-R", "-J",
        msys(fix),
    ]
    print("running:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        return r.returncode
    if not out.exists():
        print("xorriso reported success but the image is missing")
        return 1
    size = out.stat().st_size
    print(f"wrote {out} ({size} bytes)")
    for path, data in files.items():
        print(f"  {path.relative_to(fix)}: {len(data)} bytes")
    # 2048-byte sectors, and a size that is a multiple of it
    if size % 2048:
        print(f"WARNING: image size {size} is not a multiple of 2048")
    return 0


if __name__ == "__main__":
    sys.exit(main())
