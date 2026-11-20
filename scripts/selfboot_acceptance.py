#!/usr/bin/env python3
"""SutureOS self-written BIOS boot chain acceptance (stage 1 + stage 2).

Proves the second, fully self-written boot path actually boots the SAME x86_64
kernel as the Limine path:

  * host side (independent, byte-exact): rebuilds the flat kernel from
    build/kernel.elf and byte-compares it with the image payload, re-reads the
    stage-1 / stage-2 headers and re-derives entry64/ksect/klba from the ELF
    symbol table - the image is not trusted on its own manifest.
  * guest side: QEMU boots build/selfboot.img with NO CD-ROM and NO Limine,
    and the serial log must show stage 2, the VBE mode, the long-mode hand-off,
    the multiboot magic, the framebuffer and the full self-test summary
    (16/16, i.e. the same count as the Limine path) with no PANIC.

  py -3 scripts/build_boot_disk.py     # (re)build the image first
  py -3 scripts/selfboot_acceptance.py
"""
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
IMAGE = os.path.join(BUILD, 'selfboot.img')
MANIFEST = os.path.join(BUILD, 'selfboot_manifest.json')
KERNEL_ELF = os.path.join(BUILD, 'kernel.elf')
ARGS = os.path.join(BUILD, 'qemu_selfboot.args')
LOG = os.path.join(BUILD, 'serial_selfboot.log')
OBJCOPY = os.path.join(ROOT, 'tools', 'cross', 'bin', 'x86_64-elf-objcopy.exe')
NM = os.path.join(ROOT, 'tools', 'cross', 'bin', 'x86_64-elf-nm.exe')

SECTOR = 512
STAGE2_SECTORS = 32
STAGE2_SIZE = STAGE2_SECTORS * SECTOR
KERNEL_LBA = 1 + STAGE2_SECTORS


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def nm_symbols(elf):
    out = subprocess.run([NM, '-a', elf], capture_output=True).stdout.decode('utf-8', 'replace')
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                syms[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return syms


def host_checks(checks):
    if not os.path.exists(IMAGE):
        checks.append(('selfboot.img exists', False))
        return
    manifest = json.load(open(MANIFEST, encoding='utf-8'))
    image = open(IMAGE, 'rb').read()

    checks.append(('image size matches manifest', len(image) == manifest['image_bytes']))
    checks.append(('image sha256 matches manifest', sha256(image) == manifest['image_sha256']))

    stage1 = image[:SECTOR]
    checks.append(('stage 1 is 512 bytes with 0xAA55', len(stage1) == 512 and stage1[-2:] == b'\x55\xaa'))

    stage2 = image[SECTOR:SECTOR + STAGE2_SIZE]
    checks.append(('stage 2 slot is 16 KiB', len(stage2) == STAGE2_SIZE))
    checks.append(('stage 2 magic is SB02', struct.unpack_from('<I', stage2, 4)[0] == 0x53423032))
    checks.append(('stage 2 JMP opcode first', stage2[0] in (0xEB, 0xE9)))

    kernel = image[SECTOR * KERNEL_LBA:]
    exp = subprocess.run([OBJCOPY, '-O', 'binary', KERNEL_ELF, os.path.join(BUILD, 'kernel.flat')],
                         capture_output=True)
    checks.append(('objcopy of kernel.elf succeeds', exp.returncode == 0))
    flat = open(os.path.join(BUILD, 'kernel.flat'), 'rb').read()
    flat_padded = flat + b'\x00' * ((SECTOR - len(flat) % SECTOR) % SECTOR)
    checks.append(('image payload == independent objcopy(kernel.elf)',
                   kernel == flat_padded))
    checks.append(('kernel payload sha256 matches manifest',
                   sha256(flat_padded) == manifest['layout']['kernel_flat_sha256']))

    syms = nm_symbols(KERNEL_ELF)
    entry = struct.unpack_from('<I', stage2, 8)[0]
    ksect = struct.unpack_from('<I', stage2, 12)[0]
    klba = struct.unpack_from('<I', stage2, 16)[0]
    bss_start = struct.unpack_from('<I', stage2, 24)[0]
    bss_end = struct.unpack_from('<I', stage2, 28)[0]
    pml4 = struct.unpack_from('<I', stage2, 32)[0]
    checks.append(('patched entry64 == kernel _start64', entry == syms.get('_start64')))
    checks.append(('patched ksect == payload sectors', ksect == len(flat_padded) // SECTOR))
    checks.append(('patched kernel LBA is 33', klba == KERNEL_LBA))
    checks.append(('patched bss range is the kernel __bss_start/end',
                   bss_start == syms.get('__bss_start') and bss_end == syms.get('__bss_end')))
    checks.append(('patched page-table addresses are kernel pml4', pml4 == syms.get('pml4')))
    checks.append(('_start64 is a separate entry above _start',
                   syms.get('_start64', 0) > 0x100000 and syms.get('_start64') != syms.get('_start')))

    # the flat payload must begin exactly at the lowest kernel LMA (1 MiB)
    checks.append(('kernel payload starts with the multiboot1 magic 0x1BADB002',
                   struct.unpack_from('<I', flat, 0)[0] == 0x1BADB002))


def guest_checks(checks):
    log = open(LOG, encoding='utf-8', errors='replace').read()
    want = [
        ('stage 2 ran', '[selfboot] stage2 @0x7E00'),
        ('VBE 1024x768x32 linear framebuffer', '[selfboot] vbe 1024x768x32 linear framebuffer ok'),
        ('kernel image loaded via ATA PIO',
         '[selfboot] entering protected mode, loading kernel via ATA PIO'),
        ('kernel sees multiboot magic', '[boot] multiboot magic OK'),
        ('kernel framebuffer hand-off', '[fb] ready: 1024x768 32 bpp'),
        ('console reaches the shell/selftest tail', '[stitch] selftests:'),
        ('kernel reached the interactive shell', 'sutureos# '),
    ]
    for name, needle in want:
        checks.append((name, needle in log))

    m = re.search(r'\[stitch\] selftests: (\d+)/(\d+) passed', log)
    if m:
        passed, total = int(m.group(1)), int(m.group(2))
        checks.append(('self-tests did not regress (%d/%d, baseline 16/16)' % (passed, total),
                       total == 16 and passed >= 16))
    else:
        checks.append(('self-tests did not regress (baseline 16/16)', False))
    checks.append(('no kernel PANIC', 'PANIC' not in log.upper().replace('NO PANIC', '')))


def main():
    checks = []
    host_checks(checks)

    if os.path.exists(LOG):
        os.remove(LOG)
    with open(ARGS, 'w', encoding='ascii', newline='') as fh:
        fh.write('-drive file=build/selfboot.img,format=raw,if=ide,index=0 -boot c '
                 '-smp 2 -no-reboot\n')
    subprocess.run(['powershell', '-NoProfile', '-File',
                    os.path.join(ROOT, 'scripts', 'qemu_boot.ps1'),
                    '-SerialLog', 'build/serial_selfboot.log',
                    '-ArgFile', 'build/qemu_selfboot.args',
                    '-BootWaitSeconds', '30', '-MonitorPort', '45631'],
                   cwd=ROOT, check=False)
    guest_checks(checks)

    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    print('[selfboot] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[selfboot] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
