# SutureOS: host-side UI fixture for VMware Workstation.
#
# Boots a VMware guest, focuses the console window, injects keystrokes and
# captures the desktop, so "type a command -> the system reacts" can be
# verified on the hypervisor the user actually runs (VMware has no vmrun
# key injection; SendInput with KEYEVENTF_UNICODE is the way in).
#
# Usage:
#   powershell -NoProfile -File scripts\vmware_ui_probe.ps1 -Keys "startgui" -WaitSeconds 45
param(
    [string]$Vmx = "C:\Users\fanqi\Desktop\新建文件夹 (2)\Oracle Linux 5 和更早版本 64 位.vmx",
    [string]$Keys = "startgui",
    [int]$BootWaitSeconds = 45,
    [int]$AfterKeysSeconds = 6,
    [string]$Shot = "build\vmware_ui.png",
    [switch]$NoReboot
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class UiProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public KEYBDINPUT ki; }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, IntPtr dwExtraInfo);
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(120);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, IntPtr.Zero);
    }
    public const uint KEYEVENTF_UNICODE = 0x0004, KEYEVENTF_KEYUP = 0x0002, KEYEVENTF_EXTENDEDKEY = 0x0001;
    public static void SendChar(char c) {
        INPUT[] inp = new INPUT[2];
        inp[0].type = 1; inp[0].ki.wScan = (ushort)c; inp[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inp[1].type = 1; inp[1].ki.wScan = (ushort)c; inp[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, inp, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void SendEnter() {
        INPUT[] inp = new INPUT[2];
        inp[0].type = 1; inp[0].ki.wVk = 0x0D; inp[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        inp[1].type = 1; inp[1].ki.wVk = 0x0D; inp[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInput(2, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

function Get-VmwareWindow {
    $p = Get-Process vmware -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*VMware Workstation*' } | Select-Object -First 1
    if ($p) { return $p.MainWindowHandle }
    return [IntPtr]::Zero
}

if (-not $NoReboot) {
    Write-Host "[probe] restarting the guest"
    $null = & $vmrun -T ws stop $Vmx hard 2>&1
    Start-Sleep -Seconds 4
    Start-Process -FilePath $vmrun -ArgumentList @('-T', 'ws', 'start', "`"$Vmx`"") -WindowStyle Hidden
    Start-Sleep -Seconds $BootWaitSeconds
}

$hwnd = Get-VmwareWindow
if ($hwnd -eq [IntPtr]::Zero) { Write-Warning "[probe] no VMware window found"; exit 2 }
[void][UiProbe]::ShowWindow($hwnd, 9)   # SW_RESTORE
[void][UiProbe]::SetForegroundWindow($hwnd)
Start-Sleep -Seconds 2

$fg = [UiProbe]::GetForegroundWindow()
$title = New-Object System.Text.StringBuilder 256
[void][UiProbe]::GetWindowTextW($fg, $title, 256)
Write-Host "[probe] foreground window: '$($title.ToString())' (match=$($fg -eq $hwnd))"

# Click inside the guest console area so the guest captures the keyboard.
# Without this the keystrokes go to the Workstation chrome and never reach the
# guest (the "zero key events in the guest" symptom).
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinRect {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
}
"@
$r = New-Object WinRect+RECT
[void][WinRect]::GetWindowRect($hwnd, [ref]$r)
$cx = [int]($r.Left + 400); $cy = [int]($r.Top + 300)
Write-Host "[probe] clicking guest console at ($cx,$cy) window=($($r.Left),$($r.Top))-($($r.Right),$($r.Bottom))"
[UiProbe]::Click($cx, $cy)
Start-Sleep -Milliseconds 800
$fg = [UiProbe]::GetForegroundWindow()
[void][UiProbe]::GetWindowTextW($fg, $title, 256)
Write-Host "[probe] foreground after click: '$($title.ToString())'"

if ($Keys -ne "") {
    Write-Host "[probe] typing: '$Keys'"
    foreach ($ch in $Keys.ToCharArray()) {
        [UiProbe]::SendChar($ch)
        Start-Sleep -Milliseconds 90
    }
    Start-Sleep -Milliseconds 400
    [UiProbe]::SendEnter()
}
Start-Sleep -Seconds $AfterKeysSeconds

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$shotAbs = Join-Path $root $Shot
$bmp.Save($shotAbs, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "[probe] screenshot: $Shot ($((Get-Item $shotAbs).Length) bytes)"
