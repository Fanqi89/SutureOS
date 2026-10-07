# SutureOS (缝合怪系统) build script - simplified version
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = 'C:\Users\fanqi\Desktop\缝合怪系统'
$cc   = 'C:/msys64/mingw64/bin/clang.exe'
if (-not (Test-Path $cc)) { throw "clang not found: $cc" }

# ELF linker
$ldCandidates = @(
    (Join-Path $root 'tools/cross/bin/x86_64-elf-ld.exe'),
    'C:/Users/fanqi/AppData/Local/Temp/xb64/install/bin/x86_64-elf-ld.exe'
)
$ld = $ldCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ld) { throw "x86_64-elf-ld not found. Run scripts/fetch-toolchain first." }

$build = Join-Path $root 'build'
$obj   = Join-Path $build 'obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item $obj -Recurse -Force }
New-Item -ItemType Directory -Path $obj -Force | Out-Null

$resourceDir = (& $cc -print-resource-dir).Trim()
$cflags = @(
    '--target=x86_64-unknown-none-elf',
    '-ffreestanding', '-fno-builtin',
    '-mno-red-zone', '-mno-sse', '-mno-mmx', '-mno-80387',
    '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
    '-nostdinc', '-isystem', "$resourceDir\include",
    "-I$(Join-Path $root 'include')",
    "-I$(Join-Path $root 'arch\x86_64')",
    "-I$(Join-Path $root 'kernel\interrupts')",
    '-O2', '-g', '-Wall', '-Wextra'
)

# ---- collect sources ----
$sources = @()
$sources += Get-ChildItem -Path (Join-Path $root 'boot') -Filter '*.S' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'arch\x86_64') -Filter '*.c' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'arch\x86_64') -Filter '*.S' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'kernel') -Filter '*.c' -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'kernel') -Filter '*.S' -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'lib') -Filter '*.c' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources = $sources | Sort-Object -Unique
if ($sources.Count -eq 0) { throw 'no sources found' }

# ---- compile ----
$objs = @()
$fail = 0
foreach ($s in $sources) {
    $rel = $s.Substring($root.Length + 1)
    $flat = ($rel -replace '[\\/]', '__')
    $o = Join-Path $obj ($flat + '.o')
    $ext = [IO.Path]::GetExtension($s).ToLower()
    $args = @('-c')
    $args += $cflags
    $args += '-o', $o, $s
    & $cc @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host "COMPILE FAIL: $rel" -ForegroundColor Red
        $fail++
    } else {
        $objs += $o
    }
}
if ($fail -gt 0) { throw "$fail file(s) failed to compile" }

# ---- link ----
$elf = Join-Path $build 'kernel.elf'
$ldArgs = @('-T', (Join-Path $root 'boot\linker.ld'), '-o', $elf) + $objs
& $ld @ldArgs
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }

Write-Host "BUILD OK -> $elf" -ForegroundColor Green
& $ld --version | Select-Object -First 1 | Out-Host
Get-Item $elf | ForEach-Object { "kernel.elf: $($_.Length) bytes" }