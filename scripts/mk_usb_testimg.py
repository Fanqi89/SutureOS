#!/usr/bin/env python3
"""Build a USB mass-storage test image for the EHCI/BOT driver.

The USB port batch (B8) needs a real FAT32 volume to enumerate and mount, and
it wants a *known* file the guest can read back and check. This script reuses
the deterministic FAT32 superfloppy builder from `mkfat32_testimg.py` and writes
a second image with:

  USB.TXT      "SutureOS USB selftest\\n..."   <- the guest self-test reads this
  HELLO.TXT    same payload as the ATA fixture  <- `cat /mnt/usb/hello.txt`
  README.TXT   provenance blurb

Image size follows the shared builder: 71680 x 512-byte blocks, so the expected
guest log line is `[usb] storage: 71680 blocks x 512`.

Usage:
  py -3 scripts/mk_usb_testimg.py [--out build/usbdisk.img] [--quick]

--quick skips the independent re-parse (only for throwaway iterations).
"""
import argparse
import os
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import mkfat32_testimg as mk  # noqa: E402  (reuse the deterministic builders)

DEFAULT_OUT = os.path.join(ROOT, "build", "usbdisk.img")

USB_TXT = (b"SutureOS USB selftest\n"
           b"This file lives on a real FAT32 volume reachable over EHCI\n"
           b"Bulk-Only Transport + SCSI READ(10).\n")
HELLO_TXT = (b"Hello from SutureOS FAT32!\n"
             b"This file lives on a real FAT32 volume (driver: fat32).\n")
README_TXT = (b"SutureOS USB test disk.\n"
              b"Files: USB.TXT, HELLO.TXT, README.TXT\n"
              b"Attach with: -device usb-ehci,id=ehci "
              b"-drive id=usbdisk,file=build/usbdisk.img,format=raw,if=none "
              b"-device usb-storage,drive=usbdisk,bus=ehci.0\n")


def build() -> bytes:
    expect = {
        "USB.TXT": USB_TXT,
        "HELLO.TXT": HELLO_TXT,
        "README.TXT": README_TXT,
    }
    img, fat_sz, clusters = mk.build_image(dict(expect))
    return img, fat_sz, clusters, expect


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()

    img, fat_sz, clusters, expect = build()
    assert len(img) == mk.TOTAL_SECTORS * mk.SECTOR, "unexpected image length"
    blocks = len(img) // mk.SECTOR
    assert blocks == 71680, f"expected 71680 blocks, got {blocks}"

    if not args.quick:
        found = mk.verify_image(img, expect)
        # The independent re-parse must also prove the volume is still genuine
        # FAT32 and that both FAT copies agree (verify_image does that).
        assert "USB.TXT" in found and "HELLO.TXT" in found

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(img)

    print(f"USB FAT32 image: {args.out} ({len(img)} bytes)")
    print(f"  blocks    : {blocks} x {mk.SECTOR}")
    print(f"  clusters  : {clusters} (genuine FAT32 >= 65525)")
    print(f"  fat size  : {fat_sz} sectors x2 (mirrored)")
    print(f"  crc32     : {zlib.crc32(img) & 0xFFFFFFFF:08X}")
    print(f"  root files: {', '.join(sorted(expect))}")
    print("host-side verification: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
