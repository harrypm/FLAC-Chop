# Restore point — ld-compress-ng GPU integration + bit-perfect verify + checksums + batch/sync surfacing + dialog fix (2026-10-11)

Checkpoint of everything since `4a21172` (the 2026-10-09 streaming/.lds/
Output-File session): the ld-compress-ng GPU acceleration integration,
bit-perfect verification with SHA-256 checksums, the batch/sync verify
surfacing, the About dialog, and the file-dialog dark-mode fix.

Archive: `C:\Users\Harry\fc-restore-points\flac-chop_2026-10-11_ldc-gpu-verify.zip`
(git archive of this commit — master). To go back: unzip over a checkout, or
reset to the commit that added this note:
`git log -1 --format=%H -- restore_points/2026-10-11_ldc_gpu_verify_batchsurfaced.md`.

## What shipped in this state

1. **GPU-accelerated whole-file conversions** (`core/src/chop.rs`): `.lds` and
   8-bit raw (u8/s8) PLAIN whole-file conversions run through the
   ld-compress-ng CLI (`ldcdet` beside-exe/PATH/`FLAC_CHOP_LDC`, the sox
   model) — `compress --overwrite --progress`, the `compressing: NN%` stderr
   line feeds the existing progress atomic, cancel kills the child. Raw 8-bit
   packs onto the `.lds` 10-bit grid via `pack_raw8_to_lds` ((b-128)*256).
   Guards: whole-file, PLAIN, mono, group-aligned, exactly 40 MSPS. Global
   `ChopOptions::lds_backend` (Auto/Ldc/Sox) + `fc_set_lds_backend()`,
   `fc_ldc_available()`; Auto falls back to SoX with a note, a required-GPU
   preference that cannot engage gets a clear error.
2. **Bit-perfect verification** (`core/src/verify.rs`, on by default,
   `fc_set_verify_output()`): after a PLAIN non-FLAC conversion the output is
   decoded and compared sample-for-sample against the source read natively
   (`.lds` unpack / WAV data chunk / raw with SoX width+sign semantics,
   depth-aware for the GPU ×256 path). PASS → `verify: PASS (N samples,
   bit-perfect) — safe to delete the source`; FAIL → loud warning;
   processed conversions → explicit skip; FLAC/Ogg sources → no note.
3. **SHA-256 checksums** (`core/src/sha256.rs`, self-contained FIPS 180-4):
   `sha256(output): <hex>` appended to the notes LAST, and
   `SOURCE_SHA256=<hex>` embedded as a Vorbis tag on non-FLAC conversions
   (survives source deletion).
4. **Global Encoder combo + verify checkbox** (Output Processing box,
   `gui/mainwindow.cpp`): Auto (GPU/CPU) / GPU / SoX (Metal wording on macOS),
   generic tooltip, persisted `engine/choice` (with `lds/engine` fallback);
   CLI `--encoder auto|gpu|sox` (+ the old `--lds-backend`) and
   `--verify`/`--no-verify` (`gui/main.cpp`).
5. **Batch + Sync Edit verify surfacing** (`gui/batchtab.*`,
   `gui/syncedittab.*`): per-tab "Verify output (bit-perfect, before deleting
   sources)" checkboxes (default on, persisted `batch/verify`/`sync/verify`),
   `fc_set_verify_output()` before the jobs launch, workers extract the
   `verify:` line from stderr notes, Done rows show the verdict underneath,
   FAILED verdicts get the warning colour, summaries append
   "Verified: N passed, M flagged.".
6. **About dialog** (Help → About FLAC-Chop, the tbc-tools ld-analyse layout:
   icon left, linked text right, version bottom-left, OK bottom-right; the
   About tab page was removed). Status-label height caps
   (`fontMetrics().height() * 5 + 8`).
7. **File-dialog dark-mode fix**: the dist-vfix bundle had 52 System32 DLLs
   ldd-walk-copied beside the exe; the app-local `UxTheme.dll` (not a
   KnownDLL) broke the native file dialog's immersive dark mode → dark title
   bar + light body on a dark-mode system (what read as "the old dialog").
   Fixed by `scripts/bundle_clean_system_dlls.ps1` (never bundle system
   DLLs); documented in `docs/DEV_NOTE_native_file_dialogs.md` (the
   2026-10-11 case + rules). Dev tooling added: `FLAC_CHOP_AUTO_BROWSE=
   open|dir|multi` reproducible-dialog hook (MainWindow ctor) +
   `scripts/enum_filedialog_classes.ps1` + `scripts/capture_filedialog_png.ps1`.

## Verification summary (hard data)

- cargo test (sox + ldc forced locally): **190 passed, 0 failed** — 158 lib +
  6 ffi_plan + 11 lds_integration (engine-pinned per test) + 4 parallel_cancel
  + 11 roundtrip12.
- Real data (251 MB `.lds`, whole-file, GPU path): **15.97 s vs 63.97 s SoX**
  (4.0×), output 4.3% smaller (70,196,278 vs 73,357,763 bytes), decoded s16
  **SHA256-identical** to the reference ld-lds-converter unpack
  (`0b513122…55ea5d`), `verify: PASS`, tags + checksums correct
  (ffprobe-checked).
- 4 MB raw u8 whole-file GPU conversion: 0.59 s, `verify: PASS`.
- Dialog fix A/B (screenshot region mean RGB, dark-mode system): before —
  title 10 / sidebar 235 / list 239 (hybrid); after — title 54 / sidebar 33 /
  list 46 / footer 40 (dark everywhere, matching a neutral-host reference
  54/37/61), **0 system DLLs loaded from the bundle**; `--version` +
  `--probe` (.lds, 40 MSPS 10-bit) pass from the cleaned bundle.
- Test builds stamped `dev-20261010-enc2` / `dev-20261010-vfy` in the title
  bar.
- Session log: `prompt_logs/PROMPT_ldcompress_acceleration_integration_20261009.md`
  (all follow-ups through 2026-10-11).

Still awaiting the user's on-screen confirmation: the Batch/Sync verify
rows + the fixed dark file dialog (relaunched 2026-10-11 as
`dev-20261010-vfy`).
