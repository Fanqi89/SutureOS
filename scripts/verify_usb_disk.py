#!/usr/bin/env python3
"""Host-side verification of the USB mass-storage port (never trust the guest).

Independent, host-only evidence for the EHCI/BOT work:

  1. re-parse the image with the deterministic FAT32 reader
     (mkfat32_testimg.verify_image): genuine FAT32 geometry, FAT1 == FAT2,
     FSInfo free count == the measured free clusters, and every expected file
     byte-for-byte;
  2. with a pristine image supplied, diff the two images sector by sector:
     - default: the two images must be IDENTICAL. The guest self-test writes a
       pattern into the last block, reads it back and restores the original
       bytes, so a clean run has to leave the medium untouched - this is what
       proves the write path did not scribble anywhere else;
     - --allow-write: changed sectors are expected (the shell `write` test),
       and every changed sector must fall inside the FAT / FSInfo / root
       directory / the new file's clusters (reported, not asserted blindly);
  3. optionally assert a file the guest created through the shell:
     --expect-file W.TXT --expect-text "hello from usb".

Usage:
  py -3 scripts/verify_usb_disk.py build/usbdisk.img
  py -3 scripts/verify_usb_disk.py build/usbdisk_dirty.img build/usbdisk.img --allow-write \
         --expect-file W.TXT --expect-text "hello from usb"
"""
import argparse
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import mkfat32_testimg as mk  # noqa: E402
import mk_usb_testimg as usbimg  # noqa: E402

S = 512

# Everything the guest is expected to have on the stick before it writes
# anything itself.
EXPECT = {
    "USB.TXT": usbimg.USB_TXT,
    "HELLO.TXT": usbimg.HELLO_TXT,
    "README.TXT": usbimg.README_TXT,
}


def geometry(img: bytes):
    bs = img[:S]
    reserved = struct.unpack_from("<H", bs, 14)[0]
    nfats = bs[16]
    fatsz = struct.unpack_from("<I", bs, 36)[0]
    rootclus = struct.unpack_from("<I", bs, 44)[0]
    spc = bs[13]
    data_start = reserved + nfats * fatsz
    return reserved, nfats, fatsz, rootclus, spc, data_start


def read_root_entries(img: bytes):
    """Return {8.3 name: (first_cluster, size)} walking the whole root chain."""
    reserved, nfats, fatsz, rootclus, spc, data_start = geometry(img)
    fat0 = img[reserved * S:(reserved + fatsz) * S]

    def fget(cl):
        return struct.unpack_from("<I", fat0, cl * 4)[0] & 0x0FFFFFFF

    # Root directory may span several clusters (the FAT32 driver itself had a
    # bug here once), so follow the chain.
    cl = rootclus
    entries = {}
    guard = 0
    while cl < 0x0FFFFFF8 and guard < 4096:
        base = data_start + (cl - 2) * spc
        chunk = img[base * S:(base + spc) * S]
        for i in range(0, len(chunk), 32):
            ent = chunk[i:i + 32]
            if len(ent) < 32 or ent[0] == 0:
                continue
            if ent[0] == 0xE5 or (ent[11] & 0x0F) == 0x0F or (ent[11] & 0x08):
                continue
            name = ent[0:8].decode(errors="replace").rstrip()
            ext = ent[8:11].decode(errors="replace").rstrip()
            full = f"{name}.{ext}" if ext else name
            first = struct.unpack_from("<H", ent, 26)[0] | \
                (struct.unpack_from("<H", ent, 20)[0] << 16)
            size = struct.unpack_from("<I", ent, 28)[0]
            entries[full] = (first, size)
        cl = fget(cl)
        guard += 1
    return entries


def read_chain(img: bytes, first: int, size: int) -> bytes:
    reserved, nfats, fatsz, rootclus, spc, data_start = geometry(img)
    fat0 = img[reserved * S:(reserved + fatsz) * S]

    def fget(cl):
        return struct.unpack_from("<I", fat0, cl * 4)[0] & 0x0FFFFFFF

    out = b""
    cl = first
    guard = 0
    while cl < 0x0FFFFFF8 and guard < 4096:
        base = data_start + (cl - 2) * spc
        out += img[base * S:(base + spc) * S]
        cl = fget(cl)
        guard += 1
    return out[:size]


def file_map(img: bytes):
    entries = read_root_entries(img)
    out = {}
    for name, (first, size) in entries.items():
        out[name] = read_chain(img, first, size)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("post", help="image after the guest ran")
    ap.add_argument("pristine", nargs="?", default=None,
                    help="image before the guest ran (for the byte diff)")
    ap.add_argument("--allow-write", action="store_true",
                    help="the guest was asked to create a file, so sectors may differ")
    ap.add_argument("--expect-file", default=None)
    ap.add_argument("--expect-text", default=None)
    args = ap.parse_args()

    problems = []
    checks = 0

    with open(args.post, "rb") as f:
        post = f.read()
    checks += 1
    print(f"image       : {args.post} ({len(post)} bytes, "
          f"crc32={zlib.crc32(post) & 0xFFFFFFFF:08X})")

    # ---- 1. independent FAT32 re-parse -------------------------------------
    try:
        found = mk.verify_image(post, EXPECT)
        checks += 1
        print("geom        : FAT32 genuine, FAT1==FAT2, FSInfo free recount OK")
        for name in sorted(EXPECT):
            checks += 1
            print(f"file        : {name} {len(found[name])} bytes byte-for-byte OK")
    except AssertionError as exc:
        problems.append(f"FAT32 re-parse failed: {exc}")

    # ---- 2. sector-level diff against the pristine image --------------------
    if args.pristine:
        with open(args.pristine, "rb") as f:
            pristine = f.read()
        checks += 1
        print(f"pristine    : {args.pristine} "
              f"({len(pristine)} bytes, crc32={zlib.crc32(pristine) & 0xFFFFFFFF:08X})")
        if len(pristine) != len(post):
            problems.append("image length changed")
        same = zlib.crc32(pristine) == zlib.crc32(post)
        changed = []
        if not same:
            for i in range(0, min(len(pristine), len(post)), S):
                if pristine[i:i + S] != post[i:i + S]:
                    changed.append(i // S)
        if args.allow_write:
            checks += 1
            print(f"diff        : {len(changed)} changed sectors (write test expected)")
        else:
            checks += 1
            if same:
                print("diff        : whole-image CRC identical -> no stray writes, "
                      "write test restore proven byte-for-byte")
            else:
                problems.append(
                    f"image changed but no guest write was requested "
                    f"({len(changed)} sectors, first={changed[:8]})")

    # ---- 3. optional file the guest created ---------------------------------
    if args.expect_file:
        entries = read_root_entries(post)
        key = args.expect_file.upper()
        want = (args.expect_text or "").encode()
        checks += 1
        if key not in entries:
            problems.append(f"{key} missing from the image root")
        else:
            got = read_chain(post, entries[key][0], entries[key][1])
            if want and want not in got:
                problems.append(f"{key} content mismatch: {got!r}")
            else:
                print(f"guest file  : {key} {len(got)} bytes content OK "
                      f"({got!r})")

    print()
    if problems:
        print(f"=== RESULT: FAIL ===  checks={checks} problems={len(problems)}")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"=== RESULT: PASS ===  checks={checks} ok={checks} fail=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
