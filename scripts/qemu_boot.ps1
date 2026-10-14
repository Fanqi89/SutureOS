<#
QEMU boot harness for SutureOS (host-side, Windows).

Why this exists: the kernel's serial log is opened with `-serial file:...`, and
QEMU only flushes the tail of that file on a clean exit. Killing the process
loses the last lines and has already produced one wrong conclusion, so every
automated boot must end with `quit`.

This script
  1. starts qemu-system-x86_64.exe with the project root as the working
     directory (QEMU cannot open non-ASCII absolute paths),
  2. exposes the QMP monitor on a loopback TCP port (QMP is used instead of the
     human monitor because its replies are clean JSON without readline echo),
  3. optionally types shell commands with `sendkey` (space = spc, '.' = dot,
     '/' = slash, enter = ret),
  4. optionally runs raw human-monitor commands and prints their replies,
  5. always ends with `quit` and waits for the process to exit.

Usage:
  powershell -NoProfile -File scripts/qemu_boot.ps1 -SerialLog build\serial.log `
      -ArgFile build\qemu_virtio.args `
      -Keys "ping spc 1 0 dot 0 dot 2 dot 2 ret" `
      -MonitorCmds "info pci;i /4 0xc100"

Because `-File` hands every argument over as a single string, -Keys and
-MonitorCmds are ';'-separated lists.
#>
param(
    [Parameter(Mandatory = $true)][string]$SerialLog,
    [Parameter(Mandatory = $true)][string]$ArgFile,
    [string]$Keys = '',
    [string]$MonitorCmds = '',
    [int]$BootWaitSeconds = 12,
    [int]$KeyDelayMs = 400,
    [int]$AfterKeysSeconds = 8,
    [int]$MonitorPort = 45554
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'

if (-not (Test-Path $qemu)) { throw "QEMU not found at $qemu" }
$serialAbs = Join-Path $root $SerialLog
if (Test-Path $serialAbs) { Remove-Item $serialAbs -Force }

$extra = Get-Content (Join-Path $root $ArgFile) |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') }

$qargs = @('-machine', 'pc', '-m', '256', '-display', 'none',
           '-serial', "file:$SerialLog",
           '-qmp', "tcp:127.0.0.1:$MonitorPort,server,nowait") + $extra

Write-Host "[qemu] $qemu $($qargs -join ' ')"
$proc = Start-Process -FilePath $qemu -ArgumentList $qargs -WorkingDirectory $root -PassThru

# ---- minimal QMP client -----------------------------------------------------
$client = $null
$stream = $null
$reader = $null

function Qmp-Connect {
    $script:client = New-Object System.Net.Sockets.TcpClient
    $script:client.Connect('127.0.0.1', $MonitorPort)
    $script:stream = $script:client.GetStream()
    $script:stream.ReadTimeout = 2000
    $script:reader = New-Object System.IO.StreamReader($script:stream, [System.Text.Encoding]::ASCII)
    $null = $script:reader.ReadLine()          # exactly one greeting line {"QMP": ...}
    Send-Qmp 'qmp_capabilities' | Out-Null
}

function Send-Qmp([string]$execute, $arguments = $null) {
    $obj = @{ execute = $execute }
    if ($arguments) { $obj['arguments'] = $arguments }
    $json = $obj | ConvertTo-Json -Compress -Depth 6
    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$json`n")
    $script:stream.Write($bytes, 0, $bytes.Length)
    $script:stream.Flush()
    while ($true) {
        $line = $script:reader.ReadLine()
        if ($null -eq $line) { return $null }
        if ($line -match '"event"') { continue }        # skip async events
        return ($line | ConvertFrom-Json)
    }
}

function Monitor-Cmd([string]$cmd) {
    $r = Send-Qmp 'human-monitor-command' @{ 'command-line' = $cmd }
    if ($r -and $r.return) { return ($r.return -replace "`r", '').Trim() }
    return ''
}

try {
    $connected = $false
    $lastErr = ''
    for ($i = 0; $i -lt 40 -and -not $connected; $i++) {
        try { Qmp-Connect; $connected = $true } catch { $lastErr = "$($_.Exception.Message)"; Start-Sleep -Milliseconds 250 }
    }
    if (-not $connected) { throw "could not connect to QEMU QMP on port $MonitorPort (last error: $lastErr)" }

    Start-Sleep -Seconds $BootWaitSeconds

    $keyList = @($Keys -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    foreach ($k in $keyList) {
        Write-Host "[qemu] sendkey $k"
        Monitor-Cmd "sendkey $k" | Out-Null
        Start-Sleep -Milliseconds $KeyDelayMs
    }
    if ($keyList.Count -gt 0) { Start-Sleep -Seconds $AfterKeysSeconds }

    $cmdList = @($MonitorCmds -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    foreach ($c in $cmdList) {
        Write-Host "[monitor] $c"
        $out = Monitor-Cmd $c
        if ($out) { $out -split "`n" | ForEach-Object { "  $_" } }
    }
} finally {
    try { Send-Qmp 'quit' | Out-Null } catch { Write-Warning "QMP quit failed: $_" }
    if ($client) { try { $client.Close() } catch { } }
    if (-not $proc.HasExited) {
        if (-not $proc.WaitForExit(15000)) {
            Write-Warning "QEMU did not exit after quit; killing"
            $proc.Kill()
        }
    }
}

Write-Host "[qemu] exited (code $($proc.ExitCode))"
if (Test-Path $serialAbs) {
    $len = (Get-Item $serialAbs).Length
    Write-Host "[qemu] serial log $SerialLog : $len bytes"
}
