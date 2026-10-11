# Detailed Build, Testing, And Development Notes

`ld-compress-ng` is a C++20/CMake project targeting Linux and macOS on arm64 and
amd64/x86_64. The normal CPU compressor is intentionally small: it does not
depend on Qt, ffmpeg, `.NET`, Mono, FlaLDF, OpenSSL, or `ld-lds-converter` at
build time or runtime. Some optional compatibility tests use local reference
tools when they are available and skip cleanly when they are not.

## Dependency Matrix

| Component | Required | Purpose | Notes |
| --- | --- | --- | --- |
| C++20 compiler | Yes | Build all targets | AppleClang, Clang, or GCC with C++20 support. |
| CMake 3.20+ | Yes | Configure/build | Matches the project minimum in `CMakeLists.txt`. |
| `pkg-config` / `pkgconf` | Yes | Locate FLAC/Ogg | CMake uses imported pkg-config targets. |
| `libFLAC` development files | Yes | CPU FLAC encode/decode | Requires pkg-config module `flac`. |
| `libogg` development files | Yes | Ogg FLAC container support | Requires pkg-config module `ogg`. |
| OpenCL headers + loader/framework | Optional | `devices` enumeration and OpenCL compression backend | Disable with `-DLDCOMPRESS_ENABLE_OPENCL=OFF`. |
| Vulkan headers + loader + `glslangValidator` | Optional | Vulkan `devices` enumeration and Linux-first Vulkan backend | Disable with `-DLDCOMPRESS_ENABLE_VULKAN=OFF`. |
| Apple Metal + Foundation frameworks | Optional on macOS | Metal `devices` enumeration and macOS Metal backend | Provided by Command Line Tools/macOS SDK; disable with `-DLDCOMPRESS_ENABLE_METAL=OFF`. |
| Python 3 interpreter | Optional | Skip-safe external decode compatibility CTests and helper scripts | CMake adds Python-based tests only when an interpreter is found. |
| `ffmpeg`/`ffprobe` | Optional | External native-FLAC decode compatibility CTest and legacy fixture regeneration | The compatibility test skips if `ffmpeg` is unavailable. |
| PyAV and reference `ld-decode` dependencies | Optional | External `ld-decode` loader compatibility CTests | Tests skip if the local reference tree or imports are unavailable. |
| `ld-lds-converter` | No | Legacy fixture regeneration only | Reference binary may live under `build-ld-lds-converter/`. |
| Qt | No | Reference converter build only | `ld-compress-ng` itself does not link Qt. |

## macOS

Install the baseline CPU build dependencies with Homebrew:

```sh
brew install cmake pkg-config flac libogg
```

Xcode Command Line Tools provide AppleClang, the macOS SDK, and the
Metal/Foundation frameworks used by the macOS Metal backend. The SDK may also
provide the deprecated but still linkable OpenCL framework used by optional
OpenCL support. If OpenCL is unavailable or intentionally unwanted, configure
with `-DLDCOMPRESS_ENABLE_OPENCL=OFF`.

Apple has deprecated OpenCL and current macOS systems may build the OpenCL path
without exposing any usable OpenCL devices. Use `--backend metal` for native
macOS GPU acceleration. The Metal backend embeds its Metal Shading Language
source and compiles it at runtime with `newLibraryWithSource`; full Xcode,
`metal`, `metallib`, and an Xcode project are not required.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Metal-focused local build:

```sh
cmake -S . -B build-metal -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_METAL=ON
cmake --build build-metal --parallel
build-metal/ld-compress-ng devices
ctest --test-dir build-metal -L metal --output-on-failure
```

Some managed sandboxes hide `MTLCreateSystemDefaultDevice()` even on systems
with a working Metal GPU. In that case the Metal tests print skip messages in
the sandbox; rerun `ld-compress-ng devices` and `ctest -L metal` from an
unsandboxed terminal for hardware validation.

Install from a configured build:

```sh
cmake --install build --prefix /usr/local
```

CPU-only configure:

```sh
cmake -S . -B build-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF \
    -DLDCOMPRESS_ENABLE_METAL=OFF
cmake --build build-cpu-only --parallel
ctest --test-dir build-cpu-only --output-on-failure
```

## Linux

Debian/Ubuntu baseline CPU build:

```sh
sudo apt install build-essential cmake pkg-config libflac-dev libogg-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Fedora/RHEL-family baseline CPU build:

```sh
sudo dnf install gcc-c++ cmake pkgconf-pkg-config flac-devel libogg-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Arch Linux baseline CPU build:

```sh
sudo pacman -S base-devel cmake pkgconf flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For OpenCL enumeration on Linux, install OpenCL headers plus the ICD loader
development files, and make sure a vendor ICD/runtime is installed for the
actual hardware. Examples include Mesa/Rusticl, Intel NEO, NVIDIA, or ROCm.
OpenCL headers/linker support alone is enough to build, but it does not
guarantee that `ld-compress-ng devices` will find usable hardware.

Common optional package names:

| Distribution | OpenCL development packages |
| --- | --- |
| Debian/Ubuntu | `ocl-icd-opencl-dev`, `opencl-headers`, optional `clinfo` |
| Fedora/RHEL-family | `ocl-icd-devel`, `opencl-headers`, optional `clinfo` |
| Arch Linux | `ocl-icd`, `opencl-headers`, optional `clinfo` |

For Vulkan acceleration, install Vulkan headers, the loader development package,
`glslangValidator`, and a vendor runtime. Common package names:

| Distribution | Vulkan development packages |
| --- | --- |
| Debian/Ubuntu | `libvulkan-dev`, `glslang-tools`, optional `vulkan-tools` |
| Fedora/RHEL-family | `vulkan-headers`, `vulkan-loader-devel`, `glslang`, optional `vulkan-tools` |
| Arch Linux | `vulkan-headers`, `vulkan-icd-loader`, `glslang`, optional `vulkan-tools` |

`LDCOMPRESS_ENABLE_VULKAN=ON` is opportunistic: CMake enables Vulkan only when
both Vulkan development files and `glslangValidator` are found. Without them,
`ld-compress-ng devices` reports `Vulkan support: not built`.

When Vulkan is enabled, CMake compiles the checked-in GLSL shaders to SPIR-V
with `glslangValidator`. The test-only smoke shader is passed to
`vulkan_smoke`; the fixed/constant/LPC exact-analysis shader is embedded into
the binary for `--backend vulkan`. Set `LDCOMPRESS_VULKAN_TEST_DEVICE` at
configure time to force a specific backend-local Vulkan device index for the
Vulkan CTests.

### Linux OpenCL Validation Record

The current OpenCL analysis smoke tests were validated on `smaug`:

- OS/kernel: Debian amd64, `Linux smaug 6.12.94+deb13-amd64`.
- OpenCL platform: NVIDIA CUDA, OpenCL 3.0 CUDA 13.3.44.
- Driver: NVIDIA `610.43.02`.
- Devices reported by `ld-compress-ng devices`:
  - `[0] NVIDIA GeForce RTX 4070 SUPER`, 56 compute units,
    12,481,134,592 bytes global memory.
  - `[1] NVIDIA GeForce RTX 5070 Ti`, 70 compute units,
    16,652,042,240 bytes global memory.

Validation commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=ON
cmake --build build --parallel
build/ld-compress-ng devices
build/test_opencl_analysis
ctest --test-dir build --output-on-failure
```

Observed result: `build/test_opencl_analysis` exited successfully without skip
messages, and the non-real-fixture CTest suite passed. The test currently
selects the first available OpenCL device, so this validates device `[0]` unless
future tests add explicit multi-device coverage.

### Linux Vulkan Development Validation Record

The current Vulkan device enumeration and smoke shader were validated on
`smaug` with Vulkan development files and `glslangValidator` installed. Devices
reported by `ld-compress-ng devices` from a GPU-visible context:

- `[0] AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)`, integrated GPU.
- `[1] NVIDIA GeForce RTX 5070 Ti`, discrete GPU.
- `[2] NVIDIA GeForce RTX 4070 SUPER`, discrete GPU.
- `[3] llvmpipe (LLVM 19.1.7, 256 bits)`, CPU Vulkan implementation.

Validation commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_VULKAN=ON
cmake --build build --parallel
build/ld-compress-ng devices
ctest --test-dir build -L vulkan --output-on-failure
build/test_vulkan_smoke build/shaders/vulkan_smoke.comp.spv --device 1
build/test_vulkan_analysis --device 1
build/ld-compress-ng compress --backend vulkan --device 1 capture.lds
```

Observed result: the Vulkan-labelled CTests passed, direct fixed/Rice and LPC
analyzer parity passed on NVIDIA device `[1]`, and direct Vulkan CLI compression
on that NVIDIA device verified back to the source LDS bytes. Direct smoke runs
also passed on the AMD integrated GPU plus both NVIDIA GPUs. Sandboxed runs may
expose only llvmpipe; use a GPU-visible context for NVIDIA/AMD runtime
validation.

### Linux Large-Capture STREAMINFO Validation

FLAC STREAMINFO can represent at most `(2^36)-1` samples. An LDS file stores
four samples in five bytes, so an 80 GiB LDS capture (`85,899,345,920` bytes)
is the first valid LDS size whose sample count does not fit. At that boundary
and above, every backend must write `0` (unknown) in STREAMINFO rather than
failing at finalization. The PCM MD5 remains the full-stream integrity value.

The boundary is unit-tested without creating an 80 GiB fixture. When a real
large capture and enough scratch space are available on Linux, use this as the
hardware/runtime validation breadcrumb. Run one output at a time if scratch
capacity cannot hold all four compressed files:

```sh
large_capture=/path/to/capture-over-80-GiB.lds
large_scratch=/path/to/large/scratch
test "$(stat -c %s "$large_capture")" -ge 85899345920
mkdir -p "$large_scratch"

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=ON \
    -DLDCOMPRESS_ENABLE_VULKAN=ON
cmake --build build --parallel
build/ld-compress-ng devices
ctest --test-dir build --output-on-failure

build/ld-compress-ng compress --backend cpu --container flac \
    "$large_capture" "$large_scratch/cpu.flac.ldf"
metaflac --show-total-samples "$large_scratch/cpu.flac.ldf"
build/ld-compress-ng verify --source "$large_capture" \
    "$large_scratch/cpu.flac.ldf"

build/ld-compress-ng compress --backend native-fixed --threads 8 \
    "$large_capture" "$large_scratch/native-fixed.flac.ldf"
metaflac --show-total-samples "$large_scratch/native-fixed.flac.ldf"
build/ld-compress-ng verify --source "$large_capture" \
    "$large_scratch/native-fixed.flac.ldf"

build/ld-compress-ng compress --backend opencl --device OPENCL_INDEX --threads 8 \
    "$large_capture" "$large_scratch/opencl.flac.ldf"
metaflac --show-total-samples "$large_scratch/opencl.flac.ldf"
build/ld-compress-ng verify --source "$large_capture" \
    "$large_scratch/opencl.flac.ldf"

build/ld-compress-ng compress --backend vulkan --device VULKAN_INDEX --threads 8 \
    "$large_capture" "$large_scratch/vulkan.flac.ldf"
metaflac --show-total-samples "$large_scratch/vulkan.flac.ldf"
build/ld-compress-ng verify --source "$large_capture" \
    "$large_scratch/vulkan.flac.ldf"
```

Each `metaflac` command must print `0`, and every `verify --source` command must
succeed. Replace the backend-local device indexes with values printed by
`ld-compress-ng devices`. The CPU row explicitly selects native FLAC so the
same STREAMINFO field is directly inspectable; CPU/libFLAC already applies the
unknown-length convention internally. On macOS, use the same procedure for
`cpu`, `native-fixed`, and `metal`, replacing the final backend with
`--backend metal --device METAL_INDEX` and using `stat -f %z` for the size
check.

### Legacy STREAMINFO Overflow Recovery

Older FlaLDF captures may instead contain a nonzero 36-bit total that wrapped
while their FLAC frames continue to physical EOF. The decoder treats an
underreported total as advisory and prints both counts after it has recovered
the complete stream. It still rejects a stream that ends before its declared
total.

The known Indiana Jones capture has a declared total of `3758096384` samples.
One 36-bit wrap gives an expected recovered total of `72477573120` samples,
or `90596966400` packed-LDS bytes. Recovering it needs at least 85 GiB of free
scratch space for the output alone:

```sh
legacy_capture=reference/discs/indiana_jones_and_the_temple_of_doom_s1.flac.ldf
legacy_output=/path/with/scratch/indy1-full.lds

build/ld-compress-ng decompress --progress "$legacy_capture" "$legacy_output"
stat -f %z "$legacy_output"  # expected: 90596966400
```

Use `ld-decode` with a start field past the former 2,523-frame cutoff to check
that recovery continues beyond the opening credits.

### Linux Accelerated Writer Lifetime Validation

OpenCL, Vulkan, and Metal all queue selected-frame writer jobs through the same
accelerated host path. The `accelerated_native_backend` regression makes one of
two writer workers fail while the other is still reading the analyzed sample
batch. It is device-independent, so first replay the lifetime check under
AddressSanitizer and UndefinedBehaviorSanitizer on Linux:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF \
    -DLDCOMPRESS_ENABLE_METAL=OFF \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-sanitize --parallel \
    --target test_accelerated_native_backend
ctest --test-dir build-sanitize -R '^accelerated_native_backend$' \
    --output-on-failure
```

Then use a GPU-visible Linux shell for the normal OpenCL/Vulkan build and
hardware lanes. Replace the device indexes with values printed by `devices`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=ON \
    -DLDCOMPRESS_ENABLE_VULKAN=ON
cmake --build build --parallel
build/ld-compress-ng devices
ctest --test-dir build -R '^accelerated_native_backend$' --output-on-failure
ctest --test-dir build -L 'opencl|vulkan' --output-on-failure
build/test_vulkan_analysis --device VULKAN_INDEX
```

The regression must exit successfully with no sanitizer report. The labeled
suites must run against real devices rather than skip or select llvmpipe before
counting the Linux accelerator validation complete.

CPU-only configure on Linux is the same as macOS:

```sh
cmake -S . -B build-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF
cmake --build build-cpu-only --parallel
ctest --test-dir build-cpu-only --output-on-failure
```

## Verifying The Built Binary

The binary should show Qt-free runtime linkage. On macOS:

```sh
otool -L build/ld-compress-ng
```

On Linux:

```sh
ldd build/ld-compress-ng
```

Expected required runtime libraries for the CPU build are the C++ runtime,
system C library, `libFLAC`, and `libogg`. OpenCL, Vulkan, and Metal appear only
when the corresponding optional paths are enabled and found by CMake.

Useful smoke commands:

```sh
build/ld-compress-ng --help
build/ld-compress-ng --version
build/ld-compress-ng devices
ctest --test-dir build --output-on-failure
```

`ld-compress-ng devices` prints grouped OpenCL, Vulkan, and Metal device indexes.
OpenCL indexes are used by `compress --backend opencl --device INDEX` or
`--opencl-device INDEX`, plus platform-local `platform/device` coordinates. The
OpenCL compression backend writes native FLAC and requires an available OpenCL
device at runtime. Vulkan indexes are backend-local and used by
`compress --backend vulkan --device INDEX`; Vulkan compression requires a
compute-capable device with `shaderInt64`. Metal indexes are backend-local and
used by `compress --backend metal --device INDEX` or `--metal-device INDEX`.

Manual diagnostics are not built by default. Configure with
`-DLDCOMPRESS_BUILD_DIAGNOSTICS=ON` to build tools such as
`compare_opencl_scalar_frames` and `compare_metal_scalar_frames`:

```sh
cmake -S . -B build-diagnostics -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_BUILD_DIAGNOSTICS=ON
cmake --build build-diagnostics --target compare_opencl_scalar_frames
cmake --build build-diagnostics --target compare_metal_scalar_frames
```

## Install Layout

The CMake install target installs:

- `ld-compress-ng` under `${CMAKE_INSTALL_BINDIR}`.
- `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `CHANGELOG.md` under
  `${CMAKE_INSTALL_DOCDIR}`.
- Release-facing Markdown files from `docs/` under
  `${CMAKE_INSTALL_DOCDIR}/docs`. Maintainer-only notes such as
  `docs/remote-sync.md` stay source-tree-only and are not installed.

Use a temporary prefix for local install validation:

```sh
cmake --install build --prefix /tmp/ld-compress-ng-install
/tmp/ld-compress-ng-install/bin/ld-compress-ng --version
```

## Compatibility And Tuning CLI Notes

The README covers the normal compression and decompression commands. These
commands are useful when checking compatibility or doing local size/speed
tuning.

Write native FLAC through the CPU/libFLAC backend instead of the default Ogg
container. This keeps CPU-only compression on libFLAC while changing only the
container:

```sh
build/ld-compress-ng compress --backend cpu --container flac capture.lds capture.flac.ldf
```

The scalar native reference backend and the OpenCL/Vulkan/Metal backends use native
FLAC tuning controls:

```sh
build/ld-compress-ng compress --backend native-fixed \
    --threads 8 \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5 \
    --stats \
    capture.lds
```

The current native tuning defaults are frame size `4608`, maximum LPC order
`12`, LPC coefficient precision `12`, and maximum Rice partition order `5`.
Compression still defaults to one thread unless `--threads` is specified. Use
the scalar native backend as a reference/debug oracle for tuning and writer
coverage; use CPU/libFLAC for normal CPU-only compression. OpenCL, Vulkan, and
Metal use the same native tuning options, and `--threads` parallelizes their CPU
selected-frame writer after GPU analysis. The normal `compress` command uses the
exact native analysis profile; faster order-guess and mean-Rice profiles are
available through `bench` and the sweep helper for tuning work.
Vulkan and Metal exact-cost fixed/Rice and GPU-generated LPC candidates in the
normal compression path. Use `--stats` on native/OpenCL/Vulkan/Metal
compression when investigating backend behavior; accelerated backends also
print coarse timing splits for setup, ingest, analyzer, selected-frame writing,
and accelerator plan/exact-analysis stages. Metal prints generated-LPC plus
exact-analysis/readback timing. Vulkan
additionally prints GPU queue timestamp splits when the selected compute queue
supports timestamp queries, which helps separate transfer/readback cost from
generated-LPC and exact residual/Rice shader work.

## Local Validation Matrix

Use the local matrix helper when you want the normal configure/build/test checks
without remembering the exact command pile:

```sh
python3 tools/check_local_matrix.py
```

By default, the helper configures isolated `build/local-check/default`,
`build/local-check/no-opencl`, and `build/local-check/no-vulkan` trees, builds
them, runs their CTest suites, and runs `ld-compress-ng devices` from each tree.
The default lane keeps opt-in
fixture suites disabled so an existing CMake cache cannot accidentally turn a
quick check into a real-fixture run.

Add local ignored fixture suites explicitly:

```sh
python3 tools/check_local_matrix.py --include-flac-test-files
python3 tools/check_local_matrix.py --include-real-fixtures
python3 tools/check_local_matrix.py --include-real-fixtures --skip-default --skip-no-opencl
python3 tools/check_local_matrix.py --all-local
```

The FLAC testbench lane uses `reference/flac-test-files/` by default and runs
only the `flac-test-files` CTest label. The real-fixture lane uses the current
local `reference/testdata/ld-decode-testdata-ci/...` fixture root by default and
runs `real-fixtures` while excluding the OpenCL-labelled real-fixture test.
`--all-local` follows that same scalar-only real-fixture behavior. Pass
`--include-opencl-real-fixture` when you want the runtime OpenCL real-fixture
check; it implies the real-fixture lane and uses the first available OpenCL
device unless `--opencl-device INDEX` is provided. Pass
`--include-vulkan-real-fixture` for the Vulkan-labelled compatibility test; it
uses the same implicit Vulkan policy as compression: first backend-usable
discrete GPU, then any backend-usable non-CPU device, unless
`--vulkan-device INDEX` is provided. Pass `--include-metal-real-fixture` for the
Metal-labelled compatibility test on macOS; it selects the first available
non-low-power Metal device unless `--metal-device INDEX` is provided. Run GPU
lanes from a context that can see the accelerator runtime and devices;
sandboxed executions may skip or report no available devices even when the
system build can see them. For Vulkan
performance tests on mixed-GPU hosts, use an explicit discrete GPU index from
`ld-compress-ng devices`; the integrated AMD device is suitable for functional
smoke testing but should not be used for NVIDIA performance numbers.
For `compress`, `--device INDEX` is backend-local shorthand for
`--opencl-device INDEX`, `--vulkan-device INDEX`, or `--metal-device INDEX`
after `--backend` selects OpenCL, Vulkan, or Metal. With the default `auto`
policy, the index is checked at each candidate backend in priority order.
Backend-specific device flags are valid only when the resolved backend matches;
use the matching explicit backend to pin one. For benchmark runs that include
multiple accelerators, use the backend-specific device flags; the bare
`--device` form is rejected because it is ambiguous. Optional accelerator
`bench` rows are omitted when no suitable device is visible, while direct
`compress --backend vulkan --device INDEX` fails if the selected device is
unavailable or lacks backend-required features such as `shaderInt64`, and
`compress --backend metal --device INDEX` fails if the selected Metal device is
unavailable.
Use `--dry-run` to inspect the generated commands, and `--strict-optional` to
fail instead of skipping when a requested local fixture directory is missing.

If the reference `ld-decode` dependencies live in a non-default Python
environment, pass that interpreter through to CMake:

```sh
python3 tools/check_local_matrix.py --include-real-fixtures \
    --python-executable /path/to/ld-decode-env/bin/python
```

The core OpenCL smoke tests are labelled `opencl`, so a configured OpenCL build
can also run them directly:

```sh
ctest --test-dir build -L opencl --output-on-failure
```

Metal smoke and analysis tests are labelled `metal`:

```sh
ctest --test-dir build-metal -L metal --output-on-failure
```

In a real-fixture-enabled build, that label also includes the OpenCL
real-fixture loader compatibility test.

## Opt-In Real Fixture Regression

Normal tests use generated or embedded fixtures and do not require real RF
captures. If ignored reference data is available locally, configure a separate
build with the real-fixture suite enabled:

```sh
cmake -S . -B build-real-fixtures -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=ON \
    -DLDCOMPRESS_REAL_FIXTURE_DIR=reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a
cmake --build build-real-fixtures --parallel
ctest --test-dir build-real-fixtures -L real-fixtures --output-on-failure
```

The C++ real-fixture test recursively finds `.lds` files under the configured
directory, verifies matching legacy `.ldf` files when present, and round-trips
each fixture through the CPU/libFLAC path and the threaded native-fixed
reference backend. It prints ratio, timing, and compact native decision-stat
columns as a local regression scoreboard.

When Python and the local `reference/ld-decode/` loader dependencies are
available, the same CMake option also adds skip-safe external compatibility
tests. Those compress the first fixture to native `.flac.ldf` output and verify
the reference `ld-decode` loader can read both `.flac.ldf` and `.flac` suffixes.
OpenCL, Vulkan, and Metal real-fixture loader tests are added too; they skip cleanly
when backend support, a runtime device, or the reference loader dependencies
are unavailable.
Use `ctest --test-dir build-real-fixtures -L real-fixtures -LE "opencl|vulkan|metal"`
when you want the scalar real-fixture suite without accelerator runtime checks.
The fixture tree remains ignored by Git.

For exhaustive accelerator round-trip coverage across every local `.lds`
fixture, use the standalone helper:

```sh
python3 tools/roundtrip_real_fixtures.py \
    --backends opencl,vulkan,metal \
    --opencl-device 1 \
    --vulkan-device 1 \
    --metal-device 0
```

The helper writes ignored CSV/Markdown reports and temporary compressed/decoded
outputs under `build/real-fixture-roundtrips/`. It runs `compress`,
`verify --source`, and `decompress`, then performs an MD5/size compare for each
fixture/backend pair. It defaults to OpenCL plus Vulkan, and can also include
`cpu` or `native-fixed` with `--backends` when a single manual round-trip sweep
is useful.

## Opt-In FLAC Decoder Testbench

The local `reference/flac-test-files/` tree can be used for targeted FLAC
compatibility checks. These are not general audio playback tests; they verify
that `ld-compress-ng` accepts only RF-shaped FLAC input for decompression and
rejects non-40 kHz, non-mono, non-16-bit, missing-STREAMINFO, and raw-frame
testbench cases cleanly.

```sh
cmake -S . -B build-flac-test-files -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=ON \
    -DLDCOMPRESS_FLAC_TEST_FILE_DIR=reference/flac-test-files
cmake --build build-flac-test-files --parallel
ctest --test-dir build-flac-test-files -L flac-test-files --output-on-failure
```

The FLAC testbench tree remains ignored by Git and is used only when the opt-in
CMake option is enabled.

## Real Fixture Tuning Sweep

For native encoder tuning and accelerator comparison work, use the helper script
to run `bench` across all ignored real fixtures and save CSV plus Markdown
summaries under the ignored `build/` tree:

```sh
python3 tools/sweep_real_fixtures.py \
    --binary build/ld-compress-ng \
    --fixtures reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a
```

The default sweep is intentionally focused: frame size `4608`, LPC orders
`10,12`, LPC coefficient precisions `10,12`, Rice partition order `5`, exact
analysis, and `8` native threads. Add `--include-opencl` and optionally
`--opencl-device INDEX` to include OpenCL backend rows in the CSV/Markdown
output when an OpenCL device is available. Add `--include-vulkan` and optionally
`--vulkan-device INDEX` to include Vulkan backend rows; on mixed-GPU hosts, pass
the discrete GPU index so the run does not land on an integrated GPU. Add
`--include-metal` and optionally `--metal-device INDEX` on macOS to include
Metal rows. For accelerator speed sweeps, `--reuse-opencl-session`,
`--reuse-vulkan-session`, and `--reuse-metal-session` reuse one analysis session
across rows and avoid charging device setup to every benchmark case. Expand the
grid explicitly when doing a broader local tuning pass:

```sh
python3 tools/sweep_real_fixtures.py \
    --threads 1,8 \
    --frame-samples 2304,4608 \
    --lpc-order 8,10,12 \
    --lpc-precision 8,10,12,14 \
    --rice-partition-order 4,5,6 \
    --analysis-profile exact,order-guess-mean-estimate-rice
```

Use `--dry-run` to print the generated `bench` commands and `--limit N` for a
quick subset check. The helper depends only on Python 3 stdlib, the built
`ld-compress-ng` binary, and local ignored fixture files.

After adding Tukey-windowed LPC analysis candidates plus high-order
Welch-windowed candidates and retuning over the six current real fixtures, frame
size `4608`, LPC order `12`, LPC coefficient precision `12`, and Rice partition
order `5` remain the exact-analysis native reference settings for normal
compression. In the pinned exact sweep, raw LDS inputs total `149,954,560`
bytes, CPU/libFLAC outputs total `80,086,984` bytes, scalar native-fixed outputs
total `79,867,690` bytes, and OpenCL outputs total `79,952,087` bytes.

The current Linux OpenCL/Vulkan speed-focused sweep reference is
`build/real-fixture-sweeps/real-fixture-sweep-20260708-145656.{csv,md}`. It used
`threads=8`, `frame=4608`, `lpc=12`, `prec=12`, Rice orders `5,6`,
`analysis-profile=order-guess-mean-estimate-rice`, and OpenCL/Vulkan session
reuse. Across the six local real fixtures, the best speed-profile rows were:

| Backend | Rice order | Output bytes | Elapsed time |
| --- | ---: | ---: | ---: |
| CPU/libFLAC | - | `80,086,984` | `2.440s` |
| Native-fixed | `6` | `79,926,901` | `1.689s` |
| OpenCL | `6` | `79,946,987` | `0.814s` |
| Vulkan | `6` | `79,946,934` | `0.813s` |

The normal exact-analysis OpenCL/Vulkan `compress` roundtrip helper produced
smaller aggregate output while verifying and decompressing back to the original
LDS bytes across all six fixtures:

| Backend | Input bytes | Output bytes | Compress time |
| --- | ---: | ---: | ---: |
| OpenCL | `149,954,560` | `79,892,119` | `4.452s` |
| Vulkan | `149,954,560` | `79,892,217` | `3.938s` |

Metal rows use the same helper and report comparable measurement data when
fixtures are present:

```sh
python3 tools/roundtrip_real_fixtures.py --backends metal --metal-device INDEX
python3 tools/sweep_real_fixtures.py --include-metal --reuse-metal-session --metal-device INDEX
```

The Apple M5 Pro Metal exact size-parity checkpoint used Metal device `0`.
This exact-output compatibility artifact is retained as historical context; it
is not the current Metal speed-profile baseline. The exact six-fixture
roundtrip artifact is
`build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-185327/`:

| Backend | Input bytes | Output bytes | Compress time |
| --- | ---: | ---: | ---: |
| Native-fixed | `149,954,560` | `79,867,690` | `9.931s` |
| Metal | `149,954,560` | `79,892,801` | `166.953s` |

The matching exact OpenCL+Metal sweep artifact is
`build/real-fixture-sweeps/real-fixture-sweep-20260708-185945.{csv,md}`:

| Backend | Output bytes | Elapsed time |
| --- | ---: | ---: |
| Native-fixed | `79,867,690` | `10.188s` |
| OpenCL | `79,892,332` | `3.887s` |
| Metal | `79,892,801` | `174.326s` |

The current Apple M5 Pro Metal speed-profile checkpoint is
`build/real-fixture-sweeps/real-fixture-sweep-20260709-103103.{csv,md}` and used
Metal device `0`, session reuse, `threads=8`, and
`--analysis-profile order-guess-mean-estimate-rice --rice-partition-order 5,6`.
It is in the same output-size class as the Linux OpenCL/Vulkan speed rows and is
currently faster in the documented six-fixture rice6 comparison:

| Backend | Rice order | Output bytes | Elapsed time |
| --- | ---: | ---: | ---: |
| Metal | `6` | `79,946,831` | `0.626s` |
| OpenCL Linux reference | `6` | `79,946,987` | `0.814s` |
| Vulkan Linux reference | `6` | `79,946,934` | `0.813s` |

Scalar native-fixed is useful as a size and decision oracle, but CPU/libFLAC
remains the recommended CPU-only encoder.

## Legacy Fixture Regeneration

Committed generated-fixture tests do not require `ffmpeg` or `ld-lds-converter`;
the legacy Ogg FLAC fixture is embedded as bytes. To regenerate that fixture
intentionally, use the reference converter and ffmpeg outside the normal build
path:

```sh
build-ld-lds-converter/ld-lds-converter -q -u -i reference.lds -o reference.s16
ffmpeg -hide_banner -loglevel error -y -f s16le -ar 40k -ac 1 \
    -i reference.s16 -acodec flac -compression_level 11 -f ogg reference.ldf
```

The reference converter may itself link Qt; that dependency stays isolated to
the ignored reference/helper build and is not part of `ld-compress-ng`.
