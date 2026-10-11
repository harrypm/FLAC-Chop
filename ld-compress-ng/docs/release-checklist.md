# Release Checklist

Use this checklist for source releases. Binary packages and GitHub Actions are
intentionally out of scope unless a future release explicitly adds them.

## Required Local Gate

Start from a clean working tree:

```sh
git status --short --branch
```

Build and run the normal non-real-fixture suite:

```sh
cmake --build build
ctest --test-dir build -LE real-fixtures --output-on-failure
```

Check version/help output:

```sh
build/ld-compress-ng --version
build/ld-compress-ng --help
```

Run a CPU-only build/test lane:

```sh
cmake -S . -B build-release-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF \
    -DLDCOMPRESS_ENABLE_METAL=OFF
cmake --build build-release-cpu-only --parallel
ctest --test-dir build-release-cpu-only -LE real-fixtures --output-on-failure
```

On macOS, run the Metal-enabled and no-Metal lanes:

```sh
cmake -S . -B build-metal -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_METAL=ON
cmake --build build-metal --parallel
build-metal/ld-compress-ng devices
ctest --test-dir build-metal -L metal --output-on-failure

cmake -S . -B build-no-metal -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_METAL=OFF
cmake --build build-no-metal --parallel
ctest --test-dir build-no-metal --output-on-failure
```

Smoke-test installation:

```sh
cmake --install build --prefix /tmp/ld-compress-ng-install
/tmp/ld-compress-ng-install/bin/ld-compress-ng --version
find /tmp/ld-compress-ng-install -maxdepth 6 -type f | sort
otool -L /tmp/ld-compress-ng-install/bin/ld-compress-ng
```

On macOS, `otool -L` should show Metal/Foundation only for builds configured
with `LDCOMPRESS_ENABLE_METAL=ON`.

Confirm the installed files include:

- `bin/ld-compress-ng`
- `share/doc/ld-compress-ng/README.md`
- `share/doc/ld-compress-ng/LICENSE`
- `share/doc/ld-compress-ng/THIRD_PARTY_NOTICES.md`
- `share/doc/ld-compress-ng/CHANGELOG.md`
- `share/doc/ld-compress-ng/docs/PROJECT_PLAN.md`
- `share/doc/ld-compress-ng/docs/build-and-testing.md`
- `share/doc/ld-compress-ng/docs/release-checklist.md`

## Optional Full Local Gate

When local ignored fixtures and GPU access are available, run the full matrix
from a context that can see the OpenCL, Vulkan, and Metal runtimes. Use explicit
device indexes from `ld-compress-ng devices` on mixed-GPU hosts:

```sh
python3 tools/check_local_matrix.py \
    --build-root build/local-check-release \
    --all-local \
    --include-opencl-real-fixture \
    --opencl-device 1 \
    --include-vulkan-real-fixture \
    --vulkan-device 1 \
    --include-metal-real-fixture \
    --metal-device 0 \
    --python-executable /path/to/ld-decode-env/bin/python \
    --jobs 2 \
    --strict-optional
```

Adjust the device indexes and PyAV-capable Python interpreter path for the
local validation host.

Then run the full accelerator round-trip check across every local real fixture.
This compresses, verifies against the source `.lds`, decompresses, and compares
the decoded bytes for each requested backend:

```sh
python3 tools/roundtrip_real_fixtures.py \
    --backends opencl,vulkan,metal \
    --opencl-device 1 \
    --vulkan-device 1 \
    --metal-device 0
```

## Tag And Publish

After the required gate passes, tag and push the chosen release version:

```sh
git tag -a vX.Y.Z -m "ld-compress-ng X.Y.Z"
git push origin main
git push origin vX.Y.Z
```

Create the GitHub source release manually from the release tag, using
`CHANGELOG.md` as the release-note source.
