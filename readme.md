# FLAC-Chop

A small, cross-platform tool for **sample-exact cutting of RF-capture FLAC files**
(LaserDisc / VHS / etc. as produced by the Domesday Duplicator / MISRC pipeline).
A Rust core reads the FLAC metadata directly (no `soxi`/`ffprobe` shell-out) and
[SoX](https://sox.sourceforge.net/) performs the actual cut. The GUI is Qt6.

It correctly handles the things that trip up generic audio editors on these
files: the RF "20 kHz header = 20 MSPS real" convention, the 36-bit
`total_samples` wrap that long captures hit, and unfinalized/piped captures
whose FLAC header total is unknown.

## Features
- Real-time HH:MM:SS duration for RF captures, not the 1000×-wrong header value.
- Handles `total_samples` wrapping past 2³⁶ (recovers the true sample count).
- Reads the MISRC/DdD Vorbis tags (`RF_TOTAL_SAMPLES`, `RF_SAMPLE_RATE`,
  `DURATION_SECONDS`) as the authoritative in-file record.
- Falls back to a sibling `.log`/`.wav` for unfinalized captures, then to an
  exact FLAC frame-header scan.
- Async probing — loading a 100 GB file doesn't freeze the window.
- IN / OUT markers via a single time box + Set IN / Set OUT buttons (ld-analyse
  style), with a dual-handle slider.
- Headless `probe_cli` and `chop_cli` for scripting / validation.

## Requirements
**Runtime:**
- **SoX** on PATH — FLAC-Chop shells out to `sox` for the actual cut. It is not
  bundled. Install it separately:
  - Linux: `sudo apt install sox`
  - macOS: `brew install sox`
  - Windows: install [SoX for Windows](https://sourceforge.net/projects/sox/) and add it to PATH.

**Build from source:**
- Rust / cargo (stable)
- Qt 6 (Widgets, Concurrent) + dev headers
- CMake ≥ 3.16 and Ninja (recommended)

## Get prebuilt binaries
Cross-platform builds run on GitHub Actions (`.github/workflows/build.yml`).

- **Latest CI build (any branch):** download the workflow run artifacts from
  the [Actions tab](https://github.com/harrypm/FLAC-Chop/actions) —
  `linux-zip-x86_64`, `linux-zip-arm64`, `windows-exe`, `windows-exe-arm64`,
  `macos-app` (universal DMG). These are `dev-<sha>` builds.
- **Versioned releases:** push a `v*` tag (e.g. `v1.2.0`) and the release job
  publishes the same assets to a GitHub Release automatically.

Artifacts:

| Platform | Artifact | Contents |
|---|---|---|
| Linux x86_64 | `linux-zip-x86_64` | AppImage (~26 MB) |
| Linux arm64 | `linux-zip-arm64` | AppImage (~25 MB) |
| Windows x86_64 | `windows-exe` | `flac-chop.exe` + Qt6 + MinGW runtime DLLs (ZIP) |
| Windows arm64 | `windows-exe-arm64` | `flac-chop.exe` + Qt6 + LLVM-mingw runtime DLLs (ZIP) |
| macOS universal | `macos-app` | universal `FLAC-Chop.dmg` (arm64 + x86_64, ~134 MB) |

> SoX is still a separate runtime install on every platform (see above).

## Build from source
```bash
cd FLAC-Chop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/gui/flac-chop
```
This builds the Rust core (`cargo build --release`, invoked by CMake) and links
the Qt6 GUI against it.

Linux apt example:
```bash
sudo apt install cmake ninja-build pkg-config qt6-base-dev rustc cargo sox
```

## Using the GUI
1. **Browse** to a `.flac` RF capture (or drag-and-drop one onto the window).
2. The probe runs on a background thread; the "Total (real)" label shows the
   real-time duration and a provenance tag (`vorbis`, `companion file`,
   `scanned from frames`, or `wrap-corrected +N×2³⁶`).
3. Move the slider or type a time into the time box, then click **Set IN** /
   **Set OUT** to drop the IN (green) and OUT (red) markers. On load the
   handles sit at the start/end of the tape.
4. Click **Process**. FLAC-Chop writes `<input>-cut.flac` next to the source
   via `sox <in> <out> trim <start>s <len>s` (the `s` suffix = sample counts,
   so the cut is sample-exact at the real MSPS rate).

## Headless use
```bash
# probe a file (print real rate, total samples, real duration, provenance)
cargo run --release --manifest-path core/Cargo.toml --example probe_cli -- file.flac

# cut: chop_cli <in.flac> <out.flac> <start_sec> <len_sec>
cargo run --release --manifest-path core/Cargo.toml --example chop_cli -- file.flac out.flac 60 10
```
The CLIs run the exact same probe → plan → SoX path as the GUI.

## Status & limitations
- The GUI has been launched and confirmed on **Linux** only. The Windows and
  macOS binaries are built by CI and verified as well-formed for their target
  architecture, but have **not** been runtime-tested yet — feedback welcome.
- No progress percentage during a cut (SoX doesn't emit sample progress to a
  captured pipe easily); the GUI shows a busy indicator instead.
- The cut is at the input sample rate. Decimation is a decode-side concern and
  is not performed here.
- No tagged release yet; CI binaries so far are `dev-<sha>` builds.

## Development notes
Design rationale, reference extraction, the per-session development log, and
verification hard data are in [`dev_notes.md`](dev_notes.md).
