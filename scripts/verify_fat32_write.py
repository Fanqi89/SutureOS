#!/usr/bin/env python3
"""Host-side verification of a guest FAT32 write (fat32-writable-volume bar).

Run AFTER the guest has booted with `-hda build/fat32.img` and written its
self-test file. Re-parses the raw image with an independent FAT32 reader and
requires, without trusting anything the guest printed:

  1. the new file SUTURE.TXT exists with byte-for-byte correct content,
  2. the pre-existing files (HELLO.TXT / README.TXT / HELLO.DEB) are unchanged,
  3. FAT1 == FAT2 and the root cluster is still marked allocated,
  4. the FSInfo free count equals the measured free-cluster count,
  5. the whole-image CRC32 changed versus the pristine image (a write really
     reached the medium),
  6. the directory/rename/unlink self-test left exactly the expected state:
     SUTDIR/ and DELME.TXT gone, TMPFILE.TXT renamed to RENAMED.TXT with its
     bytes intact, and no orphaned cluster chains anywhere in the FAT.

Usage:
  py -3 scripts/verify_fat32_write.py <dirty.img> [pristine.img]
"""
import io
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import mkfat32_testimg as mk  # noqa: E402  (reuse the deterministic builders)

S = 512
# The guest writes `sizeof(wpayload)` bytes, i.e. the string INCLUDING its NUL
# terminator - the verifier must expect that trailing 0x00.
PAYLOAD = b"SutureOS FAT32 write self-test\n\x00"


def parse(img: bytes):
    bs = img[:S]
    reserved = struct.unpack_from("<H", bs, 14)[0]
    nfats = bs[16]
    fatsz = struct.unpack_from("<I", bs, 36)[0]
    rootclus = struct.unpack_from("<I", bs, 44)[0]
    fsinfo = struct.unpack_from("<I", bs, 48)[0]
    spc = bs[13]
    data_start = reserved + nfats * fatsz
    clusters = (len(img) // S - data_start) // spc
    fat0 = img[reserved * S:(reserved + fatsz) * S]
    fat1 = img[(reserved + fatsz) * S:(reserved + 2 * fatsz) * S]

    def fget(cl):
        return struct.unpack_from("<I", fat0, cl * 4)[0] & 0x0FFFFFFF

    def read_chain(first, size=None):
        out = b""
        cl = first
        guard = 0
        while 2 <= cl < 0x0FFFFFF8 and guard < 4096:
            sec = data_start + (cl - 2) * spc
            out += img[sec * S:(sec + spc) * S]
            cl = fget(cl)
            guard += 1
        return out[:size] if size else out

    root = img[data_start * S:(data_start + spc) * S]
    entries = {}
    for i in range(0, len(root), 32):
        e = root[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            continue
        name = e[0:8].decode("latin1").rstrip()
        ext = e[8:11].decode("latin1").rstrip()
        full = f"{name}.{ext}" if ext else name
        first = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16)
        size = struct.unpack_from("<I", e, 28)[0]
        entries[full] = (first, size)
    free_measured = sum(1 for cl in range(2, clusters + 2) if fget(cl) == 0)
    fsinfo_free = struct.unpack_from("<I", img, fsinfo * S + 488)[0]
    return dict(reserved=reserved, nfats=nfats, fatsz=fatsz, rootclus=rootclus,
                spc=spc, clusters=clusters, fat0=fat0, fat1=fat1, fget=fget,
                read_chain=read_chain, entries=entries,
                free_measured=free_measured, fsinfo_free=fsinfo_free)


def main():
    dirty_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "fat32.img")
    pristine_path = sys.argv[2] if len(sys.argv) > 2 else None
    img = open(dirty_path, "rb").read()
    v = parse(img)

    checks = []
    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    if pristine_path and os.path.exists(pristine_path):
        pristine = open(pristine_path, "rb").read()
        check("whole-image CRC32 changed (write reached the medium)",
              zlib.crc32(img) != zlib.crc32(pristine),
              f"{zlib.crc32(pristine) & 0xffffffff:08X} -> {zlib.crc32(img) & 0xffffffff:08X}")

    check("FAT1 == FAT2", v["fat0"] == v["fat1"])
    check("root cluster marked allocated",
          v["fget"](v["rootclus"]) == 0x0FFFFFFF,
          f"fat[{v['rootclus']}]=0x{v['fget'](v['rootclus']):x}")

    # 1) new file, byte-for-byte
    if "SUTURE.TXT" in v["entries"]:
        first, size = v["entries"]["SUTURE.TXT"]
        data = v["read_chain"](first, size)
        check("SUTURE.TXT exists with correct size", size == len(PAYLOAD), f"size={size}")
        check("SUTURE.TXT content byte-for-byte", data == PAYLOAD, repr(data[:48]))
        check("SUTURE.TXT cluster not the root cluster", first != v["rootclus"], f"first={first}")
    else:
        check("SUTURE.TXT exists", False, "missing from root directory")

    # 2) pre-existing files unchanged
    expect = {
        "HELLO.TXT": (b"Hello from SutureOS FAT32!\n"
                      b"This file lives on a real FAT32 volume (driver: fat32).\n"),
        "README.TXT": (b"SutureOS test disk.\n"
                       b"Files: HELLO.TXT, README.TXT, hello.deb\n"
                       b"hello.deb is a real Debian package; the kernel installs it\n"
                       b"at boot (see kmain deb demo).\n"),
        "HELLO.DEB": mk.make_deb(),
    }
    for name, want in expect.items():
        if name not in v["entries"]:
            check(f"{name} still present", False, "missing")
            continue
        first, size = v["entries"][name]
        got = v["read_chain"](first, size)
        check(f"{name} unchanged ({len(want)} bytes)", got == want,
              f"got {len(got)} bytes" if got != want else "")

    # 4) FSInfo bookkeeping
    check("FSInfo free count == measured free clusters",
          v["fsinfo_free"] == v["free_measured"],
          f"fsinfo={v['fsinfo_free']} measured={v['free_measured']}")

    # 5) mkdir/rmdir + rename + unlink (the ops self-test in kmain). The guest's
    # final on-disk state must be exactly: SUTDIR/ removed, DELME.TXT removed,
    # TMPFILE.TXT renamed to RENAMED.TXT with its bytes intact.
    RENAME_PAYLOAD = b"SutureOS rename/unlink test\n\x00"   # sizeof() includes the NUL
    check("TMPFILE.TXT is gone after the rename", "TMPFILE.TXT" not in v["entries"])
    if "RENAMED.TXT" in v["entries"]:
        rfirst, rsize = v["entries"]["RENAMED.TXT"]
        rdata = v["read_chain"](rfirst, rsize)
        check("RENAMED.TXT exists with the pre-rename size",
              rsize == len(RENAME_PAYLOAD), f"size={rsize}")
        check("RENAMED.TXT content survived the rename byte-for-byte",
              rdata == RENAME_PAYLOAD, repr(rdata[:48]))
        check("RENAMED.TXT is a real file, not the root directory",
              rfirst != v["rootclus"], f"first={rfirst}")
    else:
        check("RENAMED.TXT exists after the rename", False, "missing from root")
    check("DELME.TXT is gone after the unlink", "DELME.TXT" not in v["entries"])
    check("SUTDIR is gone after the rmdir", "SUTDIR" not in v["entries"])
    # A removed directory/file must have released its clusters: with no leaked
    # chains the FAT contains nothing but the root chain and the file chains.
    live = set()
    for nm, (first, _sz) in v["entries"].items():
        cl, guard = first, 0
        while 2 <= cl < 0x0FFFFFF8 and guard < 4096:
            live.add(cl)
            cl = v["fget"](cl)
            guard += 1
    live.add(v["rootclus"])
    orphan = [cl for cl in range(2, v["clusters"] + 2)
              if v["fget"](cl) != 0 and cl not in live]
    check("no orphaned cluster chains left behind (rmdir/unlink freed them)",
          not orphan, f"{len(orphan)} orphan(s): {orphan[:8]}")

    print(f"image: {dirty_path} ({len(img)} bytes, {v['clusters']} clusters)")
    ok_count = 0
    for name, ok, detail in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))
        ok_count += ok
    failed = len(checks) - ok_count
    print(f"=== RESULT: {'PASS' if failed == 0 else 'FAIL'} ===  checks={len(checks)} ok={ok_count} fail={failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
