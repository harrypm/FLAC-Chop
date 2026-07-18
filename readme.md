# FLAC-Chop — prompt log & project notes

Date: 2026-07-11

## Goal (user request)
Cross-platform cutting tool for RF captures, using Rust + FLAC + SoX, with a
clean Qt6 GUI base. Based off the old C/Raylib work in
`/home/harry/Desktop/Lose-files/flac_chop_gui`. Lives in `/home/harry/FLAC-Chop`.

Rationale for Rust (user pointer): vhs-decode PR #260
(https://github.com/oyvindln/vhs-decode/pull/260, "Rust input file reader
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

## Toolchain (verified)
- rustc/cargo 1.87.0
- SoX v14.4.2 (`/usr/bin/sox`)
- Qt 6.2.4 (qt6-base-dev, qt6-widgets; qmake6)
- cmake 3.22.1, ninja
- cxx-qt would need cmake>=3.24 + mold/lld/gold → rejected; used the
  Rust-staticlib + Qt6-C++-Widgets + CMake approach instead (works on current
  toolchain, mirrors ld-analyse).

## Architecture
```
FLAC-Chop/
├── CMakeLists.txt                 # Qt6 Widgets+Concurrent, AUTOMOC, adds gui/
├── core/                          # Rust staticlib "flac_chop_core" (C ABI)
│   ├── Cargo.toml                 # claxon = "0.4", staticlib+rlib, LTO
│   ├── examples/probe_cli.rs      # headless STREAMINFO probe (validation)
│   ├── examples/chop_cli.rs       # headless probe->plan->sox cutter
│   ├── tests/ffi_plan.rs          # fc_plan FFI integration tests
│   └── src/{lib,probe,msps,chop,ffi}.rs
└── gui/                           # Qt6 C++ QtWidgets
    ├── CMakeLists.txt             # cargo custom command + Qt6 link + pthread/dl/m
    ├── flacchop.h                 # C ABI header mirroring core/src/ffi.rs
    ├── main.cpp
    └── mainwindow.{h,cpp}         # code-built UI, QtConcurrent SoX worker
```

Data flow: `fc_probe` (claxon STREAMINFO) → `fc_plan` (RF sample math) →
`fc_chop` (SoX `trim <start>s <len>s`). The `s` suffix = sample counts.

## RF /1000 convention
RF captures store the real 20 MSPS as 20 kHz in the FLAC header. If the filename
contains `Nmsps`, the plan uses `real_rate = N*1e6` Hz; otherwise it falls back
to the header rate. Cut samples = `round(seconds * real_rate)`.

## Commands run (this session)
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

## Verification (hard data)
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
crash (pid confirmed running). **GUI button behavior pending user real-world
confirmation** (Browse → probe values shown; Process → cut produced) per the
"don't assume GUI works, ask" rule.

## Build / run
```bash
cd /home/harry/FLAC-Chop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/gui/flac-chop
```
Requires: Qt6 dev (Widgets), Rust/cargo, SoX on PATH.

## Headless use
```bash
# probe only
cargo run --release --manifest-path core/Cargo.toml --example probe_cli -- file.flac
# cut
cargo run --release --manifest-path core/Cargo.toml --example chop_cli -- file.flac out.flac <start_sec> <len_sec>
```

## Status / not done
- GUI built in code (no Designer .ui) for compile reliability; can be ported to
  .ui later if desired.
- Windows/macOS: cross-platform BUILDS now work via CI (see session 4), but the
  GUI has NOT been run on Windows/macOS by the user — only the Linux GUI has
  been launched and confirmed. Binaries are verified as well-formed for their
  target arch (ELF/PE32+/universal Mach-O) but not runtime-tested.
- No progress % from sox (busy indicator only); sox doesn't emit sample progress
  to a captured pipe easily.
- Decimation factor from the old tool not carried over (SoX cut is at the input
  rate; decimation is a decode-side concern, not a cut concern).
- No tagged release yet; CI binaries so far are `dev-<sha>` builds. Push a `v*`
  tag to publish a versioned GitHub Release.

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

### Commands run (this session)
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

**Total-resolution priority order:**
1. Vorbis `RF_TOTAL_SAMPLES` tag (in-file, authoritative)
2. STREAMINFO header + 36-bit wrap correction (finalized files)
3. Companion `.log`/`.wav` (unknown-total files with a sibling)
4. Frame-header scan (unknown-total files, no sibling — slow, exact)

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

### Commands run (this session)
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
  noted in the session-1 toolchain block.
- The `actions/*@v4` steps emit Node.js 20 deprecation warnings (runner
  force-upgrades to Node 24). Non-blocking; bumping to `@v5` later clears them.
