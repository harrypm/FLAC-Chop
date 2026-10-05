# Restore point — v1.0.9 release state (2026-10-05)

User-confirmed working ("good", 2026-10-05 15:58): one-click Lite/Dark Theme menu
(dark default), packed 10-bit `.lds` input, release version naming, Windows
portable ZIP + Inno installer packaging, native file dialogs.

Archive: `C:\Users\Harry\fc-restore-points\flac-chop_v1.0.9_2026-10-05.zip`
(git archive of the v1.0.9 tag = master c3ce05c). To go back: unzip over a
checkout, or `git reset --hard v1.0.9`.

## What shipped in this state (since v1.0.8)

1. **Release version fix** (v1.0.8 shipped `dev-e744de2` in-app): CI passes
   `-DFLAC_CHOP_VERSION` into every build + `fetch-depth: 0`; every packaging
   job VERIFY-steps the exe's `--version` (Windows: ZIP expand + Inno silent
   install → installed exe `--version` → silent uninstall; Linux: the AppImage
   itself `--version`; macOS: the staged .app). See
   `docs/DEV_NOTE_release_version.md`.
2. **Windows packaging**: portable ZIP (`windows_FLAC-Chop_<v>_<arch>.zip`) +
   Inno Setup installer (`...exe`, per-user, shortcuts, uninstaller) replace
   the generic 7z SFX. `assets/installer/flac-chop.iss`.
3. **Packed 10-bit `.lds` input** (ld-lds-converter integration): exact probe
   from the file size, 40 MSPS format rate used silently (an `<n>msps` hint
   overrides), sample-exact window unpack (byte-verified against the reference
   ld-lds-converter on real captures, whole-file SHA256-identical);
   `core/src/lds.rs`, `core/tests/lds_integration.rs`.
4. **Lite/Dark theme** (tbc-tools `uistyle.h` port): Theme menu (File / Theme /
   Help — Dark / Light), one-click switch (incl. the Windows repaint fix —
   pixel-verified), Dark default, remembered choice, input-contrast guard.
   `gui/theme.{h,cpp}`; see `docs/DEV_NOTE_theme_switch_one_click.md`.
5. **CLI console visibility** on Windows (WIN32-subsystem std-handle
   re-attach); `Open Data...` rename; README + prompt logs + dev notes.

## Verification summary (hard data)

- cargo test --release: 172 passed (incl. 16 new `.lds` tests, run with sox
  forced locally; CI's cargo-tests job runs them sox-less per the skip design)
- Real-data A/B vs ld-lds-converter: whole-file cut SHA256-identical decoded
  s16 (`0B513122…55EA5D` both), non-aligned partial cuts byte-exact
- One-click theme: pixel-measured dark (66,66,66) / light (243,242,242) per
  single synthetic menu trigger
- File dialogs: child-class enumeration = modern native IFileDialog; user
  confirmed good

## CI gate for this release

Tag push `v1.0.9` runs build.yml: cargo-tests → Linux AppImages (x2) →
Windows portable ZIP + Inno installer (x2, with the install/uninstall tests) →
macOS universal DMG → the release job publishes the assets. The Verify steps
fail the build if any packaged exe would report a `dev-` version.
