# PROMPT: ld-compress-ng acceleration integration (2026-10-09)

## Input (user prompt)
1. "Now I want to intergrate all the accelaration features from https://github.com/JoeyLemur/ld-compress-ng clone it locally and start intergration"

## Research findings
- `ld-compress-ng` = a focused C++20/CMake CLI (LGPL-2.1-or-later) for LaserDisc RF capture compression: `.lds` → native FLAC (`.ldf`) via GPU-accelerated frame-analysis backends (Metal / Vulkan / OpenCL; a frame-threaded CPU writer encodes the selected decisions), plus a CPU/libFLAC (Ogg) fallback, decompression, verify, convert, bench, and a `devices` enumeration.
- Acceleration = GPU compute for the FLAC analysis (fixed/Rice + LPC autocorrelation + candidate costing, FLACCL-style task ABI) + frame-level threading on the writer. Benchmarks upstream: ~3× CPU (0.81s Vulkan vs 2.44s CPU/libFLAC for a 150 MB fixture).
- The `ldcompress_core` library builds clean on MSYS2/MinGW64 as-is; only the CLI layer is POSIX-bound (signal-blocking staging cleanup via `sigprocmask`/`sigaction`, and a `::link` hard-link publication fallback) plus a fork-based `tests/test_cli.cpp`.

## Changes made (in the ld-compress-ng clone, Windows port)
- `src/main.cpp`: Windows port of the CLI layer —
  - `BlockTerminationSignals`: no-op on Windows (no sigprocmask; documented residual staging race).
  - `unregister_signal_safe_staging_output`: plain flag clear on Windows.
  - `cleanup_staging_output_on_termination`/`install_staging_output_termination_cleanup`: ANSI C `signal()` + `raise()` (SIGINT/SIGTERM/SIGBREAK) instead of `sigaction`/`kill`.
  - `rename_no_replace`: `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` — Windows' NATIVE atomic no-replace rename (ERROR_ALREADY_EXISTS → EEXIST), an upgrade over the `::link` fallback, which is now gated POSIX-only.
- `CMakeLists.txt`: `test_cli` (fork/wait supervision) gated to non-Windows builds.
- Per that project's AGENTS.md: read docs/PROJECT_PLAN.md first, minimal respectful port, coherent checkpoints after validation.

## Toolchain (installed via pacman, MSYS2 MinGW64)
- `mingw-w64-x86_64-glslang` (SPIR-V build-time shader compile), `mingw-w64-x86_64-opencl-headers` + `opencl-icd`. Already present: flac 1.5.0, libogg, vulkan headers+loader, cmake 4.3.3, gcc 16.1.
- GPU: NVIDIA GeForce GTX 1080 Ti, driver 582.66 (Vulkan 1.4.312, OpenCL 3.0 CUDA).

## Verification (commands + hard results)
- Build: `ld-compress-ng.exe` + all test exes link clean on MinGW (only `test_cli` gated off).
- `devices`: OpenCL **built + GTX 1080 Ti** (28 CUs, OpenCL 3.0 CUDA); Vulkan **built + GTX 1080 Ti, shaderInt64: yes, "vulkan backend usable: yes"** (16 compute queues, API 1.4.312); Metal not built (not Apple).
- **Real-data accelerated compress** (RF-Sample_2022-12-11_00-00-34.lds, 251,658,240 B = 201,326,592 samples @ 40 MSPS):
  - `compress --backend vulkan --overwrite ... ldcng-vulkan.flac` → **70,196,278 bytes (27.89%) in 15.97 s wall** (`compressing: 100% … 15 MiB/s`; `user` ≈ 0.015 s — the GPU does the work).
  - FLAC-Chop's current SoX whole-file conversion of the SAME input: **73,357,763 bytes in 63.97 s** — the accelerated path is **~4.0× faster** AND **4.3% smaller**.
  - `verify --source`: **decoded MD5 == source MD5** (`0da1ef4f…` both, 251,658,240 B round-trip).
  - SoX decode of the accelerated output → s16: **SHA256-identical to the reference ld-lds-converter unpack** (`0b513122…55ea5d`, 402,653,184 B) — the GPU-encoded FLAC decodes to the exact same sample stream.

## Integration implementation (completed 2026-10-10, build dev-20261010-ldc)

FLAC-Chop side, all verified:
- **Core (chop.rs)**: `ldcdet` (beside-exe → PATH, `FLAC_CHOP_LDC` override, `--version` probe) + `ldc_available()` (static-sox stub false); `parse_ldc_progress` (the `compressing: NN%` stderr line); `run_sox_child` generalized with a per-child progress parser; `run_ldc_compress` (`compress --overwrite --progress IN OUT`, cancel-safe, staging-transactional); `ChopOptions::lds_backend` (Auto/Ldc/Sox, default Auto) — PLAIN whole-file `.lds` conversions only (start==0, len>=total, no rate/bits/filter); Auto falls back to SoX with a note on any failure; Ldc-required fails with a clear not-found error; the accelerated output flows through the same validate + tag-rewrite (+ pending metadata rows) as the SoX path.
- **Verification (verify.rs, new)**: bit-perfect conversion verification — decode the output FLAC (claxon) and compare every sample of the cut window against the source read natively (`.lds` group unpack, WAV data chunk at its fmt depth with SoX width/sign semantics, raw u8/s8/u16/s16); streaming, no temps, no allocation of the full stream. Wired into `chop_with_options_and_cancel` post-dispatch: PASS → `verify: PASS (N samples, bit-perfect) — safe to delete the source`; FAIL → a loud warning (the GUI pops it); processed conversions → an explicit skip note (keep the source); FLAC/Ogg sources → no note (lossless by construction). Global enable `VERIFY_OUTPUT` default ON (`set_verify_output` / `fc_set_verify_output`).
- **stderr fix**: `collapse_cr_lines` — terminal ``-overwrite semantics for the accumulated child stderr (SoX `-S` AND ld-compress-ng `--progress` repaint one line; the raw byte stream overflowed the 1024-byte FFI stderr buffer and pushed the verify verdict out of the notes). All notes now fit.
- **FFI (ffi.rs + flacchop.h)**: `fc_set_lds_backend(0/1/2)` (FFI-side static folded into every `fc_chop`/`fc_chop_ex`), `fc_ldc_available()`, `fc_set_verify_output(on)`.
- **GUI (mainwindow)**: the Output Processing box gains the LDS encoder combo (Auto / GPU / SoX, persisted `lds/engine`) + the verify checkbox (default checked, persisted `lds/verify`); `process()` applies both before the cut. Status/notes surface the engine summary + the verify verdict (warnings pop).
- **CLI (main.cpp)**: `--lds-backend auto|gpu|sox`, `--verify` / `--no-verify` (default on) + usage text.
- **Docs**: README feature bullets + `docs/DEV_NOTE_ldc_acceleration.md`.
- **Bundling**: `dist-vfix/` = `flac-chop.exe` (dev-20261010-ldc) + `ld-compress-ng.exe` + `libstdc++-6/libgcc_s_seh-1/libwinpthread-1/libFLAC/libogg.dll` (vulkan-1.dll comes with the GPU driver).
- **Tests**: 183 passing (154 lib + 6 ffi_plan + 8 lds_integration + 4 parallel_cancel + 11 roundtrip12) with BOTH sox and ld-compress-ng resolvable — the accelerated test runs the real GPU path (byte-exact decode, embedded tags, `verify: PASS`, and the clean not-found failure without the CLI); partial cuts stay on SoX under the Ldc preference; engine-pinned per test.

## Real-data verification (FLAC-Chop's own CLI, dist-vfix, 2026-10-10)

- Whole-file `.lds` conversion (Auto engine, verify on): **12.6 s wall** (vs **64.0 s** SoX — ~5×) → **70,200,528 bytes** (27.9%) vs SoX's 73,357,763; tags embedded (`RF_TOTAL_SAMPLES=201326592`, `RF_SAMPLE_RATE=40000000`, KHZ, DURATION, LENGTH); **no temps**; notes end with **`verify: PASS (201326592 samples, bit-perfect) — safe to delete the source`**; decode → s16 **SHA256-identical to the reference unpack** (`0b513122…55ea5d`).

## Global encoder (raw data) + About tab (2026-10-10 follow-up, build dev-20261010-enc2)

User: "lds encoder stuff shoulld be universal for RAW data also so lets make it global not just lds" + "also add an about tab next to help with my name and Joeys name for the ld-compress-ng code works that is intergrated" + "This is also in the metdata right?" (checksum embedding).

**Changes:**
- **Checksums (the prior prompt "add checksum info to the completed source")**: new `core/src/sha256.rs` (self-contained FIPS 180-4 SHA-256, 1 MiB streaming file reader, NIST vectors incl. the million-'a' — verified against TWO independent implementations, Windows CNG Get-FileHash + coreutils sha256sum: cdc76e5c…, NOT the commonly mis-typed cdc76e5e). After a successful cut: `sha256(output): <hex>` appended LAST (after every tag write so it matches the final bytes); **`SOURCE_SHA256`** (the source file's whole-byte SHA-256) embedded as a Vorbis tag in the output for non-FLAC conversions — the permanent provenance record surviving the source's deletion. A whole-file checksum cannot be embedded in its own file (writing the tag changes the bytes); FLAC's STREAMINFO md5 remains the format's own decoded-audio record. GUI: a short `sha256: <first16>…<last8>` line in the finish status; the full hex in the tooltip. Both new sources registered in `gui/CMakeLists.txt` RUST_SOURCES (sha256.rs + verify.rs).
- **Global encoder (raw data)**: the GPU path now covers whole-file **8-bit raw (u8/s8)** captures — `pack_raw8_to_lds` packs the raw stream exactly onto the .lds 10-bit grid ((b-128)*256 is a multiple of 256, hence of 64; ld-compress-ng's wasted-bits support compresses the 8 low zero bits) into a temp .lds beside the output (`raw_lds_temp_path`) and runs `run_ldc_compress` on it; guards: whole-file + PLAIN + mono + group-aligned size + **exactly 40 MSPS** (`compress` writes a fixed 40 kHz header — also fixes the latent mislabel for non-40-MSPS .lds hints; the rate guard applies only where the GPU path would engage, so partial cuts with the GPU preference still just use SoX). 16-bit raw (u16/s16 cannot fit the 10-bit grid), WAV, other rates, partial, and processed conversions always use SoX; a required-GPU preference that cannot engage gets a clear error (rate/depth/not-found). The GPU raw output is 16-bit native FLAC holding exactly (b-128)*256 — the bit-perfect verify now compares 8-bit sources at the OUTPUT's depth (depth-aware `conv_u8_x256`/`conv_s8_x256` readers; native depth for the SoX 8-bit path).
- **GUI rebrand**: the combo is "Encoder (.lds / raw)" (Auto / GPU / SoX; persisted `engine/choice`, reads the old `lds/engine` as fallback). **CLI**: `--encoder auto|gpu|sox` (`--lds-backend` still accepted).
- **About tab** (last tab): credits Harry (harrypm) + Joey (JoeyLemur)'s ld-compress-ng (link, LGPL-2.1+) + SoX + Qt6.
- **Tests**: `raw_u8_whole_file_cut_can_use_accelerated_ldc_backend` (decode == (b-128)*256, 16-bit, tags, verify PASS, sha notes), `raw_u8_gpu_rejected_for_non_40msps_rate` (Ldc → clear error; Auto → SoX 8-bit native, decode == b-128, verify PASS), `lds_gpu_rejected_for_non_40msps_lds`; the accelerated .lds test fixture renamed to 40msps (the old 20msps-named fixture hit the new rate guard). **190 passing** (158 lib + 6 ffi_plan + 11 lds_integration + 4 parallel_cancel + 11 roundtrip12).

**Real-data verification (dist-vfix CLI, dev-20261010-enc2):** raw u8 4 MB 40 MSPS whole-file via `--encoder auto`: **0.59 s**, `verify: PASS (4194304 samples, bit-perfect)`, `sha256(output): 0334ecb9…`, tags `SOURCE_SHA256=e6d689… RF_SAMPLE_RATE_KHZ=40000 RF_TOTAL_SAMPLES=4194304 RF_SAMPLE_RATE=40000000 DURATION_SECONDS=0.104858 LENGTH=105` (ffprobe).

## Original integration plan (as approved)
Subprocess integration first (the FLAC-Chop way — like sox.exe), then evaluate in-process linking later:
1. **Detection**: resolve `ld-compress-ng[.exe]` beside the app exe, then PATH (new `FLAC_CHOP_LDC` env override + `ldc_available()` probe via `devices`), mirroring the shelldet module.
2. **Whole-file `.lds` → FLAC conversion** (the huge-win case): run `ld-compress-ng compress --backend auto --overwrite IN.lds OUT.flac --progress` in place of the streaming-SoX path. NOTE: compress is whole-file only — partial cuts still need SoX (or a window-pack → compress → trim second pass; deferred decision).
3. **Progress + notes**: parse the `--progress` stderr percentage into the existing PROGRESS plumbing; capture the `compressed N bytes (X%, backend, threads)` summary for the notes.
4. **Metadata**: the fresh output then flows through the EXISTING tag machinery (no padding → shift splice; DATE_RECORDED from the pending rows, owned RF tags win) — already proven to handle fresh native-FLAC outputs.
5. **GUI**: auto-detect + status; a backend chooser (SoX / ld-compress-ng auto/vulkan/opencl) persisted; Batch/Sync tabs gain the option for whole-file `.lds` jobs.
6. **CI/packaging**: build ld-compress-ng for each target + bundle beside the exe (Windows: glslang/Vulkan deps; Linux: Vulkan/OpenCL; macOS: Metal) — a separate follow-up.
7. **License**: subprocess integration keeps FLAC-Chop decoupled from the LGPL core (a separately-built binary). If the core is ever linked in-process, LGPL-2.1 obligations apply to the distributed binaries.

---

## Follow-up 2026-10-10 (evening): About dialog + encoder wording + Batch/Sync verify surfacing

### About dialog (final state, uncommitted)
- Help menu -> "About FLAC-Chop" opens a fixed-size (530x260) QDialog, tbc-tools ld-analyse style:
  app icon left, QTextBrowser right, version label bottom-left, OK bottom-right.
- Content: "FLAC Chop" (16pt bold), "The Universal FM RF Archival editing tool.",
  "(c) 2026 Harry Munday" -> https://github.com/harrypm , "(c) 2026 Joey Lemur" -> https://github.com/JoeyLemur
  (profile hyperlinks on the names). The separate About tab page was removed.
- Implemented in gui/mainwindow.cpp showAboutDialog() (~590-650) + Help menu (~187-189).

### Encoder combo (final wording, uncommitted)
- Output Processing box, label "Encoder:", items on Windows/Linux:
  "Auto (GPU/CPU)" / "GPU" / "SoX (classic streaming)"; on macOS (Q_OS_MAC):
  "Auto (Metal/CPU)" / "Metal" / "SoX (classic streaming)". Generic tooltip,
  no .lds/ld-compress-ng mentions (it processes more than .lds).
- Persisted engine/choice with lds/engine fallback.

### Batch + Sync Edit verify surfacing (this session, uncommitted)
User request: "Also add verify to batch/sync edit pages for data handling."
- gui/batchtab.h/.cpp + gui/syncedittab.h/.cpp: new "Verify output (bit-perfect, before
  deleting sources)" checkbox in each tab's options box (default checked, persisted
  "batch/verify" / "sync/verify", disabled while a run is active).
- processQueue() calls fc_set_verify_output(checked ? 1 : 0) before launching any job.
- BatchResult/SyncResult gained a `verify` field; the workers extract the "verify: ..."
  line from r.stderr_buf (split '\n', startsWith "verify:").
- Success rows now show "Done in <t> - <out>" + the verify verdict line underneath;
  verify: FAILED rows get the warning colour (#e8a040 batch / #b8860b sync).
- Summary lines append " Verified: N passed, M flagged." when any verdict was seen.
- GUI status labels capped at fontMetrics().height() * 5 + 8 (window never grows).

Commands run (this follow-up):
- edit_files on batchtab.h/.cpp + syncedittab.h/.cpp (checkbox, restore, extraction,
  row status, summary counts, setControlsEnabled).
- Build: env MSYSTEM=MINGW64 ... cmake --build build-vfix  (clean, 6 targets).
- Restamped: cmake -S . -B build-vfix -G Ninja -DFLAC_CHOP_VERSION=dev-20261010-vfy
- Rebuilt, copied to dist-vfix, relaunched with the Desktop .lds preloaded.
- Window title confirmed: "FLAC-Chop dev-20261010-vfy - RF capture cutter".

Awaiting user on-screen confirmation of: checkbox on both tabs, verify verdict line
in Done rows, summary "Verified: N passed, M flagged." text.

---

## Follow-up 2026-10-11: file dialog "not the modern Windows one" — root cause + fix

User report: the file selection dialog is not using the modern Windows one.
Per docs/DEV_NOTE_native_file_dialogs.md HARD RULE: enumerate/measure before fixing.

Diagnosis chain (automated, hard data at every step):
1. NEW scripts/enum_filedialog_classes.ps1 + FLAC_CHOP_AUTO_BROWSE=open|dir|multi
   env hook in the MainWindow ctor (permanent dev tooling). ALL three dialogs
   (single-open, folder picker, multi-select) enumerate MODERN: DirectUIHWND +
   DUIViewWndClassName + Breadcrumb Parent + Address Band Root +
   NamespaceTreeControl + Search Box. Classes were NOT the problem.
2. NEW scripts/capture_filedialog_png.ps1 + region mean-RGB: FLAC-Chop dialog =
   dark title bar (RGB 10) + LIGHT body (sidebar 235, list 239) on a DARK-mode
   system (AppsUseLightTheme=0). Reference dialog (pwsh WinForms host, same
   shell IFileDialog) = dark everywhere (54/37/61). The dark-title + light-body
   hybrid is what reads as "the old dialog".
3. Get-Process .Modules with the dialog open: the process loaded app-local
   UxTheme.dll, DWrite.dll, d3d9/11/12.dll, dxgi.dll, dwmapi.dll, WINMM.dll,
   USP10.dll, VERSION.dll, WINHTTP.dll ... — 52 System32 DLLs had been
   ldd-walk-copied into dist-vfix during the earlier bundle assembly. Non-
   KnownDLLs load from the app dir; the out-of-context UxTheme.dll breaks the
   shell dialog's immersive dark-mode resolution -> light body.
4. A/B proof: the SAME exe from a copy of the folder WITHOUT the system DLLs ->
   dialog fully dark (title 6, sidebar 52, list 63, footer 62).

Fix applied:
- NEW scripts/bundle_clean_system_dlls.ps1: deletes bundle DLLs byte-identical
  to C:\Windows\System32 (52 removed from dist-vfix); the bundle's zlib1.dll
  was itself a System32 copy -> replaced with MSYS2 /mingw64/bin/zlib1.dll.
- Post-fix verification from cleaned dist-vfix: dialog dark everywhere
  (title 54, sidebar 33, list 46, footer 40), 0 system DLLs loaded from the
  bundle, --version + --probe (.lds, 40 MSPS 10-bit) pass.
- docs/DEV_NOTE_native_file_dialogs.md: new "2026-10-11 case" section + rules
  (never bundle System32 DLLs; modern classes necessary but not sufficient;
  FLAC_CHOP_AUTO_BROWSE reproducible-dialog hook).
- docs/DEV_NOTE_ldc_acceleration.md bundling section: the same warning added.
- Evidence PNGs kept at C:\Users\Harry\: dialog_appearance_open.png (broken
  hybrid, before), reference_dialog.png, dialog_clean_open.png,
  dialog_fixed_open.png (after).
- dist-test-clean removed after verification.

Awaiting: user's eyes on the relaunched build's dialog (final proof per the
dev note) + the batch/sync verify checkbox confirmation from the 2026-10-10 work.
