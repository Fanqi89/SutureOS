# StitchOS (缝合怪系统) - static rejection check: symbol collision scan
# Scans all compiled objects for globally-defined symbols appearing in
# more than one object file (排异检查-静态：符号冲突).
param()
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\fanqi\Desktop\缝合怪系统'
$obj  = Join-Path $root 'build\obj'
$nmCandidates = @(
    (Join-Path $root 'tools\cross\bin\x86_64-elf-nm.exe'),
    'C:\Users\fanqi\AppData\Local\Temp\xb64\install\bin\x86_64-elf-nm.exe',
    'C:\msys64\mingw64\bin\nm.exe'
)
$nm = $nmCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $nm) { throw 'nm not found' }

$map = @{}   # symbol -> [list of objects]
$files = Get-ChildItem -Path (Join-Path $obj '*.o') -ErrorAction SilentlyContinue
if (-not $files) { throw 'no object files in build/obj' }

foreach ($f in $files) {
    $out = & $nm --defined-only $f.FullName 2>$null
    foreach ($line in $out) {
        if ($line -match '^\S{8,16}\s+([A-Za-z])\s+(\S+)$') {
            $type = $Matches[1]; $sym = $Matches[2]
            # ignore local/debug types; count only global-ish definitions
            if ($type -match '[tTdDbBrRwW]') {
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
