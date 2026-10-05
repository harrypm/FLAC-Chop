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
