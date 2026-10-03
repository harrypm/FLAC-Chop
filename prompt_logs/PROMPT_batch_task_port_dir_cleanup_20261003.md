# PROMPT: batch+parallel processing port + out-dir cleanup + input-box contrast (2026-10-03)

## Inputs (user prompts, in order)
1. "git pull, then add the batch & parallell compresison processing features from tbc-tools lds-converter. also cleanup the input/output directory reset state"
2. "Execute this plan." (plan 3e3cce15 approved)
3. "I want to add a batch processing to a tab 'Batch Task'"
4. "right let me see the gui now you have updated the code"
5. "fix text contrast inside of file selection boxes (take notes from tbc-tools etc this has been fixed many times but should have a hard dev note)"

## Changes made
Core (Rust):
- core/src/chop.rs: per-job cancel (`Cancel::Global|Job(&AtomicI32)` threaded through run_sox_child / chop_native_input / Ogg remux / chop_12bit_source_pure); new `chop_with_options_and_cancel`; unique temp names (`temp_tag()` = pid-counter) for 6-bit/12-bit/oggsrc temps; static-sox backend serialized on a mutex.
- core/src/ffi.rs + gui/flacchop.h: `fc_chop_ex(..., const int32_t* cancel_flag)` (NULL = legacy global); fc_chop reimplemented on a shared fc_chop_run/fc_chop_impl.
- core/tests/parallel_cancel.rs (new): preset-flag cancel (sox + pure paths), pre-cancelled job + concurrent normal job, concurrent 6-bit jobs (temp collision regression), mid-flight cancel never fakes a complete cut. roundtrip12.rs CHOP_LOCK comment updated.
GUI (C++):
- gui/batchtab.{h,cpp} (new): Batch Task tab — queue table + Add/Remove/Clear, own out-dir (empty = per-input folder), mode/bits/filter combos, parallel (default ON) + overwrite (default OFF) checkboxes, Process/Stop, std::async sliding-window pump loop (lds-converter port), per-job std::atomic<int> flags -> fc_chop_ex, queued-connection transient status, pre-resolved per-job outputs (skip-existing, in-batch `_2` uniqueness, never writes onto a queued input — MISRC keep-source degenerate case sidesteps via fc_generate_output_path).
- gui/stemutil.h (new): shared renamedOutputStem(in, modeHz, bits).
- gui/mainwindow.{h,cpp}: hosts the Batch Task tab; multi-file drag&drop -> queue, single-file drop unchanged; out-dir state cleanup (m_outDirAutoFollow removed; follow == empty field; placeholder shows live effective dir; loadFile/unloadFile refresh placeholder; fixes the no-op editingFinished pin + stale-dir-on-failed-probe bugs).
- gui/CMakeLists.txt: batchtab/stemutil sources.
- gui/main.cpp: input-box contrast fix (PlaceholderText #D0D4D9, Disabled Text #AAAFB5, Disabled PlaceholderText #8D9399 + guard stylesheet) — see docs/DEV_NOTE_dark_theme_input_contrast.md.
- readme.md: Batch Task section + feature bullet.

## Bugs found by real-data testing (and fixed)
1. MISRC-named capture + keep-source settings: fresh output name == the input's own name -> wrongly "Skipped — output already exists"; with overwrite ON the worker would have deleted the INPUT. Fixed: input-protection + fc_generate_output_path sidestep (…-2.flac).
2. Queue stored relative paths (CLI-origin) while canonical outputs were absolute -> the protection comparison missed. Fixed: normalizeInputPath -> absolute.
3. Input-box contrast (user-reported on the live GUI): PlaceholderText resolved #000000 on Base #191919 (invisible). Fixed per tbc-tools uistyle.h; measured A/B (see below).

## Verification (commands + hard results)
- cargo test --release (core): 133 + 6 ffi_plan + 4 parallel_cancel + 11 roundtrip12 all pass.
- cmake build (Ninja): clean; offscreen 10s launch: constructs (exit 124 = alive).
- Headless harness (/tmp/fc_batch_harness/fc_batch_harness, drives the REAL BatchTab offscreen):
  - run (3 real captures, parallel): all Done in 8.2s wall / 14.3s CPU; ffprobe+MediaInfo: outputs match inputs (codec/rate/depth/duration identical; 19.353792s / 16.150271s / 19351.0356s).
  - skip (re-run): all "Skipped — output already exists" in 0.05s.
  - overwrite6 (smol): replaced output, ffprobe 8-bit container (6-bit grid), duration preserved.
  - stop (4 real files, sequential): Done/Done/Cancelled (mid-run)/Cancelled (not started); partial outputs removed; no stray temps.
  - India keep-source (MISRC name): output = …_8-bit_20msps-2.flac (never the input); ffprobe duration identical (10407.0795s).
  - India overwrite6: output = …_6-bit_20msps.flac (renamed stem), 8-bit container, duration identical.
- contrast_check (A/B palette measurement): OLD PlaceholderText #000000 dist 0.10 FAIL / Disabled #000000 FAIL; NEW #d0d4d9 dist 0.74 OK, Disabled/Text #aaafb5 0.59 OK, Disabled/Placeholder #8d9399 0.48 OK (tbc-tools thresholds 0.45/0.2).

## Still awaiting user confirmation (GUI on their screen)
- Batch Task tab usability, drop routing, Stop behavior, per-row statuses.
- Out-dir placeholder readability + the new follow semantics on the Chop tab.
- Input-box contrast after the fix (relaunched build).
- No commit made yet; restore point to be written only after user confirms fixed.
