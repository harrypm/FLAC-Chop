# Enumerate the native file-dialog window classes of a running process —
# the docs/DEV_NOTE_native_file_dialogs.md HARD-RULE procedure.
#
# Usage:
#   powershell -File scripts/enum_filedialog_classes.ps1 -ProcessName flac-chop
#
# Run this WHILE the file dialog in question is open. It lists every visible
# top-level window class of the process and, for each #32770 dialog, all child
# window classes. Modern IFileDialog hallmarks: DirectUIHWND,
# DUIViewWndClassName, Breadcrumb Parent, Address Band Root,
# NamespaceTreeControl, Search Box. Legacy GetOpenFileName: ComboBoxEx32
# ("Look in:") WITHOUT DirectUIHWND. A Qt fallback dialog shows NO #32770 at
# all — instead a Qt6xxQWindow* top-level class.
param(
    [Parameter(Mandatory = $true)] [string] $ProcessName
)

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class WinEnum {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);

    public static string Class(IntPtr h) {
        var sb = new StringBuilder(256);
        GetClassName(h, sb, 256);
        return sb.ToString();
    }

    public static string Title(IntPtr h) {
        var sb = new StringBuilder(512);
        GetWindowText(h, sb, 512);
        return sb.ToString();
    }

    static List<string> Lines = new List<string>();
    static HashSet<long> Seen = new HashSet<long>();

    static bool WalkChild(IntPtr h, IntPtr lParam) {
        if (!Seen.Add(h.ToInt64())) return true; // EnumChildWindows visits
        Lines.Add("    child  " + Class(h) + (IsWindowVisible(h) ? "" : " (hidden)"));
        return true;
    }

    static bool WalkTop(IntPtr h, IntPtr lParam) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid != (uint)lParam) return true;
        var cls = Class(h);
        Lines.Add("TOP  " + cls + "  hwnd=0x" + h.ToString("X")
                  + (IsWindowVisible(h) ? "  visible" : "  hidden")
                  + "  title=\"" + Title(h) + "\"");
        if (cls == "#32770") {
            Seen.Clear();
            EnumChildWindows(h, WalkChild, IntPtr.Zero);
        }
        return true;
    }

    public static string[] Run(uint pid) {
        Lines.Clear();
        EnumWindows(WalkTop, (IntPtr)pid);
        return Lines.ToArray();
    }
}
"@

$procs = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if (-not $procs) { Write-Error "No process named '$ProcessName' is running."; exit 1 }
foreach ($p in $procs) {
    Write-Output "=== $($p.ProcessName) pid=$($p.Id) ==="
    [WinEnum]::Run([uint32]$p.Id)
}
