# ld-compress-ng

`ld-compress-ng` is a focused command-line compressor for LaserDisc RF capture
files. It compresses packed `.lds` captures to FLAC-backed `.ldf` files,
decompresses them back to `.lds`, and verifies round trips without requiring the
old shell pipeline or helper tools.

By default, `compress` selects the first usable backend in this order: Metal,
Vulkan, OpenCL, then CPU/libFLAC. Metal, Vulkan, and OpenCL write native FLAC
`.flac.ldf` files through the native writer; the CPU/libFLAC fallback writes an
Ogg FLAC `.ldf` with system `libFLAC` and `libogg`. Use `--backend cpu` when
you specifically need the portable Ogg output. The scalar native backends remain
available as reference/debug paths for analysis parity, writer coverage, and
tuning experiments, not as recommended CPU compression choices. Vulkan has
been validated locally on NVIDIA hardware for compatible native FLAC output;
AMD is intended through standard Vulkan compute but not yet hardware-validated.
Metal is macOS-only, uses Apple Command Line Tools plus runtime Metal source
compilation, and has been validated locally on Apple M5 Pro for compatible
native FLAC output and speed-profile throughput in the same class as the Linux
OpenCL/Vulkan accelerators; no Xcode project or offline `.metallib` is required.
Auto selection is based on built support and usable-device discovery; if the
selected encoder later fails to initialize or run, `compress` reports that
error rather than silently rerunning the capture through CPU compression.

`ld-compress-ng` does not depend at runtime on Qt, ffmpeg, `.NET`, Mono, FlaLDF,
OpenSSL, or `ld-lds-converter`.

`ld-compress-ng` is licensed under LGPL-2.1-or-later. See
[`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for
the project license and preserved third-party notices.

## Build

Requirements:

- CMake 3.20 or newer.
- A C++20 compiler.
- `pkg-config` or `pkgconf`.
- `libFLAC` development files.
- `libogg` development files.
- Optional OpenCL headers and loader/framework for `devices` and
  `--backend opencl`.
- Optional Vulkan headers, loader, and `glslangValidator` for Vulkan device
  enumeration and the Linux-first Vulkan backend.
- Optional Apple `Metal.framework` and `Foundation.framework` on macOS for
  `devices`, `--backend metal`, and `bench --include-metal`.

### Linux

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libflac-dev libogg-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Fedora/RHEL-family:

```sh
sudo dnf install gcc-c++ cmake pkgconf-pkg-config flac-devel libogg-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Arch Linux:

```sh
sudo pacman -S base-devel cmake pkgconf flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

For OpenCL compression on Linux, also install OpenCL headers, the ICD loader
development package, and a vendor runtime for your GPU or accelerator. Common
package names include `ocl-icd-opencl-dev`/`opencl-headers` on Debian/Ubuntu,
`ocl-icd-devel`/`opencl-headers` on Fedora, and `ocl-icd`/`opencl-headers` on
Arch.

For the Vulkan backend, install Vulkan development headers, the loader,
`glslangValidator`, and a vendor Vulkan runtime. Vulkan acceleration is
Linux-first; the backend supports `--backend vulkan` and requires a
compute-capable non-CPU device with `shaderInt64` unless you explicitly select
a CPU Vulkan device for functional testing.

### macOS

Install the baseline CPU build dependencies with Homebrew:

```sh
brew install cmake pkg-config flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Xcode Command Line Tools provide AppleClang and the macOS SDK. The SDK may also
provide the deprecated OpenCL framework, but Apple platform OpenCL availability
varies by OS and hardware. The same SDK provides Metal/Foundation for the
macOS-native Metal backend:

```sh
cmake -S . -B build-metal -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_METAL=ON
cmake --build build-metal --parallel
build-metal/ld-compress-ng devices
```

If you want a CPU-only build, disable the accelerator backends:

```sh
cmake -S . -B build-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF \
    -DLDCOMPRESS_ENABLE_METAL=OFF
cmake --build build-cpu-only --parallel
```

Install from a configured build:

```sh
cmake --install build --prefix /usr/local
```

## Basic Usage

Compress an `.lds` capture with automatic backend selection:

```sh
build/ld-compress-ng compress capture.lds
```

This writes `capture.ldf`. To always write the portable Ogg form:

```sh
build/ld-compress-ng compress --backend cpu capture.lds
```

Decompress an `.ldf`, `.raw.oga`, or `.flac.ldf` file back to `.lds`:

```sh
build/ld-compress-ng decompress capture.ldf
```

This writes `capture.lds` by default.

For long captures, `compress --progress` updates one stderr line with compact
LDS input progress, current input throughput, elapsed time, and (once a rate is
available) an ETA. Once all input is consumed it reports `input complete;
finalizing` rather than treating a short final read as a throughput result. The
final progress update reports the end-to-end average rate and elapsed time. Its
percentage is based on the initial size of a regular input file; the final
`compressed ...` line confirms that output finalization and transactional
publication succeeded:

```sh
build/ld-compress-ng compress --progress capture.lds
```

For long captures, add `--progress` to update one stderr line with decoded
sample progress (when STREAMINFO provides a total) and elapsed time:

```sh
build/ld-compress-ng decompress --progress capture.ldf
```

Some legacy `ld-compress`/FlaLDF captures have a stale STREAMINFO total because
the original encoder overflowed FLAC's 36-bit count. `decompress` recovers all
valid frames through physical EOF and reports the discrepancy. Progress changes
to `>=100%` once that advisory total is exceeded; it has not completed until
the command reports its final decoded-sample count.

Verify that a compressed file decodes to the original `.lds` data:

```sh
build/ld-compress-ng verify --source capture.lds capture.ldf
```

Overwrite an existing output only when you ask for it:

```sh
build/ld-compress-ng compress --overwrite capture.lds capture.ldf
build/ld-compress-ng decompress --overwrite capture.ldf capture.lds
```

Compression and decompression publish their output transactionally: each writes
inside a private staging directory beside the destination and publishes only
after the operation finishes successfully. Without `--overwrite`, publication
uses an atomic no-replace rename, so a destination created by another process
during the run is never replaced without requiring hard-link support. Older
platforms or filesystems without no-replace rename support fall back to
hard-link publication when available. With `--overwrite`, the staged payload
is atomically renamed into place. Normal failures and `SIGINT`, `SIGTERM`, or
`SIGHUP` remove the private staging data; as with any process, `SIGKILL` cannot
be cleaned up. This applies to every compression backend.

## Backends

| Backend | Output | Notes |
| --- | --- | --- |
| `auto` | Native FLAC or Ogg FLAC `.ldf` | Default. Chooses Metal, then a usable non-CPU Vulkan device with `shaderInt64`, then a GPU/accelerator-class OpenCL device, then CPU/libFLAC. Explicit `--level` or `--container ogg` selects CPU/libFLAC. |
| `cpu` | Ogg FLAC `.ldf` by default | Portable, uses system `libFLAC`/`libogg`; supports `--level` and can write native FLAC with `--container flac`. |
| `opencl` | Native FLAC `.ldf` | GPU-assisted native encoder; list devices with `devices`, select one with `--device INDEX` or `--opencl-device INDEX`. |
| `vulkan` | Native FLAC `.ldf` | Linux-first acceleration backend with Vulkan exact costing for fixed/Rice and GPU-generated LPC candidates; validated locally on NVIDIA and intended for standard Vulkan compute devices; select one with `--device INDEX` or `--vulkan-device INDEX`. |
| `metal` | Native FLAC `.ldf` | macOS acceleration backend using Apple Metal runtime source compilation for generated LPC and exact costing; select one with `--device INDEX` or `--metal-device INDEX`. |
| `native-fixed` | Native FLAC `.ldf` | Reference/debug scalar encoder for analysis parity, native writer coverage, and tuning sweeps. |
| `native-verbatim` | Native FLAC `.ldf` | Reference/debug path using verbatim FLAC frames. |

Use the scalar native reference backend for diagnostics or tuning comparison:

```sh
build/ld-compress-ng compress --backend native-fixed capture.lds
```

Use OpenCL:

```sh
build/ld-compress-ng devices
build/ld-compress-ng compress --backend opencl --device INDEX capture.lds
```

Use Vulkan:

```sh
build/ld-compress-ng devices
build/ld-compress-ng compress --backend vulkan --device INDEX capture.lds
```

Use Metal on macOS:

```sh
build-metal/ld-compress-ng devices
build-metal/ld-compress-ng compress --backend metal --device INDEX capture.lds
```

For `compress`, `--device INDEX` is backend-local shorthand after `--backend`
selects OpenCL, Vulkan, or Metal. With the default `auto` policy it tests that
index at each candidate backend in priority order. Backend-specific device flags
are valid only when the resolved backend matches; use the matching explicit
backend to pin one. For benchmark runs that include multiple accelerators, use
`--opencl-device INDEX`, `--vulkan-device INDEX`, and `--metal-device INDEX`
because a bare `--device` would be ambiguous.

## Advanced Usage

Show the command summary:

```sh
build/ld-compress-ng --help
```

Show the release version:

```sh
build/ld-compress-ng --version
```

Tune CPU/libFLAC compression level:

```sh
build/ld-compress-ng compress --backend cpu --level 12 capture.lds
```

Write CPU/libFLAC output as native FLAC instead of the default Ogg FLAC
container:

```sh
build/ld-compress-ng compress --backend cpu --container flac capture.lds capture.flac.ldf
```

Run the scalar native reference backend with multiple encoding threads and
summary stats:

```sh
build/ld-compress-ng compress --backend native-fixed --threads 8 --stats capture.lds
```

Native tuning defaults are `--frame-samples 4608`, `--lpc-order 12`,
`--lpc-precision 12`, `--rice-partition-order 5`, and `--threads 1`. These
controls primarily exist for OpenCL/Vulkan/Metal tuning and scalar reference
comparison. For OpenCL, Vulkan, and Metal, `--threads` parallelizes the CPU
selected-frame writer after GPU analysis. Vulkan and Metal still support
`--lpc-order 0` for fixed/Rice-only diagnostics.

Use explicit native FLAC tuning controls when you are comparing reference or
accelerated size/speed tradeoffs:

```sh
build/ld-compress-ng compress --backend native-fixed \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5 \
    capture.lds
```

Benchmark available backends on one capture:

```sh
build/ld-compress-ng bench --threads 8 capture.lds
build/ld-compress-ng bench --threads 8 --include-opencl --opencl-device INDEX capture.lds
build/ld-compress-ng bench --threads 8 --include-vulkan --vulkan-device INDEX capture.lds
build-metal/ld-compress-ng bench --threads 8 --include-metal --metal-device INDEX capture.lds
```

`bench` also accepts comma-separated native tuning grids, `--analysis-profile`,
and `--reuse-opencl-session`/`--reuse-vulkan-session`/`--reuse-metal-session`
for local sweep work. The normal `compress` command keeps the exact native
analysis profile; the faster analysis profiles are benchmarking/tuning
controls.

Valid analysis profiles are `exact`, `order-guess-exact-rice`,
`order-guess-mean-rice`, `order-guess-mean-estimate-rice`,
`subdivide-tukey3-mean-rice`, and
`subdivide-tukey3-mean-estimate-rice`. Use `--opencl-device` and
`--vulkan-device`, and `--metal-device` for explicit accelerator rows; invalid
explicit device indexes fail instead of silently dropping the requested backend.

Convert between packed LDS and signed 16-bit little-endian mono PCM:

```sh
build/ld-compress-ng convert --unpack capture.lds capture.s16
build/ld-compress-ng convert --pack capture.s16 capture.lds
```

## Documentation

- Detailed build, validation, fixture, and tuning notes:
  [`docs/build-and-testing.md`](docs/build-and-testing.md)
- Current implementation plan and project history:
  [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)
- Release checklist:
  [`docs/release-checklist.md`](docs/release-checklist.md)
- Changelog:
  [`CHANGELOG.md`](CHANGELOG.md)
- Third-party license notices:
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
