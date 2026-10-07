# SutureOS ISO build script
# Stages a boot tree with professional directory layout:
#   /boot/kernel.elf        - Kernel binary
#   /boot/limine/limine.conf - Limine config
#   /boot/limine/limine-bios.sys - Limine BIOS stage
#   /limine/limine.conf      - Limine config (BIOS fallback at root)
#   /limine/limine-bios.sys  - Limine BIOS stage (BIOS fallback)
#   /EFI/BOOT/BOOTX64.EFI    - UEFI bootloader
# Builds a BIOS+UEFI hybrid ISO with xorriso, then installs Limine BIOS stage.
# ASCII-only comments.
param()
$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
Set-Location $root
$limineBin = Join-Path $root 'tools\limine\limine-binary'
$stage    = Join-Path $root 'build\iso'
$kernel   = Join-Path $root 'build\kernel.elf'
$iso      = Join-Path $root 'build\sutureos.iso'
$xorriso  = 'C:\msys64\usr\bin\xorriso.exe'
$limineExe = Join-Path $limineBin 'limine-tool-windows-x86\limine.exe'

if (-not (Test-Path $kernel))  { throw 'build\kernel.elf not found - run scripts\build.ps1 first' }
if (-not (Test-Path $xorriso)) { throw "xorriso not found at $xorriso" }
if (-not (Test-Path $limineExe)) { throw "limine.exe not found at $limineExe" }

# --- 1. Stage the ISO root with professional layout ----------------------
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'boot\limine') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'system')      | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'limine')      | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'EFI\BOOT')    | Out-Null

# Kernel at /boot/kernel.elf
Copy-Item $kernel (Join-Path $stage 'boot\kernel.elf')

# Limine config at /boot/limine/limine.conf (primary) and /limine/limine.conf (fallback)
Copy-Item (Join-Path $root 'boot\limine.conf') (Join-Path $stage 'boot\limine\limine.conf')
Copy-Item (Join-Path $root 'boot\limine.conf') (Join-Path $stage 'limine\limine.conf')

# Limine BIOS stage at /boot/limine/ and /limine/ (for BIOS compatibility)
Copy-Item (Join-Path $limineBin 'limine-bios.sys') (Join-Path $stage 'boot\limine\limine-bios.sys')
Copy-Item (Join-Path $limineBin 'limine-bios.sys') (Join-Path $stage 'limine\limine-bios.sys')

# El Torito boot images at ISO root (required by xorriso -b)
Copy-Item (Join-Path $limineBin 'limine-bios-cd.bin') (Join-Path $stage 'limine-bios-cd.bin')
Copy-Item (Join-Path $limineBin 'limine-uefi-cd.bin') (Join-Path $stage 'limine-uefi-cd.bin')

# UEFI bootloader
Copy-Item (Join-Path $limineBin 'BOOTX64.EFI') (Join-Path $stage 'EFI\BOOT\BOOTX64.EFI')

# --- 2. Build the hybrid ISO (exact flags from Limine USAGE.md) ---------
if (Test-Path $iso) { Remove-Item $iso -Force }

# Forward-slash RELATIVE paths: MSYS2 xorriso resolves against cwd=$root
& $xorriso -as mkisofs -R -r -J `
    -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table `
    -hfsplus -apm-block-size 2048 `
    --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image `
    --protective-msdos-label build/iso -o build/sutureos.iso
if ($LASTEXITCODE -ne 0) { throw "xorriso failed (exit $LASTEXITCODE)" }

# --- 3. Install the Limine BIOS stage into the image --------------------
& $limineExe bios-install build\sutureos.iso
if ($LASTEXITCODE -ne 0) { throw "limine bios-install failed (exit $LASTEXITCODE)" }

$size = (Get-Item $iso).Length
Write-Host "ISO OK -> build\sutureos.iso ($size bytes)"