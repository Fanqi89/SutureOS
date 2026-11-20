#!/usr/bin/env python3
"""SutureOS selfboot disk builder.

Builds the second, fully self-written boot path of SutureOS (the primary path
stays Limine + multiboot1).  Produces build/selfboot.img, a raw BIOS disk:

    LBA 0        stage 1  (boot/selfboot/stage1.S, 512-byte MBR)
    LBA 1..32    stage 2  (boot/selfboot/stage2.S, fixed 16 KiB)
    LBA 33..     kernel.flat  (objcopy -O binary of build/kernel.elf)

stage 2 clears the kernel .bss, builds the identity page tables at the
pml4/pdpt/pd0/pd_io addresses exported by boot/boot.S, enters x86_64 long
mode and jumps to boot.S's `_start64` with eax/edi = 0x2BADB002 and
ebx/esi = multiboot1 info (framebuffer fields included).

The multiboot1 contract is byte-for-byte the one Limine provides, so the same
kernel image boots from either chain and nothing in the kernel changes.

Run:  py -3 scripts/build_boot_disk.py
Out:  build/selfboot.img + build/selfboot_manifest.json
"""
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get("SUTUREOS_ROOT", Path(__file__).resolve().parent.parent))
BUILD = ROOT / "build"
SRC = ROOT / "boot" / "selfboot"
KERNEL_ELF = BUILD / "kernel.elf"
IMAGE = BUILD / "selfboot.img"
MANIFEST = BUILD / "selfboot_manifest.json"

CROSS = ROOT / "tools" / "cross" / "bin"
AS = CROSS / "x86_64-elf-as.exe"
LD = CROSS / "x86_64-elf-ld.exe"
OBJCOPY = CROSS / "x86_64-elf-objcopy.exe"
NM = CROSS / "x86_64-elf-nm.exe"

STAGE1_ADDR = 0x7C00
STAGE2_ADDR = 0x7E00
STAGE2_SECTORS = 32
STAGE2_SIZE = STAGE2_SECTORS * 512
KERNEL_LBA = 1 + STAGE2_SECTORS
MBI_ADDR = 0x6000
SECTOR = 512

# stage-2 header offsets (see boot/selfboot/stage2.S)
HDR_MAGIC = 4
HDR_ENTRY64 = 8
HDR_KSECT = 12
HDR_KLBA = 16
HDR_MBI = 20
HDR_BSS_START = 24
HDR_BSS_END = 28
HDR_PML4 = 32
HDR_PDPT = 36
HDR_PD0 = 40
HDR_PD_IO = 44


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode("utf-8", "replace"))
        sys.stderr.write(r.stderr.decode("utf-8", "replace"))
        raise SystemExit("command failed: %s" % (cmd,))
    return r.stdout


def assemble(src_name, entry, out_name, ld_script=None):
    obj = BUILD / (out_name + ".o")
    binf = BUILD / out_name
    run([AS, "--32", "-o", obj, SRC / src_name])
    cmd = [LD, "-m", "elf_i386"]
    if ld_script:
        cmd += ["-T", ld_script]
    else:
        cmd += ["-Ttext", hex(entry)]
    cmd += ["--oformat", "binary", "-o", binf, obj]
    run(cmd)
    return binf


def symbols(elf):
    out = run([NM, "-a", elf]).decode("utf-8", "replace")
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                syms[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return syms


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def patch_u32(buf, off, value):
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def main():
    if not KERNEL_ELF.exists():
        raise SystemExit("missing build/kernel.elf - run py -3 scripts/build.py first")

    # 1. flat kernel image (the multiboot1-loader view of the kernel)
    kernel_flat = BUILD / "kernel.flat"
    run([OBJCOPY, "-O", "binary", KERNEL_ELF, kernel_flat])
    kernel_data = kernel_flat.read_bytes()
    if len(kernel_data) < SECTOR:
        raise SystemExit("kernel.flat is suspiciously small: %d bytes" % len(kernel_data))
    kernel_data += b"\x00" * ((SECTOR - len(kernel_data) % SECTOR) % SECTOR)
    ksect = len(kernel_data) // SECTOR

    # 2. kernel symbols the boot chain needs
    syms = symbols(KERNEL_ELF)
    need = ["_start64", "__bss_start", "__bss_end", "pml4", "pdpt", "pd0", "pd_io"]
    missing = [s for s in need if s not in syms]
    if missing:
        raise SystemExit("kernel.elf is missing symbols: %s" % ", ".join(missing))
    entry64 = syms["_start64"]
    if entry64 < 0x100000:
        raise SystemExit("_start64 (0x%x) is below the 1 MiB load address" % entry64)

    # 3. stage 1 / stage 2
    stage1 = assemble("stage1.S", STAGE1_ADDR, "selfboot_stage1.bin")
    stage2 = assemble("stage2.S", STAGE2_ADDR, "selfboot_stage2.bin",
                      ld_script=SRC / "stage2.ld")
    s1 = stage1.read_bytes()
    s2 = bytearray(stage2.read_bytes())
    if len(s1) != 512:
        raise SystemExit("stage 1 must be exactly 512 bytes, got %d" % len(s1))
    if s1[-2:] != b"\x55\xaa":
        raise SystemExit("stage 1 has no 0xAA55 boot signature")
    if len(s2) > STAGE2_SIZE:
        raise SystemExit("stage 2 is %d bytes, slot is %d" % (len(s2), STAGE2_SIZE))

    # 4. patch the stage-2 header
    patch_u32(s2, HDR_MAGIC, 0x53423032)      # 'SB02'
    patch_u32(s2, HDR_ENTRY64, entry64)
    patch_u32(s2, HDR_KSECT, ksect)
    patch_u32(s2, HDR_KLBA, KERNEL_LBA)
    patch_u32(s2, HDR_MBI, MBI_ADDR)
    patch_u32(s2, HDR_BSS_START, syms["__bss_start"])
    patch_u32(s2, HDR_BSS_END, syms["__bss_end"])
    patch_u32(s2, HDR_PML4, syms["pml4"])
    patch_u32(s2, HDR_PDPT, syms["pdpt"])
    patch_u32(s2, HDR_PD0, syms["pd0"])
    patch_u32(s2, HDR_PD_IO, syms["pd_io"])
    s2 = bytes(s2) + b"\x00" * (STAGE2_SIZE - len(s2))

    # 5. raw disk image
    image = s1 + s2 + kernel_data
    IMAGE.write_bytes(image)

    manifest = {
        "image": IMAGE.name,
        "image_bytes": len(image),
        "image_sha256": sha256(image),
        "layout": {
            "stage1_lba": 0,
            "stage1_bytes": len(s1),
            "stage1_sha256": sha256(s1),
            "stage2_lba": 1,
            "stage2_sectors": STAGE2_SECTORS,
            "stage2_bytes": STAGE2_SIZE,
            "stage2_sha256": sha256(s2),
            "kernel_lba": KERNEL_LBA,
            "kernel_sectors": ksect,
            "kernel_flat_bytes": len(kernel_data),
            "kernel_flat_sha256": sha256(kernel_data),
        },
        "kernel_symbols": {
            "entry64": hex(entry64),
            "bss_start": hex(syms["__bss_start"]),
            "bss_end": hex(syms["__bss_end"]),
            "pml4": hex(syms["pml4"]),
            "pdpt": hex(syms["pdpt"]),
            "pd0": hex(syms["pd0"]),
            "pd_io": hex(syms["pd_io"]),
        },
    }
    MANIFEST.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print("selfboot.img: %d bytes (%.2f MiB)" % (len(image), len(image) / 1048576.0))
    print("  stage1     512 B  sha256=%s" % manifest["layout"]["stage1_sha256"][:16])
    print("  stage2   %6d B  sha256=%s" % (STAGE2_SIZE, manifest["layout"]["stage2_sha256"][:16]))
    print("  kernel   %6d B x %d sectors at LBA %d" % (len(kernel_data), ksect, KERNEL_LBA))
    print("  kernel.flat sha256=%s" % manifest["layout"]["kernel_flat_sha256"][:16])
    print("  entry64=%s  bss=[%s..%s)  pml4=%s" % (
        hex(entry64), hex(syms["__bss_start"]), hex(syms["__bss_end"]), hex(syms["pml4"])))
    print("  image sha256=%s" % manifest["image_sha256"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
