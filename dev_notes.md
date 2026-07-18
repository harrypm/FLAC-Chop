# FLAC-Chop — developer notes

Historical development log, design rationale, and verification hard data.
The user-facing readme is `readme.md`; this file is the working journal kept
per the "make a prompt readme with all output/input/commands run" rule.

## Goal (original user request)
Cross-platform cutting tool for RF captures, using Rust + FLAC + SoX, with a
clean Qt6 GUI base. Based off the old C/Raylib work in
`/home/harry/Desktop/Lose-files/flac_chop_gui`. Lives in `/home/harry/FLAC-Chop`.

Rationale for Rust (user pointer): vhs-decode PR #260
(https://github.com/oyvind-ln/vhs-decode/pull/260, "Rust input file reader
(u8, s16 and flac supported)") — vhs-decode is porting its FLAC/raw reader to
Rust (libflac-sys). This tool keeps a Rust core for the same reason: native
FLAC metadata reading instead of shelling out to soxi/ffprobe.

## Reference extracted (per "extract from reference, then test on real data")
- Old C tool (`flac_chop_gui.c`): HH:MM:SS IN/Duration/OUT, MSPS-from-filename,
  decimation, RF /1000 header convention, `-cut` output naming, SoX/ffmpeg.
- `tape-decode-rust` FLAC reader: uses symphonia 0.6 (`symphonia-bundle-flac`).
- `ld-analyse` (ld-decode): QtWidgets + CMake, `Qt::Core/Gui/Widgets`,
  AUTOMOC/AUTOUIC, Qt5/6 dual find_package. Mirrored for the GUI.
- claxon API (docs.rs verified): `FlacReader::new_ext(file, FlacReaderOptions{
  metadata_only:true, read_vorbis_comment:false })` reads only STREAMINFO;
  `StreamInfo{ sample_rate:u32, channels:u32, bits_per_sample:u32,
  samples:Option<u64> }`. Pure Rust, cross-platform, no C deps.

## Toolchain (verified locally)
- rustc/cargo 1.87.0
- SoX v14.4.2 (`/usr/bin/sox`)
- Qt 6.2.4 (qt6-base-dev, qt6-widgets; qmake6)
- cmake 3.22.1, ninja
- cxx-qt would need cmake>=3.24 + mold/lld/gold → rejected; used the
  Rust-staticlib + Qt6-C++-Widgets + CMake approach instead (works on current
  toolchain, mirrors ld-analyse).
- Note: CI uses rustc 1.97.1 (runner default), not 1.87.0.

## Architecture deep-dive
```
FLAC-Chop/
├── CMakeLists.txt                 # Qt6 Widgets+Concurrent, AUTOMOC, adds gui/
├── .github/workflows/
│   ├── tests.yml                  # cargo test on push/PR (Linux/macOS/Windows)
│   └── build.yml                  # packaging pipeline (Linux AppImage / Win EXE / macOS DMG)
├── core/                          # Rust staticlib "flac_chop_core" (C ABI)
│   ├── Cargo.toml                 # claxon = "0.4", staticlib+rlib, LTO
│   ├── examples/probe_cli.rs      # headless STREAMINFO probe (validation)
│   ├── examples/chop_cli.rs       # headless probe->plan->sox cutter
│   ├── tests/ffi_plan.rs          # fc_plan FFI integration tests
│   └── src/{lib,probe,msps,rate,vorbis,companions,chop,ffi}.rs
└── gui/                           # Qt6 C++ QtWidgets
    ├── CMakeLists.txt             # cargo custom command + Qt6 link + per-platform Rust std deps
    ├── flacchop.h                 # C ABI header mirroring core/src/ffi.rs
    ├── main.cpp
    ├── rangeslider.{h,cpp}        # IN/OUT dual-handle slider
    └── mainwindow.{h,cpp}         # code-built UI, QtConcurrent SoX worker
```

Data flow: `fc_probe` (claxon STREAMINFO + Vorbis + companions + frame scan)
→ `fc_plan` (RF sample math) → `fc_chop` (SoX `trim <start>s <len>s`). The `s`
suffix = sample counts.

## RF /1000 convention (why durations need correction)
RF captures store the real 20 MSPS as 20 kHz in the FLAC header. If the filename
contains `Nmsps`, the plan uses `real_rate = N*1e6` Hz; otherwise it falls back
to the header rate. Cut samples = `round(seconds * real_rate)`.

This is refined in session 3: `/1000` is the DEFAULT — `real_rate = header_rate
* 1000` — except for standard audio rates (22050/24000/32000/44100/48000/64000/
88200/96000/176400/192000/352800/384000, ±5%) which are used as-is.

## Total-sample resolution priority order (v1.1.0)
1. Vorbis `RF_TOTAL_SAMPLES` tag (in-file, authoritative)
2. STREAMINFO header + 36-bit wrap correction (finalized files)
3. Companion `.log`/`.wav` (unknown-total files with a sibling)
4. Frame-header scan (unknown-total files, no sibling — slow, exact)

---

## 2026-07-11 (session 1) — initial build

### Commands run
```bash
# Rust core
cd /home/harry/FLAC-Chop/core
cargo build --release                       # -> target/release/libflac_chop_core.a (20M)
cargo test --release                        # 9 unit + 5 ffi integration, all pass
cargo run --release --example probe_cli -- <Tape_15.flac>
cargo run --release --example chop_cli -- <Tape_15.flac> /tmp/fc_verify_1s.flac 0 1

# GUI
cd /home/harry/FLAC-Chop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                         # -> build/gui/flac-chop (4.5M)
nohup ./build/gui/flac-chop >/tmp/flacchop.log 2>&1 &   # launched for user test
```

### Verification (hard data)
Source: `/media/harry/20TB HDD1/Lukas_etothepiisreal_Europe_2025/Tape_15/VHS_PAL_SP_Tape_15_2026.04.20_19.52.17_video_rf_8-bit_20msps.flac`

`soxi` (reference): Sample Rate 20000, Precision 8-bit, Channels 1,
49,528,274,364 samples.

Rust `probe_cli` output (matches soxi exactly):
- header_sample_rate: 20000
- bits_per_sample: 8
- channels: 1
- total_samples: 49,528,274,364 (known=true)
- msps_from_name: Some(20.0)
- real_total_seconds: 2476.414 (at real_rate 20,000,000 Hz)

`fc_plan` FFI test: 10 s @ 20 MSPS = exactly 200,000,000 samples (clamps to
total when request exceeds file; rejects zero rate / non-positive length).

`chop_cli` 1 s cut of Tape_15 → `/tmp/fc_verify_1s.flac`, verified by soxi:
- Channels 1, Sample Rate 20000 (header preserved), Precision 8-bit,
  20,000,000 samples. ✓ (engine verified end-to-end on real data)

GUI: builds and links Qt6 (`libQt6Widgets/Core/Gui.so.6`); launches without
crash (pid confirmed running). GUI button behavior confirmed by user (Browse →
probe values shown; Process → cut produced).

---

## 2026-07-11 (session 2) — fix: 36-bit total_samples wrap (real HH:MM:SS)

### Problem (user-reported)
A loaded file did not show its real HH:MM:SS duration. Tape_15 (a ~1h38 VHS
PAL SP capture) showed ~41:16 instead of 01:38:32. User hint: the FLAC header
`total_samples` had wrapped by one full 2^36 "base offset".

### Root cause (verified against hard data)
The FLAC STREAMINFO `total_samples` field is only 36 bits wide (max 2^36 =
68,719,476,736). Captures longer than 2^36 samples wrap modulo 2^36. vhs-decode
documents this exact bug at `vhs-decode/vhsdecode/hifi/utils.py:21-25`
(`FLAC_TOTAL_SAMPLES_FIELD_MOD = 2**36`, `check_flac_header_total_samples`).
The old probe trusted the wrapped header count; the /1000 MSPS->MHz math was
already correct.

Tape_15 hard data (soxi + claxon probe agree):
- header_sample_rate = 20000, bps = 8, channels = 1
- declared total_samples = 49,528,274,364  (the WRAPPED value)
- file_size = 115,365,932,987 bytes, audio_offset = 4,718,682 bytes
- true samples = 49,528,274,364 + 1 * 2^36 = 118,247,751,100
- real duration @ 20 MSPS = 118,247,751,100 / 20,000,000 = 5912.388 s = 01:38:32

The 115 GB file cannot fit the declared 49.5B 8-bit samples (~49.5 GB
uncompressed), proving the header count wrapped.

### Fix
`core/src/probe.rs` now:
1. stats the file size and walks the FLAC metadata block headers to find the
   audio offset (claxon's metadata-only reader does not expose where it
   stopped);
2. ports `check_flac_header_total_samples` from vhs-decode: if the compressed
   audio payload exceeds the bytes the declared count could occupy, the field
   wrapped; recover the true count as `declared + k*2^36` for the unique `k`
   that fits the frame-size [lower, upper] bounds;
3. falls back to the smallest `k>=1` with `declared + k*2^36 >= ceil(audio_bytes
   / bytes_per_sample)` when the frame-size bounds do not yield a unique `k`
   (happens when a silent block sets `min_frame` tiny — Tape_15 has
   min_frame=13). Correct for RF noise, which compresses poorly (Tape_15 is
   97.6% of uncompressed). Marked `estimated` in that case.

New `ProbeResult` / `FcProbe` fields: `declared_total_samples`,
`total_samples_wraps`, `total_samples_estimated`, `file_size`, `audio_offset`.
`total_samples` is now the CORRECTED value. Mirrored in `gui/flacchop.h`.

GUI `setProbeInfo()` shows the corrected duration and a
`(wrap-corrected +N x 2^36, raw <declared>)` tag when wraps > 0; the navigate
slider range now spans the true full-tape duration.

### Commands run
```bash
cd /home/harry/FLAC-Chop/core
cargo test --release                     # 12 unit + 5 ffi, all pass
cargo build --release --example probe_cli
./target/release/examples/probe_cli <Tape_15.flac>
# -> total_samples 118,247,751,100 (wraps=1), 01:38:32.39

cd /home/harry/FLAC-Chop
cmake --build build                      # GUI relinked against new core
nohup ./build/gui/flac-chop >/tmp/flacchop.log 2>&1 &
```

### Verification (hard data)
probe_cli on Tape_15:
```
declared_total     : 49528274364 (raw STREAMINFO, pre-correction)
total_samples      : 118247751100 (known=true, wraps=1, estimated=true)
real_total_seconds : 5912.388  = 01:38:32.39  (at real_rate 20000000 Hz)
```
Matches the user's known real duration 01:38:32. Unit test
`check_detects_wrap_and_recovers_precisely` asserts the +2^36 recovery on
synthetic Tape_15-like inputs.

GUI display CONFIRMED by user on Tape_15: "Total (real)" shows ~01:38:32 with
the wrap-corrected tag; Navigate slider max reaches ~01:38:32 (full tape).

---

## 2026-07-13 (session 3) — v1.1.0: robust duration, RF rate, GUI redesign

### Problems (user-reported)
1. A file loaded with both slider handles stuck at the left (0) — the duration
   was wrong, so the slider had no range.
2. The real-time math was broken for some `msps` files: a file showed "hundreds
   of hours" instead of its real ~35–100 min duration.
3. The GUI froze (then crashed) when loading a 70 GB file whose STREAMINFO
   total was unknown.
4. Loading a new file did not unload the previous file's state.
5. The IN/OUT dual edit boxes glitched on load (a recompute clobbered the
   load-time handle positions).

### Root causes (verified against hard data)
- The `/1000` MSPS->MHz correction was gated on the filename containing an
  `<n>msps` token; `extract_msps` is strict (no separators), so files whose
  name didn't match fell back to the raw 20000 Hz header -> 1000x too long.
- For files with an UNKNOWN STREAMINFO total (piped/streamed captures that
  couldn't finalize the header), the first attempt guessed the count from
  `audio_bytes / bytes_per_sample`. That assumed ~1:1 compression and was
  wrong by 1.74x on Tape_12 (RF noise compresses variably).
- `fc_probe` ran on the GUI thread; for unknown-total files it now scans the
  whole file, freezing the window, and a slice-index panic on a C++ thread
  aborted the process (Rust can't unwind through foreign threads).

### Fixes
**`core/src/rate.rs` (new):** central rate resolution. RF `/1000` is the
DEFAULT — `real_rate = header_rate * 1000`. Exception: standard audio rates
(22050/24000/32000/44100/48000/64000/88200/96000/176400/192000/352800/384000,
±5% tol) are used as-is. Filename `<n>msps` confirms the MSPS value. So a
20000 Hz header with no `msps` in the name now correctly resolves to 20 MSPS.

**`core/src/vorbis.rs` (new):** reads the MISRC/DdD pipeline's custom Vorbis
comment tags `RF_TOTAL_SAMPLES`, `RF_SAMPLE_RATE`, `DURATION_SECONDS` via
claxon (`metadata_only` + `read_vorbis_comment`). These are the capture tool's
authoritative in-file record. The tags are self-consistent:
`RF_TOTAL_SAMPLES / RF_SAMPLE_RATE = DURATION_SECONDS`. The pipeline used two
schemas (early: `RF_SAMPLE_RATE=20000` the /1000 value; later:
`RF_SAMPLE_RATE=20000000` the real Hz, with `RF_SAMPLE_RATE_KHZ` for /1000).
Both work because we divide total by the tag's rate directly — no ×1000
assumption. When present, the Vorbis total is the HIGHEST-priority source.

**`core/src/companions.rs` (new):** for unknown-total files with no Vorbis
tags, infers the duration from a sibling file sharing the capture base prefix
(through the `YYYY.MM.DD_HH.MM.SS` timestamp): a `*.log` with a
`duration=Ns` line, or a `.wav` RIFF header. Verified on Tape_12: the misrc
log says `duration=6120.27s` and the baseband WAV agrees at 6120.82s.

**`core/src/probe.rs` — FLAC frame-header scanner:** last-resort exact count
for unknown-total files with no companion. Walks every FLAC frame header
(mirrors claxon's `frame.rs` bit layout + CRC-8), summing each frame's block
size. Robust against false syncs: requires valid field encodings + CRC-8 match
+ sequential frame/sample-number cross-check. Speedup: 8 MiB read buffer +
skips `min_frame_size` bytes past each accepted frame. Sanity check: the
scanned count's uncompressed size must be >= the compressed audio payload
(FLAC never expands data); warns if not.

**`core/src/ffi.rs`:** `fc_probe` now wrapped in `catch_unwind` — a Rust panic
on the QtConcurrent thread becomes an error string instead of aborting the
process. `fc_plan` signature simplified to take the resolved `real_rate_hz`
directly (drops msps/msps_known/header_rate params).

**GUI (`gui/mainwindow.{h,cpp}`):**
- Probe runs async on a `QtConcurrent` worker thread with a busy indicator +
  "Probing…" status; the window stays responsive (no freeze).
- `unloadFile()` resets all per-file state at the top of `loadFile`, so
  dropping/loading a new file clears the old state first.
- Markers redesigned to one editable time box + Set IN / Set OUT buttons
  (drops the dual IN/Duration edit boxes that glitched). `m_inSec`/`m_outSec`
  are the single source of truth, mutated only by explicit actions (button or
  slider drag) — never by a `textChanged` signal, so load can't be clobbered.
- On load, IN/OUT handles go to each end of the tape (IN at start, OUT at full
  duration) with signals blocked so they stick.
- ld-analyse dark Fusion palette; IN (green) / OUT (red) slider handles.
- Total label shows provenance: `(vorbis RF_TOTAL_SAMPLES)`,
  `(companion file)`, `(scanned from frames)`, or `(wrap-corrected +N×2³⁶)`.

### Commands run
```bash
cd /home/harry/FLAC-Chop/core
cargo test --release                     # 30 unit + 5 ffi, all pass
cargo build --release --example probe_cli
./target/release/examples/probe_cli <metadata_test.flac>   # vorbis-tag path
./target/release/examples/probe_cli <Tape_12.flac>          # companion path

cd /home/harry/FLAC-Chop
cmake --build build                      # GUI relinked against new core
nohup ./build/gui/flac-chop >/tmp/flacchop.log 2>&1 &
```

### Verification (hard data)
metadata_test `09.19.56` (Vorbis tags: RF_TOTAL_SAMPLES=211811850,
RF_SAMPLE_RATE=20000000, DURATION_SECONDS=10.590592):
```
real_rate_hz       : 20000000  (is_rf=true)
total_samples      : 211811850 (known=true, provenance=vorbis-tag)
real_total_seconds : 10.591  = 00:00:10.59
```

Tape_12 (no Vorbis tags; companion misrc log duration=6120.27s):
```
real_rate_hz       : 20000000  (is_rf=true)
total_samples      : 122405400000 (known=true, provenance=companion-file)
real_total_seconds : 6120.270  = 01:42:00.27
```
Matches the user's absolute 01:42:00. Frame-scan fallback on the same file
independently gave 01:42:00.77 (29,886,592 frames scanned) — agrees.

Tape_15 (April-20, 115 GB, wrap path, user absolute 01:38:32):
```
total_samples      : 118247751100 (wraps=1, provenance=wrap-corrected)
real_total_seconds : 5912.388  = 01:38:32.39
```

### Honest caveats
- The GUI load of these files has NOT been confirmed end-to-end by the user at
  commit time; only the headless `probe_cli` path is verified above.
- Companion inference covers sibling `.log` (with `duration=Ns`) and `.wav`
  (RIFF header). RF64 WAVs and `.tbc.json` companions are not handled — those
  fall through to the slow frame scan.
- The frame scan reads the whole file (I/O-heavy); it is the last-resort
  fallback only when no Vorbis tag and no companion exist.

---

## 2026-07-18 (session 4) — cross-platform GitHub Actions CI + binaries

### Goal (user request)
Make GH Actions for all platforms, mirroring the tbc-tools / MISRC GUI repos'
workflow setup; then actually build the binaries and verify them.

### What was added
`.github/workflows/tests.yml` — fast `cargo test --release` + `probe_cli` smoke
build on push/PR to master across ubuntu-22.04 / macos-14 / windows-2022.

`.github/workflows/build.yml` — full packaging pipeline (triggered on `v*` tags,
PRs, or manual `workflow_dispatch`):
- Linux AppImage (x86_64 + arm64) via apt qt6 + linuxdeploy + qmake6
- Windows EXE (x86_64) via MSYS2 MINGW64 Qt6 + windeployqt
- macOS APP (arm64 + x86_64) via brew qt@6 + macdeployqt, then a universal
  lipo-merged DMG
- Release job publishes assets to a GH Release on tag push

`gui/CMakeLists.txt` — cross-platform fixes: staticlib name `.a` vs `.lib` by
toolchain, per-platform Rust std native deps (Linux pthread/dl/m, macOS +iconv,
MinGW ws2_32/advapi32/userenv/bcrypt/ntdll).

### Fix sequence (10 issues, all on CI runners — local Linux build was already
green; details in git log `9a35916..c714009`)
1. Linux apt: Ubuntu 22.04 has no `qt6-widgets-dev` (ships in `qt6-base-dev`).
2. Windows rustup: `rustup-init.exe` ignored the MSYS2 POSIX `CARGO_HOME`;
   switched to `dtolnay/rust-toolchain` with `stable-x86_64-pc-windows-gnu`.
3. Linux linuxdeploy: `qmake` on PATH was Qt5; set `QMAKE=/usr/bin/qmake6`.
4. Linux icon: hand-embedded base64 PNG had a bad IDAT CRC; generate at runtime
   with ImageMagick `convert` instead.
5. macOS runners: `macos-13` retired 2025-12-04 (job queued forever); migrated
   to `macos-15` / `macos-15-intel`.
6. Windows C++ compiler: MSYS2 install list had cmake/qt6 but no gcc; added
   `mingw-w64-x86_64-gcc`.
7. Windows cargo PATH: ninja spawns `cargo` via `cmd.exe` which didn't inherit
   dtolnay's GITHUB_PATH under MSYS2; prepend `$(cygpath -u $CARGO_HOME)/bin`.
8. **FFI `stderr` macro collision (real source bug, not just CI):**
   `char stderr[1024]` in `gui/flacchop.h` collided with MinGW's `stderr` macro
   (`__acrt_iob_func(2)`). Linux/macOS don't define `stderr` as a macro so they
   built fine. Renamed the ABI field to `stderr_buf` across `gui/flacchop.h`,
   `core/src/ffi.rs`, and `gui/mainwindow.cpp`. Verified locally: `cargo test`
   (5 passed) + local CMake GUI build links clean.
9. Windows link: Rust std references NT Native API (`NtReadFile`,
   `NtCreateFile`, `RtlNtStatusToDosError`, ...); added `ntdll` to the CMake
   Windows link list.
10. macOS universal codesign: `codesign --deep` and signing framework *bundle
    dirs* both failed with "bundle format is ambiguous (could be app or
    framework)" on QtQmlMeta.framework. Re-sign leaf dylibs + each framework's
    inner `Versions/Current/<name>` binary, then the app bundle.

### Commands run (this session)
```bash
# local sanity checks after each source fix
cd /home/harry/FLAC-Chop
cargo test --release --manifest-path core/Cargo.toml          # 5 plan tests pass
cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-verify && rm -rf build-verify

# dispatch CI build (no release)
gh workflow run build.yml --ref master -f create_release=false
gh run view <run-id> --json status,conclusion,jobs --jq '...'

# download + verify produced binaries
gh run download <run-id> -n linux-zip-x86_64 -d lx86
gh run download <run-id> -n linux-zip-arm64   -d larm
gh run download <run-id> -n windows-exe       -d win
file lx86/*.AppImage larm/*.AppImage win/flac-chop.exe
```

### Verification (hard data)
Final green run: `29633003940` (all 6 build jobs success, release skipped via
`create_release=false`). Built from `c714009`, so version string `dev-c714009`.

`file(1)` on the downloaded artifacts:
```
lx86/linux_FLAC-Chop_dev-c714009_x86.AppImage:
  ELF 64-bit LSB pie executable, x86-64, static-pie linked, stripped
larm/linux_FLAC-Chop_dev-c714009_arm64.AppImage:
  ELF 64-bit LSB pie executable, ARM aarch64, static-pie linked, stripped
win/flac-chop.exe:
  PE32+ executable (console) x86-64, for MS Windows
```

x86_64 AppImage inner binary (`--appimage-extract usr/bin/flac-chop`):
```
ELF 64-bit LSB pie executable, x86-64, dynamically linked,
interpreter /lib64/ld-linux-x86-64.so.2, stripped
```

Windows exe DLL imports (objdump): `KERNEL32, msvcrt, ntdll, USERENV, WS2_32,
Qt6Core, Qt6Gui, Qt6Widgets, bcryptprimitives` — confirms the `ntdll` link fix
landed and Qt6 DLLs are bundled.

macOS universal DMG (134 MB artifact `macos-app`): the universal job's lipo
arch check (`lipo -archs ... | grep arm64` AND `grep x86_64`) passed in CI
before `hdiutil create`, so `macos_FLAC-Chop_dev-c714009_universal.dmg` is a
verified arm64+x86_64 universal image. (Not downloaded locally — 134 MB; only
its artifact metadata + CI log assertions were checked.)

Artifact sizes:
```
linux-zip-x86_64   24 MB   (AppImage ~26 MB)
linux-zip-arm64    23 MB   (AppImage ~25 MB)
windows-exe        13 MB   (flac-chop.exe 5.5 MB + Qt6 DLLs)
macos-app         134 MB   (universal .dmg)
```

### Honest caveats
- These binaries are BUILT and format/arch-verified, NOT runtime-tested. The
  Linux GUI is the only one the user has actually launched (session 1–3).
  Running the Windows exe or macOS .app and loading a real RF file is still
  pending user confirmation.
- No SoX bundling on any platform — SoX remains a runtime dep the user installs
  (`apt install sox` / `brew install sox` / SoX for Windows on PATH). The
  release `body` in build.yml documents this.
- `cargo test` under CI uses rustc 1.97.1 (runner default), not the 1.87.0
  noted in the toolchain block above.
- The `actions/*@v4` steps emit Node.js 20 deprecation warnings (runner
  force-upgrades to Node 24). Non-blocking; bumping to `@v5` later clears them.

---

## 2026-07-18 (session 5) — fix Windows x86_64 runtime (missing MinGW DLLs) + add Windows arm64 build

### Goal / problem (user-reported)
The CI-built Windows x86_64 `flac-chop.exe` from session 4's green run
(`29633003940`, commit c714009) failed at runtime — it would not start
(missing-DLL / crash). Session 4 had explicitly caveatted that the Windows
binary was format/arch-verified but never runtime-tested. User also asked to
add a Windows arm64 build.

### Root cause (verified against hard data, not guessed)
Downloaded the green `windows-exe` artifact (run 29633003940) and dumped PE
import tables with `objdump -p`:
- `flac-chop.exe` itself imports only Windows system DLLs + Qt6Core/Gui/Widgets
  (all bundled by windeployqt). Qt6Concurrent is NOT imported —
  `QtConcurrent::run` is header-inline templates in Qt6. The MinGW runtime was
  statically linked INTO THE EXE (no libgcc_s_seh-1 / libwinpthread-1 /
  libstdc++-6 in its imports) thanks to CMakeLists `-static-libgcc
  -static-libstdc++ -static`.
- BUT the bundled Qt6 DLLs (Qt6Core/Gui/Network) each import a tree of MinGW
  runtime + MSYS2 third-party DLLs that windeployqt did NOT bundle:
  libgcc_s_seh-1, libwinpthread-1, libstdc++-6, libpcre2-16-0, zlib1, libzstd,
  libdouble-conversion, libb2-1, libicuin78 / libicuuc78, libfreetype-6,
  libharfbuzz-0, libmd4c, libpng16-16, libbrotlidec (+ libbrotlicommon), ...
  windeployqt is built for MSVC and has no knowledge of MSYS2 MinGW runtime
  DLLs, so it copies none of them.
- Result: the loader resolved Qt6Core.dll → its deps → missing libgcc_s_seh-1
  etc. → the "missing DLL / won't start" failure the user saw.

A local dry-run of a single-pass `ldd`-walk against the downloaded dist
(found 9 deps present in the local /mingw64/bin) proved a single pass MISSES
multi-level transitives: `libbrotlicommon` (dep of libbrotlidec) was only
revealed once libbrotlidec had been copied into dist/, because ldd only
recurses into deps that resolve from dist. It also proved a strict leak-check
over ALL dist DLLs gives FALSE POSITIVES (plugins in subdirs resolve their
runtime deps via PATH under ldd, even though at runtime the loader searches
the exe's directory first and finds them in dist/ root). `ldd` on the exe
alone was clean after the copy → that is the reliable gate.

### Fix
`gui/CMakeLists.txt`:
- staticlib extension now `if(MSVC)` (was `WIN32 AND NOT MINGW`) so the
  LLVM-mingw arm64 toolchain picks `.a` even if CMake doesn't set MINGW for
  clang. No-op for the existing MinGW-gcc x86_64 job (MSVC false → .a).
- MinGW static-link flags now gated on `CMAKE_CXX_COMPILER_ID STREQUAL "GNU"`
  (was `WIN32 AND MINGW`) so they're never passed to clang on arm64; for that
  build the runtime DLLs are bundled by the ldd-walk instead. No-op for gcc.

`.github/workflows/build.yml`:
- x86_64 job: after windeployqt, a FIXPOINT `ldd`-walk copies every
  /mingw64/bin DLL the exe + bundled Qt6 DLLs + plugins transitively need into
  dist/, iterating until a pass copies nothing new. Sanity gate: `ldd
  dist/flac-chop.exe` (exe tree only) must have no /mingw64/bin refs. Renamed
  the zip `..._x86.zip` → `..._x86_64.zip`.
- NEW `windows-arm64` job: `runs-on: windows-11-arm` (GA, free for public
  repos) + MSYS2 `CLANGARM64` (packages mingw-w64-clang-aarch64-{clang,cmake,
  ninja,pkgconf,qt6-base,qt6-tools}) + Rust `stable-aarch64-pc-windows-gnullvm`
  (LLVM-mingw arm64 ABI matches CLANGARM64 Qt6; MSVC would ABI-mismatch and
  fail to link) installed as the job's default toolchain so `cargo build
  --release` (no --target) lands the staticlib at core/target/release/
  libflac_chop_core.a (matching gui/CMakeLists.txt). Same fixpoint ldd-walk
  against /clangarm64/bin. Artifact `windows-exe-arm64` → `..._arm64.zip`.
- release job: `needs` += windows-arm64; `files` globs updated
  (windows_FLAC-Chop_*_x86_64.zip + windows_FLAC-Chop_*_arm64.zip).

Toolchain facts confirmed before writing the arm64 job: `windows-11-arm` is a
GA GitHub-hosted runner for public repos (actions/runner-images table);
MSYS2 CLANGARM64 packages exist (local pacman -Sl clangarm64):
mingw-w64-clang-aarch64-clang 22.1.4, -qt6-base 6.11.0, -qt6-tools 6.11.0,
-cmake 4.3.3, -ninja 1.13.2, -pkgconf; `aarch64-pc-windows-gnullvm` is a valid
Rust target (local `rustc --print target-list`).

### Commands run (this session)
```bash
# investigate the green CI artifact (downloaded via gh run download 29633003940)
objdump -p flac-chop.exe | grep 'DLL Name:'           # exe imports
objdump -p Qt6Core.dll | grep 'DLL Name:'             # Qt6 DLL deps -> the missing tree
# local dry-run of the ldd-walk logic (proved fixpoint + exe-only gate needed):
#   C:\Users\Harry\fc-win-inspect\test_ldd_walk.sh (MSYS2 MINGW64)

# local mirror of the CI x86_64 build (MSYS2 MINGW64 + gnu Rust toolchain):
#   installed: pacman -S mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-tools
#   installed: rustup toolchain install stable-x86_64-pc-windows-gnu
#   RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu \
#   cmake -S . -B build-local -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-local
#   windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw dist/flac-chop.exe
#   <fixpoint ldd-walk, BINDIR=/mingw64/bin>
#   script: C:\Users\Harry\fc-win-inspect\local_build_x86_64.sh
```

### Verification (hard data) — x86_64
Local build (mirrors CI): cargo 1.97.1 (gnu), claxon 0.4.3, Qt 6.11.0, GNU
16.1.0 → `build-local/gui/flac-chop.exe`.

PE: `objdump -f dist/flac-chop.exe` → `file format pei-x86-64`,
`architecture: i386:x86-64`.

Fixpoint ldd-walk copied 28 DLLs into dist/ root (verbatim):
libb2-1, libbrotlicommon, libbrotlidec, libbz2-1, libdouble-conversion,
libffi-8, libfreetype-6, libgcc_s_seh-1, libgio-2.0-0, libglib-2.0-0,
libgmodule-2.0-0, libgobject-2.0-0, libgraphite2, libharfbuzz-0, libiconv-2,
libicudt78, libicuin78, libicuuc78, libintl-8, libjpeg-8, libmd4c,
libpcre2-16-0, libpcre2-8-0, libpng16-16, libstdc++-6, libwinpthread-1,
libzstd, zlib1. (MSYS2 Qt6 Gui's libharfbuzz is built WITH glib, so the whole
glib stack — glib/gobject/gio/gmodule + ffi/intl/iconv/pcre2-8/graphite2 +
bz2 — is pulled in transitively; the fixpoint loop captured all of it, which
a single-pass walk would have missed.)

Exe-tree leak gate: EXE_TREE_CLEAN (ldd dist/flac-chop.exe has no
/mingw64/bin refs after bundling).

USER RUN-TEST (real-world confirmation): launched
C:\Users\Harry\flac-chop\dist\flac-chop.exe — "It opens now without error
prompts." The missing-DLL runtime failure is FIXED. Restore-point snapshot
zipped at C:\Users\Harry\fc-restore-points\2026-07-18_windows-x86_64-runtime-fix\
(working dist + changed source + this log).

### Honest caveats
- arm64 CI-VERIFIED green on the first run (windows-11-arm + CLANGARM64 +
  gnullvm Rust linked cleanly, no iteration needed; see "CI verification" below).
  Still NOT runtime-tested on an arm64 device — only arch + dep-bundling
  verified. The risks flagged below (runner availability, dtolnay gnullvm
  toolchain spec, gnullvm link deps) did NOT materialize.
- The x86_64 run-test confirmed the GUI STARTS. An end-to-end cut (Process →
  sox trim) still needs SoX on PATH + a real RF file; not re-tested here.
- x86_64 dist grew: root ~82 MB uncompressed (dominated by libicudt78 ~32 MB),
  so the zip is now ~35-40 MB (was ~14 MB when it was broken/missing DLLs).
  Exact compressed size to be confirmed from the CI artifact.
- Committed as 22751a8 on fix/windows-x86_64-runtime-and-arm64, pushed to
  origin, and verified via CI run 29641872959 (create_release=false) — see
  "CI verification (hard data)" below.

### CI verification (hard data)
Dispatched build.yml on fix/windows-x86_64-runtime-and-arm64 (run 29641872959,
create_release=false). Result: run completed success; all 8 build jobs green
(Cargo tests, Windows EXE x86_64, Windows EXE arm64, macOS APP arm64/x86_64/
universal, Linux AppImage arm64/x86_64); Publish release assets correctly
skipped.

Downloaded both Windows artifacts (gh run download 29641872959) and inspected:
- windows-exe (x86_64): flac-chop.exe PE Machine 0x8664 (x86-64); 32 root
  DLLs = the 28 MinGW/third-party runtime DLLs + 4 Qt6 — matches the local
  build exactly. The fix landed on CI.
- windows-exe-arm64 (arm64): flac-chop.exe PE Machine 0xAA64 (a genuine
  aarch64 Windows PE); 25 root DLLs bundled, including libc++.dll (the
  LLVM-mingw C++ runtime — CLANGARM64 uses libc++, not libstdc++) plus
  ICU/freetype/harfbuzz/glib/pcre2/zlib/zstd/brotli. The fixpoint ldd-walk
  captured the arm64 dep tree (which differs from x86_64: no
  libgcc_s/libwinpthread/libstdc++-6; libc++.dll instead) correctly.

arm64 is CI-built + arch/dep-verified but NOT runtime-tested on an arm64
device locally. The windows-11-arm runner provisioned with no queue issue;
the dtolnay stable-aarch64-pc-windows-gnullvm toolchain + CLANGARM64 Qt6
linked cleanly on the first run.
