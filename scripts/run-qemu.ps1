# SutureOS QEMU run/verify script
# Boots the Limine hybrid ISO (build\sutureos.iso) and watches COM1 for the
# self-test milestones. All QEMU paths are RELATIVE with working directory =
# project root: QEMU cannot open absolute paths containing non-ASCII chars
# (the project dir name), proven by earlier failures.
param(
    [int]$TimeoutSeconds = 30,
    [int]$MemoryMB = 256
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$qemu   = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$iso    = 'build\sutureos.iso'      # relative to $root
$serial = Join-Path $root 'build\serial.log'

if (-not (Test-Path (Join-Path $root $iso))) { throw "sutureos.iso not found - run scripts\make-iso.ps1 first" }
# A stale QEMU from an aborted run can hold serial.log; reap it first.
Get-Process 'qemu-system-x86_64' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
if (Test-Path $serial) { Remove-Item $serial -Force -ErrorAction SilentlyContinue }

$p = Start-Process -FilePath $qemu -WorkingDirectory $root -ArgumentList @(
    '-machine', 'pc',
    '-m', "$MemoryMB",
    '-display', 'none',
    '-serial', 'file:build\serial.log',
    '-cdrom', $iso,
    '-boot', 'd',
    '-no-reboot', '-no-shutdown'
) -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$done = $false
# Read serial.log through a shared stream: QEMU keeps the file open
# exclusively while running, so [IO.File]::ReadAllBytes would throw.
function Read-Serial {
    if (-not (Test-Path $serial)) { return $null }
    try {
        $fs = [IO.File]::Open($serial, 'Open', 'Read', 'ReadWrite')
        try {
            $buf = New-Object byte[] $fs.Length
            $null = $fs.Read($buf, 0, $buf.Length)
            return [Text.Encoding]::ASCII.GetString($buf)
        } finally { $fs.Dispose() }
    } catch { return $null }
}
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $txt = Read-Serial
    if ($txt -and $txt -match '\[stitch\] All self-tests passed. Halting') { $done = $true; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 300

$out = Read-Serial
if ($out) {
    Write-Host '---------- serial output ----------'
    Write-Host $out
    Write-Host '-----------------------------------'
    $passLines = ([regex]::Matches($out, '\[selftest\]\[\w+\] PASS')).Count
    $anyFail   = $out -match '\[selftest\]\[\w+\] FAIL'
    if ($done -and -not $anyFail -and $passLines -ge 12 -and
        $out -match '\[stitch\] all self-tests PASSED' -and
        $out -match 'Halting') {
        Write-Host "SMOKE TEST PASS ($passLines self-tests passed)" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "SMOKE TEST FAIL (done=$done passLines=$passLines anyFail=$anyFail)" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host 'SMOKE TEST FAIL (no serial output)' -ForegroundColor Red
    exit 1
}
