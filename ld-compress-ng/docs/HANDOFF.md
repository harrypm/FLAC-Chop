# Handoff Notes

Last updated: 2026-08-21, for 1.3.0 release preparation.

This file is for maintainer/agent continuity. It is intentionally not installed
by CMake; release-facing installed docs are listed explicitly in
`CMakeLists.txt`.

## Current Repository State

- Active branch: `main`.
- Project version is `1.3.0` in `CMakeLists.txt`; `ld-compress-ng --version`
  prints `ld-compress-ng 1.3.0`.
- 1.3.0 adds `compress --progress`, compressed-size percentages in its final
  summary, and a shared default `.ldf` output suffix for every backend.
  Accelerated outputs remain native FLAC while the CPU default remains Ogg
  FLAC. It also includes speed-profile optimization passes for Metal, Vulkan,
  and OpenCL, plus the portable MD5 transform improvement.
- `compress` and `decompress` write inside a private `mkdtemp` same-directory
  staging directory. `--overwrite` atomically renames its payload to the
  requested destination; no-overwrite mode uses the platform atomic
  no-replace rename so a destination created during the run cannot be replaced,
  without requiring hard-link support. Older platforms or filesystems without
  no-replace rename support retain a hard-link fallback when available.
  `SIGINT`, `SIGTERM`, and `SIGHUP` unlink the active payload and staging
  directory before preserving normal signal termination (`SIGKILL` is
  uncatchable). The CLI regression covers successful and failed
  compression/decompression cleanup, no-overwrite publication races, and
  interrupt cleanup. It repeats compression failure-preservation checks for
  each available accelerator. The Vulkan branch will run automatically on a
  Vulkan-capable Linux test host.
- FLAC decompression now buffers 8,192 LDS groups (40 KiB packed output) per
  write and records `FLAC__stream_decoder_finish()` PCM-MD5 mismatches without
  discarding otherwise valid output, replacing the former second MD5 pass over
  individual samples. Original ld-compress Ogg and FlaLDF captures can have a
  stale nonzero field; the Indiana Jones capture does, so `verify --source`
  remains the end-to-end check.
- `decompress --progress` reports a throttled stderr line from decoder frame
  callbacks. It emits an immediate STREAMINFO-based `0%` state, shows elapsed
  time, uses a percentage when the FLAC total is known, and otherwise reports
  decoded samples; the option has a CLI regression that captures stderr.
- Decoding rejects samples outside the exact signed-16-bit LDS 10-bit grid
  (`sample % 64 == 0`) before they can be silently quantized by the packer. A
  native-FLAC regression supplies an MD5-valid off-grid sample and confirms no
  LDS bytes are emitted.
- `verify` uses a sequential libFLAC read callback that feeds the compressed
  input MD5 during decoding. It requires the counted bytes to equal the input
  file size, so large captures avoid the former separate `md5_file()` pass.
  Native and Ogg tests compare this digest and byte count to `md5_file()`.
- Potential scalar-native tuning point: profile `update_md5_s16le()` before
  changing it. It still updates the project MD5 once per four-sample LDS group;
  if that matters on Apple, batch the existing signed-16-bit little-endian PCM
  bytes, flush the final tail before STREAMINFO finalization, and keep updates
  on the ordered ingest thread rather than frame workers. This is not a current
  correctness or release blocker.
- The shared OpenCL/Vulkan/Metal selected-frame writer now gives each queued
  job shared ownership of its analyzed sample batch. If one worker fails, a
  sibling can finish unwinding without reading freed batch storage. The
  `accelerated_native_backend` regression forces this two-worker error path and
  passes under AppleClang AddressSanitizer/UndefinedBehaviorSanitizer.
- 1.2.0 adds the macOS-only `metal` native FLAC accelerator backend using Apple
  Command Line Tools, `Metal.framework`, `Foundation.framework`, and runtime
  Metal source compilation. There is no Xcode project and no required offline
  `.metallib`.
- Metal exact compression now runs generated-LPC candidate population on the
  GPU before exact costing and selected-writer handoff. The previous
  `119.8 MB` six-fixture Metal baseline is obsolete; the current exact
  six-fixture output is within `0.032%` of native-fixed on Apple M5 Pro device
  `0`.
- The current Metal speed-profile checkpoint on Apple M5 Pro device `0` is
  `79,946,831` bytes in `0.626s` across the six local real fixtures, using
  `threads=8`, `frame=4608`, `lpc=12`, `prec=12`, Rice order `6`, and
  `analysis-profile=order-guess-mean-estimate-rice`.
- The managed Codex sandbox on macOS may hide `MTLCreateSystemDefaultDevice()`;
  sandboxed Metal tests should skip cleanly. Hardware validation should be run
  from a GPU-visible shell with:

  ```sh
  cmake -S . -B build-metal -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLDCOMPRESS_ENABLE_METAL=ON
  cmake --build build-metal --parallel
  build-metal/ld-compress-ng devices
  ctest --test-dir build-metal -L metal --output-on-failure
  ctest --test-dir build-metal --output-on-failure
  ```

- Also keep a no-Metal lane current:

  ```sh
  cmake -S . -B build-no-metal -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLDCOMPRESS_ENABLE_METAL=OFF
  cmake --build build-no-metal --parallel
  ctest --test-dir build-no-metal --output-on-failure
  ```

## Metal Subdivide-Tukey3 Regression Follow-Up

The Metal generated-LPC path previously defined the Subdivide-Tukey3 profile as
six windows with a `0.375` taper. The shared task planner and the scalar,
OpenCL, and Vulkan implementations define the profile as nine windows: one
full Tukey window, two two-way partial windows, three three-way partial
windows, and three punch-out windows, all with a `0.5 / 3.0` taper. As a
result, normal Metal profile runs silently used rectangular windows; a
hand-built six-window plan could also overrun its window buffer while filling
the final three punch-out windows.

The fix in `src/metal_analysis.mm` now uses the nine-window/`0.5 / 3.0`
definition, rejects an incompatible task plan instead of falling back, and
checks that the generator filled exactly the allocated windows. Validate it on
a GPU-visible Mac shell after building `build-metal` as above:

```sh
ctest --test-dir build-metal -L metal --output-on-failure

build-metal/ld-compress-ng bench \
    --threads 1 \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 6 \
    --analysis-profile subdivide-tukey3-mean-rice,subdivide-tukey3-mean-estimate-rice \
    --include-metal \
    --metal-device 0 \
    reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a/ntsc/issue176.lds
```

Use the index reported by `build-metal/ld-compress-ng devices` in place of
`0`. The Metal rows must complete and decode successfully; compare their
output size and selected-subframe statistics with the CPU/native-fixed rows in
the same benchmark. Repeat against the six-fixture real-fixture lane before
recording a new Metal performance baseline.

## Large-Capture STREAMINFO Follow-Up

The shared native FLAC writer now records an unknown (`0`) STREAMINFO
total-sample count when the actual count exceeds the 36-bit FLAC field. This
fix covers native-verbatim, native-fixed, OpenCL, Vulkan, and Metal; the
CPU/libFLAC backend already uses the same convention. Exactly 80 GiB of LDS
input is the first LDS-aligned count over the field limit. The decoder accepts
the unknown count and still validates the complete PCM MD5.

Boundary regression coverage and the macOS Metal/no-Metal suites are the local
validation baseline. A real capture over 80 GiB should still be exercised on a
GPU-visible Linux host through CPU, native-fixed, OpenCL, and Vulkan. The exact
commands, scratch-space warning, expected STREAMINFO value, and end-to-end
verification steps are in `docs/build-and-testing.md` under "Linux
Large-Capture STREAMINFO Validation".

## Accelerated Writer Lifetime Follow-Up

The macOS Metal and no-Metal suites pass the new device-independent regression.
Linux should replay it under AddressSanitizer/UndefinedBehaviorSanitizer, then
run the GPU-visible OpenCL and Vulkan lanes so the shared host path remains
covered with both Linux accelerators. Exact configure and test commands are in
`docs/build-and-testing.md` under "Linux Accelerated Writer Lifetime
Validation".

## 1.2 Final Metal Performance Checkpoint

This is the current 1.2.0 Metal performance baseline. It supersedes the older
M5/M6 Metal sections below, which are retained only as historical debugging
context.

Final speed-profile sweep artifact:
`build/real-fixture-sweeps/real-fixture-sweep-20260709-103103.{csv,md}`.

Command shape:

```sh
python3 tools/sweep_real_fixtures.py \
    --binary build-metal/ld-compress-ng \
    --threads 8 \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5,6 \
    --analysis-profile order-guess-mean-estimate-rice \
    --include-metal \
    --reuse-metal-session \
    --metal-device 0
```

| Backend | Rice order | Output bytes | Aggregate elapsed |
| --- | ---: | ---: | ---: |
| Metal | `6` | `79,946,831` | `0.626s` |
| Linux OpenCL reference | `6` | `79,946,987` | `0.814s` |
| Linux Vulkan reference | `6` | `79,946,934` | `0.813s` |

The final Metal speed-profile path is byte-stable in the same output-size class
and is currently faster than the documented Linux OpenCL/Vulkan rice6 rows.
Accepted performance changes since the first Metal size-parity checkpoint
included Apple CommonCrypto MD5, pre-shifted autocorrelation input, and
pre-shifted exact-analysis input. The remaining obvious buckets are host scan,
selected-frame writing, fixed-order guess, and generated-path overhead; further
wins are architectural and are not required for 1.2.0.

## Historical 1.2 Metal M6 Size-Parity And Speed Checkpoint

This checkpoint is superseded by the final performance checkpoint above.

GPU-visible validation on Apple M5 Pro device index `0` passed after fixing the
Metal LPC task coefficient ordering and moving generated LPC onto the Metal
shader path:

- `build-metal/ld-compress-ng devices` saw Metal `[0] Apple M5 Pro` and OpenCL
  `[0] Apple M5 Pro`.
- `build-metal/test_metal_smoke --device 0` passed.
- `build-metal/test_metal_analysis --device 0` passed.
- `cmake --build build-metal --parallel` passed.
- `ctest --test-dir build-metal -L metal --output-on-failure` passed `3/3`.
- `ctest --test-dir build-metal --output-on-failure` passed `19/19`, with the
  expected optional Python/reference-loader skips.
- `compare_metal_scalar_frames` built under `build-metal-diagnostics` and on
  the first four `ggv-ntsc-mb-v2800` frames reported exact selected-writer
  recost parity for Metal-selected tasks
  (`metal_recost_delta_bits=0`, `metal_recost_mismatches=0`).

Full six-fixture `native-fixed,metal` roundtrip passed:

- Artifact:
  `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-185327/`.
- Each row ran `compress`, `verify --source`, `decompress`, and decoded
  size/MD5 comparison.

| Backend | Output bytes | Ratio | Aggregate compress time |
| --- | ---: | ---: | ---: |
| Native-fixed | `79,867,690` | `0.532613` | `9.931s` |
| Metal | `79,892,801` | `0.532780` | `166.953s` |

Exact OpenCL+Metal sweep artifact:
`build/real-fixture-sweeps/real-fixture-sweep-20260708-185945.{csv,md}`.

| Backend | Output bytes | Ratio | Aggregate elapsed |
| --- | ---: | ---: | ---: |
| Native-fixed | `79,867,690` | `0.532613` | `10.188s` |
| OpenCL | `79,892,332` | `0.532778` | `3.887s` |
| Metal | `79,892,801` | `0.532780` | `174.326s` |

At this checkpoint, Metal was `+25,111` bytes (`+0.0314%`) versus native-fixed in the
six-fixture exact roundtrip and `+469` bytes versus OpenCL in the exact sweep.
Exact Metal remained much slower than OpenCL, but it was no longer the old
compression-size outlier.

Speed-profile sweep artifact:
`build/real-fixture-sweeps/real-fixture-sweep-20260708-191154.{csv,md}`.

Command shape:

```sh
python3 tools/sweep_real_fixtures.py \
    --binary build-metal/ld-compress-ng \
    --include-opencl \
    --include-metal \
    --reuse-opencl-session \
    --reuse-metal-session \
    --opencl-device 0 \
    --metal-device 0 \
    --analysis-profile order-guess-mean-estimate-rice \
    --rice-partition-order 5,6
```

| Backend | Rice order | Output bytes | Aggregate elapsed |
| --- | ---: | ---: | ---: |
| Native-fixed | `6` | `79,926,901` | `1.551s` |
| OpenCL | `6` | `79,946,777` | `1.602s` |
| Metal | `6` | `79,946,831` | `4.666s` |

This speed-profile row is obsolete; use the final Metal performance checkpoint
above for current performance expectations.

## Historical 1.2 Metal M5 Initial Correctness And Baseline Checkpoint

This was the first functional Metal checkpoint before the coefficient-ordering
and GPU generated-LPC fixes. Keep it only as historical evidence for the size
bug that the later checkpoints replaced.

GPU-visible validation on Apple M5 Pro device index `0` passed:

- `build-metal/ld-compress-ng devices` saw Metal `[0] Apple M5 Pro` and OpenCL
  `[0] Apple M5 Pro`; Vulkan was not built.
- `ctest --test-dir build-metal -L metal --output-on-failure` passed `3/3`.
- Real-fixture build:

  ```sh
  cmake -S . -B build-metal-real-fixtures \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLDCOMPRESS_ENABLE_METAL=ON \
      -DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=ON \
      -DLDCOMPRESS_REAL_FIXTURE_DIR=$PWD/reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a \
      -DLDCOMPRESS_METAL_TEST_DEVICE=0
  cmake --build build-metal-real-fixtures --parallel
  ```

- `ctest --test-dir build-metal-real-fixtures -L real-fixtures -LE
  'opencl|vulkan' --output-on-failure` passed with `3/3`; the optional
  `ld-decode` loader compatibility tests skipped due missing optional Python
  reference-loader dependencies.
- `ctest --test-dir build-metal-real-fixtures -L metal --output-on-failure`
  passed `4/4`, with the optional Metal loader-compat test skipped.
- `ctest --test-dir build-metal-real-fixtures --output-on-failure` passed
  `24/24`, with six optional external-decode/reference-loader skips.

Full six-fixture `native-fixed,metal` roundtrip passed:

- Artifact:
  `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-172240/`.
- Each row ran `compress`, `verify --source`, `decompress`, and decoded
  size/MD5 comparison.

| Backend | Output bytes | Ratio | Aggregate compress time |
| --- | ---: | ---: | ---: |
| Native-fixed | `79,867,690` | `0.532613` | `9.453s` |
| Metal | `119,832,125` | `0.799123` | `341.736s` |

Baseline sweep artifacts:

- Metal-only comparison:
  `build/metal-baseline-sweeps/real-fixture-sweep-20260708-172930.{csv,md}`.
- OpenCL+Metal comparison:
  `build/metal-opencl-baseline-sweeps/real-fixture-sweep-20260708-175114.{csv,md}`.

Best-row aggregate sizes from the OpenCL+Metal comparison sweep:

| Backend | Output bytes | Ratio | Aggregate elapsed |
| --- | ---: | ---: | ---: |
| CPU/libFLAC | `80,086,982` | `0.534075` | `2.666s` |
| Native-fixed | `79,843,831` | `0.532454` | `9.532s` |
| OpenCL | `79,876,133` | `0.532669` | `3.947s` |
| Metal | `119,775,018` | `0.798742` | `339.675s` |

Metal best rows were about `+49.56%` larger than CPU/libFLAC, `+50.01%`
larger than native-fixed, and `+49.95%` larger than OpenCL. OpenCL was
`+0.0405%` larger than native-fixed in the same sweep. Metal elapsed time was
dominated by `metal_lpc_s` plus `metal_exact_s`; `metal_choose_s` and
`metal_read_s` were effectively negligible. This baseline is now obsolete and
should not be used as the current Metal size/performance expectation.

## 1.1 Release Context

- `origin/main` was pushed to the 1.1.1 release-prep commit
  `a390f5a` (`Polish 1.1.1 release notes`) during the first publish attempt.
- Local `main` has one more release-validation/docs update on top of that push.
- Local branch `codex/vulkan-1.1` still exists as release-branch history
  context. It can be kept or deleted later.
- Annotated tag `v1.1.0` has been pushed to origin.
- Annotated tag `v1.1.1` exists locally and has not been pushed to origin.
  Make sure it points at the final release commit before pushing it.
- GitHub release was created as source-only, with no binary assets:
  `https://github.com/JoeyLemur/ld-compress-ng/releases/tag/v1.1.0`
## Post-1.1 Mainline Work

- Added benchmark/sweep-only native analysis profiles, including order-guess
  and mean-Rice speed profiles. The normal `compress` command still uses exact
  native analysis.
- Improved OpenCL throughput with selected Rice-parameter handoff, reusable
  generated-analysis sessions, larger accepted compression batches, and
  cooperative generated autocorrelation.
- Pipelined the shared accelerated host flow so the host can ingest/MD5 the next
  batch while the GPU analyzes one batch and then write the previous analyzed
  frames in order.
- Retained Vulkan/OpenCL compatibility through the shared native writer while
  improving accelerator sweep throughput.

Current accepted speed-focused reference:

- Sweep artifact:
  `build/real-fixture-sweeps/real-fixture-sweep-20260708-145656.{csv,md}`.
- Sweep shape: `threads=8`, `frame=4608`, `lpc=12`, `prec=12`, Rice orders
  `5,6`, `analysis-profile=order-guess-mean-estimate-rice`, OpenCL/Vulkan
  session reuse enabled.
- Six-fixture aggregate speed-profile rows:

  | Backend | Rice order | Output bytes | Elapsed time |
  | --- | ---: | ---: | ---: |
  | CPU/libFLAC | - | `80,086,984` | `2.440s` |
  | Native-fixed | `6` | `79,926,901` | `1.689s` |
  | OpenCL | `6` | `79,946,987` | `0.814s` |
  | Vulkan | `6` | `79,946,934` | `0.813s` |

- Validation for the wrap-up checkpoint: `cmake --build build` passed, full
  GPU-visible `ctest --test-dir build --output-on-failure` passed with `21/21`
  tests, focused OpenCL/Vulkan/real-fixture CTest rerun passed with `9/9`
  tests, the all-six-fixture OpenCL/Vulkan roundtrip helper wrote
  `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-145623/`,
  and the final focused sweep above completed.

## What 1.1 Shipped

- Linux-first Vulkan native FLAC acceleration backend.
- OpenCL throughput improvements: larger batches, persistent analysis state,
  best-task readback, parallel exact/Rice costing, and cooperative generated
  autocorrelation.
- Shared accelerator host flow used by OpenCL and Vulkan.
- Real-fixture sweep support for OpenCL and Vulkan.
- Full OpenCL/Vulkan real-fixture roundtrip helper:
  `tools/roundtrip_real_fixtures.py`.
- Release/docs cleanup, including LGPL/CUETools notice coverage for the Vulkan
  shader path.

## 1.1 Release Validation That Passed

Required release gate:

```sh
cmake --build build
ctest --test-dir build -LE real-fixtures --output-on-failure
build/ld-compress-ng --version
build/ld-compress-ng --help
cmake -S . -B build-release-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF
cmake --build build-release-cpu-only --parallel
ctest --test-dir build-release-cpu-only -LE real-fixtures --output-on-failure
cmake --install build --prefix /tmp/ld-compress-ng-install-1.1-final
/tmp/ld-compress-ng-install-1.1-final/bin/ld-compress-ng --version
```

Observed results:

- Main build passed with OpenCL and Vulkan enabled.
- Non-real-fixture CTest passed: `17/17`.
- CPU-only lane passed: `16/16`, with PyAV/reference-loader tests skipped in
  that CPU-only build because the PyAV interpreter was not configured there.
- Install smoke passed and installed only release-facing docs:
  `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`,
  `docs/PROJECT_PLAN.md`, `docs/build-and-testing.md`, and
  `docs/release-checklist.md`.

Optional full local gate:

```sh
python3 tools/check_local_matrix.py \
    --build-root build/local-check-release \
    --all-local \
    --include-opencl-real-fixture \
    --opencl-device 1 \
    --include-vulkan-real-fixture \
    --vulkan-device 1 \
    --python-executable /home/epowell/.pyenv/versions/3.13.13/envs/ld/bin/python \
    --jobs 2 \
    --strict-optional
```

Observed result: all selected validation steps passed, including default,
no-OpenCL, no-Vulkan, FLAC test-file, scalar real-fixture, OpenCL real-fixture,
and Vulkan real-fixture lanes.

Exhaustive accelerator roundtrip:

```sh
python3 tools/roundtrip_real_fixtures.py \
    --backends opencl,vulkan \
    --opencl-device 1 \
    --vulkan-device 1
```

Final report:

- `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-145623/`
- Six local `.lds` fixtures, two backends, twelve total rows.
- Each row ran `compress`, `verify --source`, `decompress`, and decoded
  size/MD5 comparison against the source `.lds`.
- Aggregate input: `149,954,560` bytes.

| Backend | Output bytes | Aggregate compress time |
| --- | ---: | ---: |
| OpenCL | `79,892,119` | `4.452s` |
| Vulkan | `79,892,217` | `3.938s` |

## Device Assumptions

Use a GPU-visible shell for accelerator validation. Sandboxed contexts can hide
the NVIDIA devices or expose only CPU Vulkan/llvmpipe.

Device indexes used for final validation on `smaug`:

- OpenCL device `1`: NVIDIA GeForce RTX 5070 Ti.
- Vulkan device `1`: NVIDIA GeForce RTX 5070 Ti.
- Vulkan device `0`: integrated AMD GPU. It is usable for functional smoke
  testing but should not be used for performance conclusions.
- Vulkan device `2`: NVIDIA GeForce RTX 4070 SUPER.
- Vulkan device `3`: llvmpipe CPU Vulkan implementation.

AMD Vulkan support is intended through standard Vulkan compute design, but the
1.1 release did not validate discrete AMD hardware.

## Current Defaults And Useful Commands

Default compression remains CPU/libFLAC Ogg FLAC `.ldf` for compatibility:

```sh
build/ld-compress-ng compress capture.lds
```

Native FLAC backends write `.flac.ldf`. Use `opencl`/`vulkan` for accelerated
compression; keep `native-fixed` as a scalar reference/debug backend:

```sh
build/ld-compress-ng compress --backend native-fixed --threads 8 capture.lds
build/ld-compress-ng compress --backend opencl --device 1 --threads 8 capture.lds
build/ld-compress-ng compress --backend vulkan --device 1 --threads 8 capture.lds
```

Native tuning defaults:

- `--frame-samples 4608`
- `--lpc-order 12`
- `--lpc-precision 12`
- `--rice-partition-order 5`
- `--threads 1` by default

For routine OpenCL/Vulkan benchmark comparisons, use `--threads 8`. OpenCL and
Vulkan still default to one thread for `compress`, but additional threads
parallelize the CPU selected-frame writer after GPU analysis. Use CPU/libFLAC
for normal CPU-only compression; use `native-fixed` when scalar reference
behavior is needed.

Speed-profile sweep command shape:

```sh
python3 tools/sweep_real_fixtures.py \
    --binary build/ld-compress-ng \
    --fixtures reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a \
    --threads 8 \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5,6 \
    --analysis-profile order-guess-mean-estimate-rice \
    --include-opencl \
    --reuse-opencl-session \
    --opencl-device 1 \
    --include-vulkan \
    --reuse-vulkan-session \
    --vulkan-device 1
```

## Important Decisions To Preserve

- Do not chase tiny OpenCL/Vulkan compression-ratio or runtime deltas unless a
  future task explicitly chooses a performance/tuning pass.
- Treat `native-fixed` and `native-verbatim` as reference/debug backends, not
  production CPU compression recommendations. Keep them available for scalar
  oracle, native writer, and fixture coverage.
- OpenCL/Vulkan outputs are compatible and roundtrip correctly on the validated
  Linux/NVIDIA host.
- The CPU/native writer remains the compatibility authority; accelerators choose
  analysis candidates and selected subframes.
- Vulkan and OpenCL may differ slightly in LPC coefficient/candidate choices due
  to floating-point summation/precision. Exact byte-for-byte candidate parity is
  not a release requirement.
- `docs/remote-sync.md` is maintainer-only and source-tree-only. Do not install
  it unless intentionally changing release doc policy.
- Preserve LGPL/CUETools notices in `src/opencl_analysis.cpp` and
  `shaders/vulkan_fixed_constant.comp` when modifying accelerator analysis code.
- Reference trees under `reference/`, `FlaLDF/`, and `ld-decode-tools/` should
  remain read-only unless a task explicitly says otherwise.

## Good Places To Resume Future Work

Start future work from the current local `main`. `origin/main` may still be at
the published `v1.1.0` release until the post-release mainline commits are
pushed, so check `git status --short --branch` and `git log --oneline -5`
before syncing.

Potential next branches:

- `codex/1.1.x-maintenance` for small bug fixes after the release.
- `codex/1.2-planning` for feature planning.
- `codex/amd-vulkan-validation` if discrete AMD hardware becomes available.

Reasonable next tasks:

- Validate Vulkan on real AMD discrete hardware and document results.
- Decide whether to keep, delete, or archive the merged `codex/vulkan-1.1`
  branch.
- If desired later, add binary packaging or GitHub Actions as a separate scoped
  plan; they were intentionally out of scope for 1.0/1.1.
- Revisit accelerator performance only at the architecture level. The next
  promising direction is reusable per-frame ingest facts or writer-ready
  residual/Rice state, not shader micro-tuning.
- For Metal follow-up, prioritize exact-speed work and overhead reduction; size
  parity against native-fixed/OpenCL is already within the current acceptance
  target on the six local fixtures.
