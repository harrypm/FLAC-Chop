# HARD DEV NOTE — native Windows file dialogs: verify WHICH native before "fixing"

Status: HARD RULE for every Qt6 GUI in the family. A report of an "old-looking / Vista / 7-style file dialog" is NOT actionable until the dialog's window classes have been enumerated: the `#32770` DIALOG class hosts BOTH the modern `IFileDialog` (Win7+/Win10/11 look — address bar, breadcrumb, search box, Quick access sidebar) AND the legacy `GetOpenFileName` dialog (Vista-era "Look in:" look). The top-level class alone proves NOTHING. Reported against FLAC-Chop on 2026-10-05 ("Win vista/7 elements in the lookup system"); enumeration proved the dialogs are the MODERN native ones — the report was against a pre-theme-port build, and the user confirmed the current build's dialog is good.

## How to verify (hard data, ~2 minutes, no guessing)

1. Make the dialog open reproducibly (a temporary env-gated auto-browse hook in the MainWindow ctor, or a minimal test app: one `QFileDialog::getOpenFileName` call).
2. While it is open, enumerate the process's visible top-level windows and the `#32770` dialog's visible child classes (EnumWindows + EnumChildWindows + GetWindowThreadProcessId + GetClassName; a ~30-line PowerShell/Add-Type).
3. Modern `IFileDialog` hallmarks (FLAC-Chop's dialog, enumerated 2026-10-05):
   `DirectUIHWND`, `DUIViewWndClassName`, `Breadcrumb Parent`, `Address Band Root`, `NamespaceTreeControl` (the sidebar), `Search Box`, `SearchEditBoxWrapperClass`, `UniversalSearchBand`, `TravelBand`, `WorkerW`.
   Legacy `GetOpenFileName` look: a `ComboBoxEx32` "Look in:" row WITHOUT `DirectUIHWND`.
4. Final proof: the user's eyes on the open dialog ("good" — 2026-10-05).

## Rules

- NEVER assume from the top-level `#32770` class — enumerate the children.
- Static `QFileDialog::getOpen*` calls use the native dialog by default on Windows (DontUseNativeDialog off, no AA_DontUseNativeDialogs, no Q_OBJECT subclass). Keep them static.
- A genuinely LEGACY/Qt-styled dialog means something broke the native path (COM apartment conflicts from background threads initializing COM differently, options disabling it) — find that cause with the enumeration first; do not "fix" the dialog code blindly.
- A long multi-pattern filter entry ("Name (*.a *.b … *.z)") is legal and does NOT force a fallback (verified: the same dialog opened with FLAC-Chop's full 19-extension filter and enumerated modern).

## 2026-10-11 case — "not the modern dialog" AGAIN: the cause was THEME, not classes

Report: "it's not using the modern Windows one" (dist-vfix test builds, after
the ld-compress-ng integration). The class enumeration said MODERN for every
dialog — single-open, folder picker (getExistingDirectory), and multi-select
(getOpenFileNames) all enumerated DirectUIHWND + DUIViewWndClassName +
Breadcrumb Parent + Address Band Root + NamespaceTreeControl + Search Box.
The differentiator this time was the dialog's THEME:

- Screenshot measurements (scripts/capture_filedialog_png.ps1 + region mean
  RGB): FLAC-Chop's dialog = BLACK title bar (RGB ~10) + LIGHT body
  (RGB 235 sidebar / 239 file list) on a DARK-mode system
  (AppsUseLightTheme=0). A reference dialog from a neutral host (pwsh
  WinForms OpenFileDialog — the same shell IFileDialog) = dark everywhere
  (54 sidebar / 37 list / 61 footer). Dark-title + light-body hybrid is
  exactly what reads as "the old dialog" next to a dark OS.
- Module check (`Get-Process flac-chop | % Modules`): the process loaded
  app-local UxTheme.dll, DWrite.dll, d3d9/d3d11/d3d12.dll, dxgi.dll,
  dwmapi.dll, WINMM.dll, USP10.dll, VERSION.dll, WINHTTP.dll, … — an ad-hoc
  ldd-walk bundle had copied 52 System32 DLLs beside the exe. Anything NOT
  in KnownDLLs loads from the app folder; an out-of-context app-local
  UxTheme.dll breaks the shell dialog's immersive dark-mode resolution →
  light body on a dark system. (comdlg32/comctl32/SHELL32/ole32 local copies
  were inert — those ARE KnownDLLs — which is why the class enumeration alone
  looked fine.)
- Fix: scripts/bundle_clean_system_dlls.ps1 — deletes bundle DLLs
  byte-identical to C:\Windows\System32 (system copies), keeps same-named
  toolchain copies (MSYS2 zlib1.dll differs; restore it from /mingw64/bin
  after cleaning). After cleanup: dialog dark everywhere (title 54, sidebar
  33, list 46, footer 40), ZERO system DLLs loaded from the bundle, CLI +
  probe + GPU conversion still work.

New rules from this case:

- NEVER bundle Windows system DLLs beside the exe. No ldd-walk copies of
  anything that exists in C:\Windows\System32 (vulkan-1.dll etc. come with
  the GPU driver).
- Modern classes are necessary but NOT sufficient: a dialog that enumerates
  modern but still "looks wrong" → compare its THEME against a neutral-host
  reference dialog, and check `Get-Process .Modules` for app-dir system DLLs.
- Reproducible dialog for any future test: `FLAC_CHOP_AUTO_BROWSE=open|dir|multi`
  (env-gated auto-open at startup, gui/mainwindow.cpp ctor) — then enumerate
  and/or capture while it sits open.
