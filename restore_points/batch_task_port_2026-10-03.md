# Restore point — Batch Task port + out-dir state cleanup + input-box contrast fix (2026-10-03, user-confirmed)

User confirmation: "looks great contrast fix is good" (GUI inspected on the live session, 2026-10-03 13:24).
Scope: the full batch-task prompt (see prompt_logs/PROMPT_batch_task_port_dir_cleanup_20261003.md for every command and result).

## What changed
Core (Rust):
- core/src/chop.rs — per-job cancel (Cancel::Global/Job(&AtomicI32)) threaded through all backends; chop_with_options_and_cancel; unique temp names (temp_tag()); static-sox serialized on a mutex.
- core/src/ffi.rs — fc_chop_ex(+cancel_flag); fc_chop refactored onto shared fc_chop_run/fc_chop_impl.
- core/tests/parallel_cancel.rs (new) + roundtrip12.rs comment update.
- gui/flacchop.h — fc_chop_ex declaration (ABI-append-only).
GUI (C++):
- gui/batchtab.{h,cpp} (new) — the Batch Task tab (queue, parallel std::async pump, per-job cancel flags, skip/overwrite, input-write protection).
- gui/stemutil.h (new) — shared renamedOutputStem.
- gui/mainwindow.{h,cpp} — Batch Task tab host, multi-drop routing, output-dir state cleanup (follow == empty field, placeholder-only updates).
- gui/main.cpp — input-box contrast fix (PlaceholderText + Disabled roles + guard stylesheet; see docs/DEV_NOTE_dark_theme_input_contrast.md).
- gui/CMakeLists.txt, readme.md.
Docs: docs/DEV_NOTE_dark_theme_input_contrast.md, prompt_logs/PROMPT_batch_task_port_dir_cleanup_20261003.md, this note.

## Verified (hard data)
- cargo test --release: 133 + 6 + 4 + 11 all pass.
- Headless BatchTab harness on real captures: parallel run (ffprobe/MediaInfo-identical outputs), skip, overwrite6, stop (Done/Cancelled mid-run/Cancelled not-started, partials removed), MISRC-named input protection (…-2.flac sidestep), renamed-stem 6-bit conversion.
- Contrast A/B: PlaceholderText #000000→#d0d4d9 (dist 0.10→0.74).
- User confirmed the GUI on their screen.

## How to restore
The changed/new files above are archived (paths preserved) in restore_points/batch_task_port_2026-10-03.zip. To go back: unzip over the repo root (git checkout of the commit also works; this zip is the pre-commit state).
