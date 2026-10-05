# PROMPT: release version shows "dev" + Windows portable/installer fix (2026-10-05)

## Inputs (user prompts, in order)
1. "git pull" (brought master 0ccb19f → e744de2, tag v1.0.8, plus the Batch Task port work)
2. "I want to fix the version naming (shows dev on a release this needs to be nuked and fixed) add CI tests for this to packaging and fix the Windows portable and Windows installer, run autonomusly and fix these 2 issues"
3. "Code not upto date" (checked: master == origin/master at e744de2; found remote v1.0.0 tag re-created + orphan v1.1.0 tag — see Notes)
4. "continue"
5. "install win get it should be installed" (winget found at %LOCALAPPDATA%\Microsoft\WindowsApps\winget.exe — not on the session PATH; used full path)

## Bugs found by hard data (before any fix)
### Bug 1 — release builds report "dev-<sha>" in-app
- v1.0.8 release artifact `windows_FLAC-Chop_v1.0.8_x86_64.exe` downloaded and unpacked with 7-Zip:
  - inner `flac-chop.exe --version` → `FLAC-Chop dev-e744de2` (also "v1.0.8" absent from the binary, "dev-e744de2" present at byte offset 1263436)
  - GUI window title → `FLAC-Chop dev-e744de2 — RF capture cutter`
- CI logs of the run that produced v1.0.8 (37127606245): every cmake configure printed `-- FLAC-Chop version: dev-e744de2` while the artifact NAME step resolved `v1.0.8` from GITHUB_REF.
- Root cause: `actions/checkout@v5` default (fetch-depth 1, no tags) → `git describe --tags` fails → CMakeLists falls back to `dev-<sha>`; the CI "Resolve build version" output was only used for artifact names, never passed into the build.
- Same defect on all platforms (AppImage, DMG, both Windows arches).

### Bug 2 — Windows artifacts were a generic 7-Zip SFX, not portable/installer
- Running the released `windows_FLAC-Chop_v1.0.8_x86_64.exe` shows a generic window titled "7-Zip self-extracting archive" (7z.sfx GUI module, no branding, no config): extracts a folder of 60+ files wherever pointed, installs nothing, runs nothing.
- Repo history shows the intended "Windows packaging rule": portable ZIP + Inno Setup installer per arch (commits cf1bac6, 67d81e0; CI-green run 33173203018 on 2026-08-28), which c30db15 reduced to the SFX detour.
- Extra: the WIN32-subsystem exe printed NOTHING for `--version`/CLI in real consoles (bare pwsh: no output; cmd: no output) — only pipe-captured runs showed output.

### Bug 3 (found while verifying locally) — stale-Rust-staticlib trap
- Local build linked a 26-Aug `libflac_chop_core.a` (missing `fc_chop_ex`/`fc_replace_comments` added 2026-10-03) because:
  - rustup default here is `stable-x86_64-pc-windows-msvc`: `cargo build --release` (spawned by CMake via cmd.exe) "succeeds" while writing `flac_chop_core.lib`; the MinGW CMake target keeps linking the stale `.a` — ninja never notices.
  - `RUST_SOURCES` in gui/CMakeLists.txt was missing the new core sources (enc12/ogg/streaminfo/tags) → cargo would not even re-run when they change.
- Local build recipe that works (= CI's): MSYS2 MINGW64 shell + `export PATH="$(cygpath -u "$HOME/.cargo")/bin:$PATH"` + `RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu`; delete the stale `.a` once to force the gnu rebuild.

## Changes made
- `CMakeLists.txt`: `FLAC_CHOP_VERSION` is now overridable (`if(NOT DEFINED …)`); git describe stays as the LOCAL-build fallback only.
- `gui/main.cpp`: `attachParentConsoleForCli()` — CLI mode attaches the parent console and reopens stdout/stderr/stdin ONLY when the inherited handles are invalid (CI pipes are never clobbered); makes `--version`/`--probe`/chop output visible in real cmd/PowerShell consoles.
- `gui/CMakeLists.txt`: `RUST_SOURCES` refreshed with all core sources (was missing enc12.rs, ogg.rs, streaminfo.rs, tags.rs — stale-staticlib bug).
- `.github/workflows/build.yml` (all 4 build jobs: linux-appimage, windows-exe, windows-arm64, macos-app-build):
  - checkout `fetch-depth: 0` (tags available to the describe fallback)
  - cmake now gets `-DFLAC_CHOP_VERSION="${{ steps.version.outputs.version }}"` (the primary fix — a packaged release can never report dev-)
  - NEW CI packaging tests: built exe `--version` must equal the resolved version exactly (each job); Windows additionally: portable ZIP expanded + inner exe `--version` + layout (sox.exe, Qt6Widgets.dll, platforms/qwindows.dll), installer silent-installed to a temp dir + installed exe `--version` + silent uninstall (unins000.exe, exit 0); Linux: the AppImage itself run with `--version` (AppRun forwards CLI args headless; APPIMAGE_EXTRACT_AND_RUN=1 for the FUSE-less runners); macOS: staged `.app` binary `--version`.
  - Windows packaging (both arches): the 7z SFX is GONE — replaced by portable ZIP (`windows_FLAC-Chop_<ver>_<arch>.zip`) + Inno Setup installer (`windows_FLAC-Chop_<ver>_<arch>.exe`, same names as the pre-SFX workflow). ISCC discovery checks choco (Program Files (x86)), Program Files, and LocalAppData\Programs (winget/user install — verified locally) before falling back to `choco install innosetup`.
  - release job uploads: the two new zips added to the globs.
- `assets/installer/flac-chop.iss` (new, real file — not an inline heredoc): per-user installer (PrivilegesRequired=lowest, `{localappdata}\Programs\FLAC-Chop`), Start-menu + optional desktop shortcuts, launch-after-install, per-arch AppId (uninstall entries don't clobber), numeric `VersionInfoVersion` + string `AppVersion`, everything parameterized via `/D` defines.
- `readme.md`: Downloads section now documents exe=installer vs zip=portable + the version guarantee.

## Verification (commands + hard results)
Local full build (MSYS2 MINGW64 + gnu toolchain, fresh `build-vfix`, override `-DFLAC_CHOP_VERSION=vLOCALVERIFY`):
- configure: `-- FLAC-Chop version: vLOCALVERIFY` (and with NO -D: `v1.0.8-dirty` — local fallback intact)
- built exe `--version`: `FLAC-Chop vLOCALVERIFY` piped (exit 0), BARE pwsh (prints! exit 0 — was silent before), cmd (prints) — console-attach fix verified
- GUI title: `FLAC-Chop vLOCALVERIFY — RF capture cutter` (ran, observed, killed)
- dist bundle replicated exactly as CI does (windeployqt + sox + ldd fixpoint walk): exe runs self-contained with NO mingw PATH (CLI + GUI)
- Portable ZIP (exact CI steps): Compress-Archive → Expand-Archive → inner exe `--version` = `FLAC-Chop vLOCALVERIFY`; sox.exe/Qt6Widgets.dll/platforms\qwindows.dll present; sox runs (`SoX v14.4.2`); zip 64.7 MB / 95 files → PASS
- Inno Setup 6.7.3 installed via winget; ISCC compiled the .iss with CI-style defines (exit 0, 46 MB installer; PE metadata: FileVersion 0.0.0.1, ProductVersion vLOCALVERIFY, ProductName FLAC-Chop, FileDescription "FLAC-Chop Setup")
- Installer end-to-end (exact CI steps): silent install exit 0 → installed exe `--version` = `FLAC-Chop vLOCALVERIFY` → sox.exe + platforms\qwindows.dll present (97 files) → GUI from install dir title correct → silent uninstall exit 0 → install dir removed → PASS

Left on disk for hands-on confirmation:
- `%TEMP%\fc-installer-out\windows_FLAC-Chop_vLOCALVERIFY_x86_64.exe` — real double-click installer UX test
- `dist-vfix\` — portable-style folder (run `dist-vfix\flac-chop.exe`)
- `build-vfix\` — the local build tree

## Notes / follow-ups
- Remote tag hygiene (needs a human decision, NOT changed): remote `v1.0.0` was re-created to point at c30db15 (local still has the original e3cf68c; `git fetch --tags` rejects it as a clobber), and `v1.1.0` (2026-07-14) is reachable from master but chronologically older than v1.0.1–v1.0.8. Neither affects the fixed versioning (describe at HEAD=v1.0.8 tag wins).
- Next release (e.g. v1.0.9 tag push or dispatch) is the real CI proof of all of this: every job's new Verify steps must pass (built exe, portable ZIP, installer install+uninstall, AppImage, .app).
- No commit made yet (repo convention); restore point zip to be written after user confirms the fixes on their screen.
