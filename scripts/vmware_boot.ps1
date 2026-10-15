<#
VMware Workstation boot harness for SutureOS (host-side, Windows).

Mirrors scripts/qemu_boot.ps1 for the other hypervisor the user tests on. It
creates a throwaway VM in build\ from a hand-written .vmx, attaches the ISO as
CD-ROM plus an optional raw disk image, captures the guest's COM1 to a file, and
then powers the VM off again - all without touching any VM the user already has
open (the VM is a separate .vmx inside the project's build directory).

Notes learned the hard way:
  * VMware needs a VMDK, not a raw image. A *descriptor-only* VMDK with a FLAT
    extent pointing at the raw file works ("RW <sectors> FLAT <file> 0"), and it
    has the nice property that guest writes land directly in that raw file, so
    the host-side verifiers (verify_fat32_write.py, verify_ext2_read.py) can read
    it unchanged.
  * bios.bootOrder = "cdrom" is required: a FAT32 superfloppy image has a valid
    0x55AA boot signature, so the BIOS would otherwise try to boot the data disk
    and the guest would hang with an empty serial log.
  * VMX comments start with '#' - a ';' comment makes Workstation refuse to read
    the configuration file ("cannot read the VM configuration file").

Usage:
  powershell -NoProfile -File scripts\vmware_boot.ps1 -SerialLog build\vmware_serial.log `
      -DiskImage build\fat32_ahci.img -WaitSeconds 60
#>
param(
    [Parameter(Mandatory = $true)][string]$SerialLog,
    [string]$DiskImage = '',
    [string]$Iso = 'build\sutureos.iso',
    [int]$WaitSeconds = 60,
    [int]$Memsize = 512,
    [int]$Cpus = 2,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$build = Join-Path $root 'build'
$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path $vmrun)) { throw "vmrun.exe not found at $vmrun" }

$vmxPath = Join-Path $build 'vmware_sutureos.vmx'
$isoAbs = Join-Path $root $Iso
if (-not (Test-Path $isoAbs)) { throw "ISO not found: $isoAbs" }
$serialAbs = Join-Path $root $SerialLog
if (Test-Path $serialAbs) { Remove-Item $serialAbs -Force }

# ---- optional SATA disk (raw image + flat-extent VMDK descriptor) -----------
$sataLines = @()
if ($DiskImage -ne '') {
    $imgAbs = Join-Path $root $DiskImage
    if (-not (Test-Path $imgAbs)) { throw "disk image not found: $imgAbs" }
    $imgName = Split-Path -Leaf $imgAbs
    $sectors = (Get-Item $imgAbs).Length / 512
    $vmdk = Join-Path $build ([IO.Path]::GetFileNameWithoutExtension($imgName) + '.vmdk')
    $vmdkText = @"
# Disk DescriptorFile
version=1
encoding="UTF-8"
CID=fffffffe
parentCID=ffffffff
createType="vmfs"

# Extent description: the raw image used directly as a flat extent, so guest
# writes land in the raw file and the host-side verifiers can read it.
RW $sectors FLAT "$imgName" 0

# The Disk Data Base
ddb.virtualHWVersion = "17"
ddb.adapterType = "sata"
"@
    Set-Content -Path $vmdk -Value $vmdkText -Encoding ascii
    $sataLines = @(
        'sata0.present = "TRUE"',
        'sata0:0.present = "TRUE"',
        "sata0:0.fileName = `"$(Split-Path -Leaf $vmdk)`"",
        'sata0:0.deviceType = "disk"',
        'sata0:0.mode = "independent-persistent"',
        'sata0:0.startConnected = "TRUE"'
    )
}

$vmx = @(
    '.encoding = "windows-1252"',
    'config.version = "8"',
    'virtualHW.version = "17"',
    'displayName = "SutureOS-test"',
    'guestOS = "other-64"',
    "memsize = `"$Memsize`"",
    "numvcpus = `"$Cpus`"",
    'firmware = "bios"',
    'bios.bootOrder = "cdrom"',
    'bios.bootDelay = "3000"',
    'msg.autoAnswer = "TRUE"',
    'floppy0.present = "FALSE"',
    'ethernet0.present = "FALSE"',
    'usb.present = "FALSE"',
    'sound.present = "FALSE"',
    'vmci0.present = "FALSE"',
    'tools.syncTime = "FALSE"',
    'tools.upgrade.policy = "manual"',
    '',
    '# Boot from the SutureOS ISO',
    'ide1:0.present = "TRUE"',
    'ide1:0.deviceType = "cdrom-image"',
    "ide1:0.fileName = `"$(Split-Path -Leaf $isoAbs)`"",
    'ide1:0.startConnected = "TRUE"',
    ''
) + $sataLines + @(
    '',
    '# Serial console to a file: this is how the boot log is captured',
    'serial0.present = "TRUE"',
    'serial0.fileType = "file"',
    "serial0.fileName = `"$(Split-Path -Leaf $serialAbs)`"",
    'serial0.tryNoRxLoss = "FALSE"',
    'serial0.yieldOnMsrRead = "TRUE"',
    'serial0.autodetect = "FALSE"'
)
Set-Content -Path $vmxPath -Value $vmx -Encoding ascii

Write-Host "[vmware] $vmxPath"
$start = & $vmrun -T ws start $vmxPath nogui 2>&1
$start | ForEach-Object { Write-Host "[vmrun] $_" }
if ($LASTEXITCODE -ne 0) {
    if (Test-Path (Join-Path $build 'vmware.log')) {
        Write-Host "[vmware.log tail]"
        Get-Content (Join-Path $build 'vmware.log') | Select-Object -Last 12 | ForEach-Object { "  $_" }
    }
    throw "vmrun start failed"
}

Start-Sleep -Seconds $WaitSeconds

if (-not $KeepRunning) {
    $null = & $vmrun -T ws stop $vmxPath hard 2>&1
    Start-Sleep -Seconds 2
}

$len = 0
if (Test-Path $serialAbs) { $len = (Get-Item $serialAbs).Length }
Write-Host "[vmware] serial log $SerialLog : $len bytes"
if ($len -eq 0) { Write-Warning "empty serial log - did the guest boot from the CD?" }
