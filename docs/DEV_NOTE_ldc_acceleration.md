# DEV NOTE: GPU-accelerated .lds conversion (ld-compress-ng integration)

## Summary

Whole-file `.lds` → FLAC conversions can run through
[`ld-compress-ng`](https://github.com/JoeyLemur/ld-compress-ng) — a focused
LGPL-2.1+ C++20 CLI whose FLAC frame analysis (fixed/Rice + LPC
autocorrelation + candidate costing) runs on GPU compute (Metal on macOS,
Vulkan / OpenCL elsewhere) with a frame-threaded CPU writer. On the dev
machine (GTX 1080 Ti, driver 582.66) the Vulkan backend converts the real
251 MB `RF-Sample_2022-12-11_00-00-34.lds` capture in **15.97 s** where the
SoX streaming path takes **63.97 s (4.0× faster)**, and the output is
**4.3% smaller** (70,196,278 vs 73,357,763 bytes) — decoding **SHA256-identical**
to the reference ld-lds-converter unpack (`0b513122…55ea5d`, 402,653,184 bytes),
MD5 round-trip verified by `ld-compress-ng verify --source`.

## How FLAC-Chop uses it

- **Detection** (`chop.rs ldcdet`): `ld-compress-ng(.exe)` beside the app
  exe, then PATH; the `FLAC_CHOP_LDC` env var overrides (the same model as
  sox/shelldet). `fc_ldc_available()` exposes it to the GUI/CLI.
- **Engine choice** (`ChopOptions::lds_backend`): `Auto` (default) uses the
  accelerated path when resolvable and falls back to SoX on any failure
  (the fallback note lands in stderr); `Ldc` requires it (clear error when
  missing); `Sox` never uses it. `fc_set_lds_backend()` sets the FFI-side
  preference, folded into every `fc_chop`/`fc_chop_ex` call.
- **Scope**: PLAIN whole-file conversions ONLY — `start == 0` and
  `length >= total` with **no** output rate/bit-depth/filter processing.
  The encoder is GLOBAL: whole-file `.lds` **and 8-bit raw (u8/s8)**
  conversions take the GPU path. Raw 8-bit packs exactly onto the `.lds`
  10-bit grid ((b-128)*256 is a multiple of 256, hence of 64; ld-compress-ng's
  wasted-bits support compresses the 8 low zero bits away) — `pack_raw8_to_lds`.
  Constraints: MONO + group-aligned size + **exactly 40 MSPS** (`compress`
  writes a fixed 40 kHz header — a non-40-MSPS capture would be mislabeled;
  this also guards the `.lds` path for `10msps.lds`-style hints).
  16-bit raw (u16/s16 cannot fit the 10-bit grid), WAV, other rates,
  partial cuts, and processed conversions always take the SoX path; a
  required-GPU (Ldc) preference that cannot engage gets a clear error.
  The raw GPU output is 16-bit FLAC holding exactly (b-128)*256 — the
  bit-perfect verify compares at that depth (depth-aware 8-bit readers).
- **Run** (`run_ldc_compress`): `compress --overwrite --progress IN OUT`
  through the shared child machinery — the stderr reader parses the
  `compressing: NN%` line into the existing PROGRESS atomic (the GUI % bar
  works unchanged), Cancel kills the child (its staging publication never
  leaves a partial output), and the final `compressed N bytes (X%, backend,
  threads)` summary surfaces in the notes channel.
- **Post-cut**: the fresh native-FLAC output flows through the SAME
  `validate_cut_output` + `rewrite_tags_after_cut` (RF tag embed + the
  pre-conversion metadata rows) as the SoX path.

## Bit-perfect verification (the pre-deletion safety net)

Converting a tag-less capture (`.lds`, WAV, raw PCM) to FLAC is the workflow
that frees the source for deletion — so the output must be PROVEN to decode
back to the source before anything is deleted. `core/src/verify.rs` does
exactly that, **on by default** (`fc_set_verify_output(0/1)`, GUI checkbox
in the Output Processing box, CLI `--verify`/`--no-verify`; the Batch and
Sync Edit tabs apply their own per-run verify checkboxes —
`fc_set_verify_output` before the jobs launch — and surface each job's
verdict line in its row status, flag FAILED verdicts, and count
"Verified: N passed, M flagged." in the run summary):

- After a successful PLAIN non-FLAC conversion, decode the output FLAC
  (claxon) and compare every sample of the cut window against the source
  read through the native input path (`.lds` group unpack `(value-512)*64`,
  WAV data chunk at its fmt bit depth, raw u8/s8/u16/s16 with SoX's
  width/sign semantics) — streaming both sides, no temps.
- **PASS** → `verify: PASS (N samples, bit-perfect) — safe to delete the
  source` in the notes. **FAIL** → a loud `warning: verify: FAILED after N
  samples: …` (the GUI pops the warning box). Never ignore a FAIL.
- **Processed conversions** (rate/bit-depth/filter) intentionally transform
  the samples: they report an explicit skip (`verify: skipped — … keep the
  source`), NOT a pass. FLAC/Ogg-FLAC sources are lossless cuts by
  construction and get no note.
- **FLAC-Chop never deletes sources itself.** The PASS note is the green
  light for the user's own deletion workflow.

## Windows port of ld-compress-ng (in the local clone)

The `ldcompress_core` library builds clean on MSYS2/MinGW64 as-is; only the
CLI layer is POSIX-bound. The port (in `C:\Users\Harry\ld-compress-ng`,
uncommitted upstream):
- `src/main.cpp`: `BlockTerminationSignals` no-op on Windows (no
  `sigprocmask`; the staging-name randomness + best-effort cleanup is
  accepted), ANSI `signal()` + `raise()` termination cleanup
  (SIGINT/SIGTERM/SIGBREAK), and `rename_no_replace` via `MoveFileExW`
  WITHOUT `MOVEFILE_REPLACE_EXISTING` — Windows' native atomic no-replace
  rename (`ERROR_ALREADY_EXISTS` → `EEXIST`), strictly better than the
  POSIX `link()` fallback which is now gated non-Windows.
- `CMakeLists.txt`: `test_cli` (fork/wait supervision) gated to non-Windows.

Build (MSYS2 MinGW64): install `mingw-w64-x86_64-{glslang,opencl-headers,
opencl-icd}` (flac/ogg/vulkan-headers/vulkan-loader already present), then
`cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake
--build build-mingw`. `devices` enumerates both backends on the GTX 1080 Ti
(Vulkan: `shaderInt64: yes`, "vulkan backend usable: yes"; OpenCL 3.0 CUDA).

## Bundling (portable testing)

Copy beside `flac-chop.exe`:
- `build-mingw/ld-compress-ng.exe`
- Its MinGW runtime DLLs (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`,
  `libwinpthread-1.dll`, `libFLAC.dll`, `libogg.dll`) — from
  `/mingw64/bin`; `vulkan-1.dll` comes with the GPU driver (System32).
- NEVER copy Windows system DLLs (anything in C:\Windows\System32) into
  the bundle — app-local UxTheme/DWrite/d3d*/… copies break the native file
  dialog's dark mode (see docs/DEV_NOTE_native_file_dialogs.md, the
  2026-10-11 case). If a bundle ever gets polluted by an ldd walk, run
  `scripts/bundle_clean_system_dlls.ps1 -BundleDir <dir>`.

## Tests

- `parse_ldc_progress_extracts_last_percentage` (unit): the `--progress`
  stderr line parses the LAST `compressing: NN%` across `\r` updates.
- `lds_whole_file_cut_can_use_accelerated_ldc_backend` (integration, runs
  the real GPU path when `FLAC_CHOP_LDC`/PATH resolves ld-compress-ng):
  byte-exact decode, embedded tags, `verify: PASS`; without the CLI the Ldc
  preference fails with the clear not-found error and no partial output.
- `lds_partial_cut_stays_on_sox_even_with_ldc_backend`: partial cuts keep
  the SoX path under the Ldc preference.
- `verify.rs` unit tests + the whole lds suite (engine-pinned per test).

## CI note (NOT yet done — follow-up)

CI packaging does not yet build/bundle ld-compress-ng on any platform; the
accelerated path simply falls back to SoX there (Auto). A follow-up plan
should add per-platform ld-compress-ng builds (Linux Vulkan/OpenCL, macOS
Metal, Windows glslang+Vulkan) to `build.yml` and bundle the CLI beside the
exe in the portable/installer artifacts.
