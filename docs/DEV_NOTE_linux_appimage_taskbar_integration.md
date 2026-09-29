# Dev note: Linux AppImage taskbar icon / pinning for Qt6 apps

Applies to every Qt6 GUI shipped as an AppImage (vhs-decode, tape-decode-rust GUI, FLAC-Chop, and future apps).
Written 2026-09-29 after porting the vhs-decode-dev fix to FLAC-Chop and tape-decode-rust.

Reference implementations
- vhs-decode-dev: `vhsdecode/qt_identity.py`, `resources/appimage/decode/AppRun`, `vhs-decode.desktop`, `restore_points/linux_taskbar_icon_fix_2026-09-29.md` (commit bf06c12e, user-confirmed on Linux Mint).
- FLAC-Chop (C++/Qt6): `gui/main.cpp`, `assets/appimage/AppRun`, `assets/appimage/flac-chop.desktop`.
- tape-decode-rust (PyQt6 + PyInstaller onefile): `decode_launcher.py` (`_apply_linux_app_identity`), `resources/appimage/AppRun`, `resources/appimage/decode-rust-gui.desktop`.

## Symptom
- Pinned launcher works, but the running window shows a placeholder icon in the taskbar, and/or pinning does not stick.
- Cinnamon creates a stray `~/.local/share/applications/<Name>.cinnamon-generated.desktop` with `Exec=/tmp/.mount_XXXX/...` (or a build dir). Seeing one of those = the app has no installed launcher matching its window.

## Root cause (two independent pieces, both are needed)
1. An AppImage is never registered with the desktop. Nothing installs a `.desktop` file or icon unless the AppImage does it itself (no appimaged / AppImageLauncher assumed).
2. The desktop matches a running window to a launcher via X11 `WM_CLASS` (class/instance) or Wayland `app_id` (desktop file name). Qt derives these from `argv[0]`, which inside an AppImage can be a temp mount path or an interpreter script (`decode.py`).
   Qt xcb: instance = `RESOURCE_NAME` env or basename(argv[0]); class = `QCoreApplication::applicationName()` or, if empty, derived from argv[0].
   Wayland: `app_id` = `QGuiApplication::desktopFileName()` or the executable name.

Which piece is broken depends on the app. Check first (do not assume):
- FLAC-Chop and tape-decode-rust already had a correct WM_CLASS -> only piece 1 was missing.
- vhs-decode ran through `decode.py` -> WM_CLASS was `decode.py` -> piece 2 was broken too.

## Fix recipe for a new app `<app>` (desktop id = `<app>`)
1. `.desktop` file shipped in the AppDir:
   - `Icon=<app>`, `Exec=<app>` (AppDir-internal), `StartupWMClass=<WM_CLASS class the app really reports>`.
   - Keep it as a real repo file and `cp` it in the workflow. Do NOT write it as an inline heredoc in the workflow YAML (nested `EOF` / `$` broke YAML in vhs-decode; that is why AppRun is a separate file).
2. Qt identity in code, right after the QApplication exists and before any window is shown (Qt reads it at native-window creation):
   - `QCoreApplication::setApplicationName(...)` (or keep the existing name and make `StartupWMClass` equal to it),
   - `QGuiApplication::setDesktopFileName("<app>")` (Wayland app_id),
   - `RESOURCE_NAME=<app>` env for the X11 instance name (before QApplication in C++; `os.environ.setdefault` in Python).
   - Linux only. Never let it raise (wrap in try/except in Python).
   - Do not rename `applicationName` casually: it also moves QSettings / QStandardPaths locations.
3. AppRun installs the user-level launcher + icon (see the three AppRuns for working code):
   - Only when `APPIMAGE` (the file path) is set and the file exists. Never write `$APPDIR`/mount path into `Exec`.
   - `Exec="<APPIMAGE path>" [%f]` (quote it; escape `%` as `%%`; skip integration if the path has `"` `` ` `` `$` `\`).
   - Icon to `${XDG_DATA_HOME:-~/.local/share}/icons/hicolor/<size>x<size>/apps/<app>.png`.
   - Rewrite when the generated content differs (follows whichever AppImage/version ran last, self-heals if deleted). Only then run `update-desktop-database` and `gtk-update-icon-cache -f -t` (best effort, `|| true`).
   - Only for GUI launches. Do not touch the desktop for CLI passthrough, `--selftest` (CI), `--probe`, `--version`. Mirror the app's own GUI-vs-CLI decision (`detectLaunchMode` in FLAC-Chop, `decode.py` in tape-decode-rust).
   - Provide an opt-out env var (`<APP>_NO_INTEGRATION=1`). Never `set -e`; integration must never block launching the app.
   - AppRun must stay executable (`chmod +x` in the workflow as well).
4. Icon: PNG with alpha (RGBA), square, 256 or 512. vhs-decode needed a grayscale -> RGBA conversion before the taskbar rendered it. Avoid 1024x1024 (appimagetool rejects it).
5. linuxdeploy-based builds (FLAC-Chop): a custom `AppDir/AppRun` copied in before `linuxdeploy` is kept as-is (verified on the shipped v1.0.4 AppImage). appimagetool-based builds (tape-decode-rust) simply use `AppDir/AppRun`.

## How to verify (hard data, not "looks fine")
- Real window class: launch the app, then `xdotool search --pid <pid>` -> `xprop -id <win> WM_CLASS _NET_WM_PID`. Match by PID: `xdotool search --name` can hit a different already-running instance (this happened while testing).
- Compare old vs new with a deliberately wrong argv[0] (`exec -a /tmp/.mount_x/whatever ...`) to prove the identity code is what pins the class.
- AppRun logic: run it in a sandbox (`HOME=/tmp/x APPIMAGE=/tmp/x/app.AppImage`) with a stub binary; check install, no-rewrite on rerun (mtime), CLI/selftest/opt-out create zero files, path with space and `%`, re-point to another AppImage.
- `desktop-file-validate` on the shipped and generated `.desktop` files.
- `ls ~/.local/share/applications | grep cinnamon-generated` before/after: no new stray entry.
- Final proof is the user's taskbar on real Linux Mint (Cinnamon): correct icon, pinning sticks. Ask the user; do not declare it fixed before that.

## Gotchas
- Old stray `*.cinnamon-generated.desktop` files keep shadowing; delete them and unpin/re-pin while the app is running.
- The desktop file id (`<app>.desktop`), `Icon=`, `setDesktopFileName`, `StartupWMClass` (or its case-insensitive match to the reported class) and the icon file name must all agree.
- Static/self-contained Qt builds and PyInstaller onefile change argv[0]/applicationFilePath: always measure WM_CLASS on the packaged build, not only from source.
- The same `~/.local/share/applications/<app>.desktop` is shared by every copy of the AppImage; the last-run copy wins (by design).
- Windows uses AppUserModelID and macOS the bundle; this recipe is Linux-only (`Q_OS_LINUX` / `sys.platform.startswith("linux")`).
