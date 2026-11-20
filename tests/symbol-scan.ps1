# SutureOS - static rejection check: symbol collision scan
# Scans all compiled objects for globally-defined symbols appearing in
# more than one object file (排异检查-静态：符号冲突).
param()
$ErrorActionPreference = 'Stop'
# Derive the checkout from this script (tests/ lives one level below the
# repository root); the old hard-coded path pointed at the pre-rename directory
# and made the scan fail with "no object files" instead of reporting symbols.
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$obj  = Join-Path $root 'build\obj'
$nmCandidates = @(
    (Join-Path $root 'tools\cross\bin\x86_64-elf-nm.exe'),
    'C:\Users\fanqi\AppData\Local\Temp\xb64\install\bin\x86_64-elf-nm.exe',
    'C:\msys64\mingw64\bin\nm.exe'
)
$nm = $nmCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $nm) { throw 'nm not found' }

$map = @{}   # symbol -> [list of objects]
$files = Get-ChildItem -Path (Join-Path $obj '*.o') -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike 'userlib_*' -and $_.Name -notlike 'userapp_*' -and $_.Name -notlike '*.user.o' }
if (-not $files) { throw 'no object files in build/obj' }
# userlib_* objects are the ring-3 libc (user/lib) and userapp_* are the
# standalone user programs; both are linked into their own blobs, never into
# the kernel image, so their repeated names are outside this scan's scope.

foreach ($f in $files) {
    $out = & $nm --defined-only $f.FullName 2>$null
    foreach ($line in $out) {
        if ($line -match '^\S{8,16}\s+([A-Za-z])\s+(\S+)$') {
            $type = $Matches[1]; $sym = $Matches[2]
            # count only GLOBAL definitions (uppercase in nm); lowercase
            # t/d/b/r are file-local statics and may legitimately repeat
            if ($type -cmatch '[TDBRW]') {
                if (-not $map.ContainsKey($sym)) { $map[$sym] = @() }
                $map[$sym] += $f.Name
            }
        }
    }
}

$conflicts = $map.GetEnumerator() | Where-Object { $_.Value.Count -gt 1 } |
    Sort-Object Name

if ($conflicts) {
    Write-Host 'SYMBOL CONFLICTS FOUND (排异检查-静态 不通过):' -ForegroundColor Red
    foreach ($c in $conflicts) {
        Write-Host ("  {0} <= {1}" -f $c.Name, ($c.Value -join ', '))
    }
    exit 1
} else {
    $defined = ($map.GetEnumerator() | Where-Object { $_.Value.Count -eq 1 }).Count
    Write-Host "SYMBOL SCAN PASS: $defined global symbols, 0 collisions ($($files.Count) objects)" -ForegroundColor Green
    exit 0
}
