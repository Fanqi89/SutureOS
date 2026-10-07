# StitchOS - prepare the x86_64-elf cross toolchain into tools/cross/bin
# Source 1: existing temp build (C:\Users\fanqi\AppData\Local\Temp\xb64\install)
# Source 2: rebuild from GNU mirror (see docs/build-and-test.md §2)
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\fanqi\Desktop\缝合怪系统'
$dest = Join-Path $root 'tools\cross\bin'
$src  = 'C:\Users\fanqi\AppData\Local\Temp\xb64\install\bin'

if (Test-Path (Join-Path $dest 'x86_64-elf-ld.exe')) {
    Write-Host "toolchain already present: $dest" -ForegroundColor Green
    exit 0
}
if (-not (Test-Path (Join-Path $src 'x86_64-elf-ld.exe'))) {
    throw "no toolchain found at $src - run the binutils bootstrap in docs/build-and-test.md first"
}
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item -Path (Join-Path $src 'x86_64-elf-*.exe') -Destination $dest
Write-Host "installed -> $dest" -ForegroundColor Green
Get-ChildItem $dest | Select-Object -ExpandProperty Name
