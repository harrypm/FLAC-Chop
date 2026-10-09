# PROMPT: progress bar % + editable output path + metadata-on-output + "Process to FLAC" label (2026-10-08)

## Inputs (user prompts, in order)
1. "Well add the progress bar feature then alongside a % completion rate, also for .lds files being converted allow the metdata editor page to work and apply to saved/chopped output files, also the output path with name should be in the output box for easy rename across all platforms"
2. "Also 'Process FLAC' should change to 'Process to FLAC' if raw/packed files are input at current state."

## Changes made

### Rust core
- `core/src/chop.rs`:
  - `static PROGRESS: AtomicU32` (0-100 = running; 101 = idle) + `pub fn get_progress()`.
  - `parse_sox_overall_progress()`: parses SoX `-S` `overall:X.XX%` from a stderr chunk (SoX writes progress with `\r`, so `rfind("overall:")` then the number before `%`).
  - `run_sox_child()`: stderr now read by a dedicated reader thread — continuously parses `overall:X.XX%` into `PROGRESS`, accumulates the text into an mpsc channel (both the cancel path and the normal exit path get the accumulated stderr via `recv_timeout(5s)`); a 16 KB tail cap on the parse buffer.
  - `-S` added to BOTH sox Command constructions in `chop_native_input()` (main cmd + the 6-bit pass-2 cmd2), after `-D` (both global options).
  - `PROGRESS.store(0)` at the start of `chop_with_options_and_cancel()`; `PROGRESS.store(101)` (idle) at every end path: normal exit, cancel, spawn-failure, and the wrapper's early `fail` closure. (Idle reset kills a stale-100% flash if a second Process is clicked before the future thread starts; the GUI's poll ignores 101.)
  - Import fixes found by the build: `AtomicU32` added to the atomic import; module-level `use std::io::Read` removed (the reader thread has its own local import); `let mut stderr_handle` for `read()`.
- `core/src/ffi.rs` + `gui/flacchop.h`: `fc_chop_get_progress() -> u32` (0-100 = running; 101 = idle).
- NOTE (unchanged): the static-sox backend (single-binary builds) has no `-S` equivalent — PROGRESS stays 0 during the cut and jumps to 100% at the end (GUI-side). Shell-out builds (incl. dist-vfix with bundled sox.exe) get live %.

### GUI (`gui/mainwindow.h/.cpp`)
- Progress bar: `setRange(0,100)` + `setTextVisible(true)` + `setFormat("%p%")`; a 200 ms `QTimer` (`m_progressTimer`, member added to the header + `class QTimer;` forward decl) polls `fc_chop_get_progress()`; `process()` starts it (bar reset to 0%, text re-shown), `onChopFinished()` stops it; 100% left showing on success; 0% + text hidden on cancel/failure. Probe/metadata-save busy indicators now hide the % text (`setTextVisible(false)`) so a busy bar never shows bogus text.
- Output path: `QLabel* m_outPathLabel` → `QLineEdit* m_outPathEdit` in the Preview box. Empty = auto-derived path (shown as the PLACEHOLDER, from `fc_generate_output_path`); typed text overrides it live (`textChanged` → `applyCut`). Disabled while no file is loaded / probing / cutting; cleared + placeholder reset in `unloadFile()`/failed probe. `process()` also mkpaths the custom path's parent directory (the Output Directory field only covers the auto path).
- Button label: `setProbeInfo()` sets "Process FLAC" for FLAC (0) and Ogg FLAC (6) inputs — FLAC→FLAC cuts — and "Process to FLAC" for everything else (WAV, raw u8/s8/u16/s16, packed .lds); reset to "Process FLAC" on unload/failed probe.
- Metadata-on-output: after a successful cut from a NON-FLAC input (WAV/raw/packed .lds/Ogg .ldf), the output (always a native FLAC) is auto-loaded via `loadFile(m_outPath)` — the Metadata Editor then works on the output (its tags ARE editable; the raw/packed source's are not), and the Chop tab reflects the produced file. The custom output-path text is cleared first so the next cut can't overwrite the just-loaded output; `return` before `setControlsEnabled(true)` so the probe owns the controls.

## Verification (commands + hard results)
- Build (MSYS2 MINGW64, rustup gnu toolchain): `env.exe MSYSTEM=MINGW64 RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu bash -lc 'export PATH="/c/Users/Harry/.cargo/bin:$PATH" && cd /c/Users/Harry/flac-chop && cmake --build build-vfix'` — core compiled, `gui/flac-chop.exe` linked clean.
  - NOTE: the MSYS2 login shell drops cargo from PATH (HOME=/home/Harry); the CMake cargo step (cmd.exe child) needs `export PATH="/c/Users/Harry/.cargo/bin:$PATH"` after login init.
- `cargo test` (FLAC_CHOP_SOX="C:\Program Files\DecodeTools\sox.exe"): **148 lib + 6 ffi_plan + 5 lds_integration + 4 parallel_cancel + 11 roundtrip12 = 174 passed, 0 failed** (cut tests actually executed with sox present; MD5-unset warnings on cut12.flac are the known benign STREAMINFO ones).
- `dist-vfix\flac-chop.exe` refresh BLOCKED: the file is locked by the still-running OLD GUI (left open from the 2026-10-05 .lds session). Refresh after closing it: `Copy-Item build-vfix\gui\flac-chop.exe dist-vfix\flac-chop.exe -Force`.

## Still awaiting user confirmation (on their screen, from dist-vfix)
1. Load a FLAC → button reads "Process FLAC"; load a .lds/raw/WAV → button reads "Process to FLAC".
2. A cut shows live % on the progress bar (SoX `overall:`), 100% on completion; busy probe/save shows no % text.
3. The Output file box shows the auto path as placeholder; typing a custom path updates the plan live and the cut lands at the custom path (incl. a not-yet-existing directory).
4. After converting a .lds (or raw/WAV/.ldf), the output auto-loads and the Metadata Editor edits/tags THE OUTPUT file (Save writes in place).
