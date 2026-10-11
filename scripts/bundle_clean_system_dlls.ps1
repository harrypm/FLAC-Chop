# Remove accidentally-bundled Windows system DLLs from a FLAC-Chop dist
# folder. Root cause of the 2026-10-11 "old-looking file dialog" regression:
# an ad-hoc ldd-walk bundle copied ~52 System32 DLLs (UxTheme.dll, DWrite.dll,
# d3d9/11/12.dll, dxgi.dll, dwmapi.dll, ...) beside the exe. DLLs that are NOT
# in KnownDLLs get loaded FROM THE BUNDLE (verified via Get-Process .Modules),
# and an app-local UxTheme.dll breaks the native file dialog's immersive
# dark-mode resolution: dark title bar + LIGHT body on a dark-mode system
# (see docs/DEV_NOTE_native_file_dialogs.md).
#
# Rule: a bundle DLL that is byte-identical to C:\Windows\System32\<same name>
# is a system copy -> delete. A same-named DLL with different bytes (e.g.
# MSYS2's zlib1.dll) is a legit toolchain DLL -> keep.
#
# Usage:
#   powershell -File scripts/bundle_clean_system_dlls.ps1 -BundleDir dist-vfix
param(
    [Parameter(Mandatory = $true)] [string] $BundleDir
)

$system32 = 'C:\Windows\System32'
if (-not (Test-Path $BundleDir)) { Write-Error "BundleDir '$BundleDir' not found."; exit 1 }

$removed = New-Object System.Collections.Generic.List[string]
$keptDifferent = New-Object System.Collections.Generic.List[string]

Get-ChildItem $BundleDir -Recurse -File -Filter '*.dll' | ForEach-Object {
    $sys = Join-Path $system32 $_.Name
    if (-not (Test-Path -LiteralPath $sys)) { return }
    $bundleHash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    $sysHash = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash
    if ($bundleHash -eq $sysHash) {
        $removed.Add($_.FullName)
        Remove-Item -LiteralPath $_.FullName -Force
    } else {
        $keptDifferent.Add($_.FullName)
    }
}

Write-Output "removed $($removed.Count) system DLL copies (byte-identical to System32):"
$removed | ForEach-Object { Write-Output "  DEL $_" }
Write-Output "kept same-named but different-content DLLs (toolchain copies):"
$keptDifferent | ForEach-Object { Write-Output "  KEEP $_" }
if ($keptDifferent.Count -gt 0) {
    Write-Output "NOTE: verify the kept same-named DLLs are the toolchain versions you intend"
    Write-Output "      (e.g. MSYS2 zlib1.dll), not stale system copies from another PC."
}
