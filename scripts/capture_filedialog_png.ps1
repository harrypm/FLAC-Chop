# Screenshot the native file dialog (#32770) of a running process —
# companion to enum_filedialog_classes.ps1 (docs/DEV_NOTE_native_file_dialogs.md).
# Captures appearance (dark/light, view mode, chrome) as hard data.
#
# Usage:
#   powershell -File scripts/capture_filedialog_png.ps1 -ProcessName flac-chop -OutPng out.png
param(
    [Parameter(Mandatory = $true)] [string] $ProcessName,
    [Parameter(Mandatory = $true)] [string] $OutPng
)

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class WinCap {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static IntPtr Found = IntPtr.Zero;
    public static string FoundTitle = "";

    static bool Walk(IntPtr h, IntPtr l) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == (uint)l && IsWindowVisible(h)) {
            var sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (sb.ToString() == "#32770") {
                Found = h;
                var tb = new StringBuilder(256); GetWindowText(h, tb, 256);
                FoundTitle = tb.ToString();
                return false;
            }
        }
        return true;
    }

    public static IntPtr FindDialog(uint pid) {
        Found = IntPtr.Zero; FoundTitle = "";
        EnumWindows(Walk, (IntPtr)pid);
        return Found;
    }
}
"@

[WinCap]::SetProcessDPIAware() | Out-Null
$procs = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if (-not $procs) { Write-Error "No process named '$ProcessName' is running."; exit 1 }
$hwnd = [IntPtr]::Zero
$title = ''
foreach ($p in $procs) {
    $hwnd = [WinCap]::FindDialog([uint32]$p.Id)
    if ($hwnd -ne [IntPtr]::Zero) { $title = [WinCap]::FoundTitle; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "No visible #32770 dialog in '$ProcessName'."; exit 1 }
Write-Output "dialog: `"$title`" hwnd=0x$($hwnd.ToString('X'))"

[WinCap]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 600
$r = New-Object WinCap+RECT
[WinCap]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
Write-Output "rect: $($r.L),$($r.T) ${w}x${ht}"

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved: $OutPng"
