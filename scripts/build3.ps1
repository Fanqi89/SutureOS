# SutureOS build - direct execution version
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\fanqi\Desktop\缝合怪系统'
$cc = 'C:/msys64/mingw64/bin/clang.exe'
$ld = 'C:/Users/fanqi/AppData/Local/Temp/xb64/install/bin/x86_64-elf-ld.exe'

$resourceDir = (& $cc -print-resource-dir).Trim()
$cflags = @(
    '--target=x86_64-unknown-none-elf',
    '-ffreestanding', '-fno-builtin',
    '-mno-red-zone', '-mno-sse', '-mno-mmx', '-mno-80387',
    '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
    '-nostdinc', '-isystem', "$resourceDir\include",
    '-Iinclude', '-Iarch\x86_64', '-Ikernel\interrupts',
    '-O2', '-g', '-Wall', '-Wextra'
)

$sources = @()
$sources += Get-ChildItem -Path (Join-Path $root 'boot') -Filter '*.S' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'arch\x86_64') -Filter '*.c' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'arch\x86_64') -Filter '*.S' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'kernel') -Filter '*.c' -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'kernel') -Filter '*.S' -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources += Get-ChildItem -Path (Join-Path $root 'lib') -Filter '*.c' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$sources = $sources | Sort-Object -Unique
if ($sources.Count -eq 0) { throw 'no sources found' }

$objs = @()
$fail = 0
foreach ($s in $sources) {
    $rel = $s.Substring($root.Length + 1)
    $flat = ($rel -replace '[\\/]', '__')
    $o = Join-Path 'build\obj' ($flat + '.o')
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

$elf = Join-Path 'build' 'kernel.elf'
$ldArgs = @('-T', 'boot/linker.ld', '-o', $elf) + $objs
& $ld @ldArgs
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }

Write-Host "BUILD OK -> $elf" -ForegroundColor Green
& $ld --version | Select-Object -First 1 | Out-Host
Get-Item $elf | ForEach-Object { "kernel.elf: $($_.Length) bytes" }