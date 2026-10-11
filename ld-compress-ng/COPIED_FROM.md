# Copy-in provenance

This is a **direct copy-in** of [ld-compress-ng](https://github.com/JoeyLemur/ld-compress-ng)
(not vendored, not a submodule, not cloned at build time) so FLAC-Chop builds
the whole accelerated-conversion stack from one repo — including CI.

- **Upstream base commit**: `7a60555` ("Harden Vulkan teardown and GPU test
  scheduling", origin/main of JoeyLemur/ld-compress-ng at copy time,
  2026-10-11).
- **Local deltas the snapshot carries** (uncommitted upstream at copy time —
  the Windows/MSYS2 port, needed for the MinGW64 CI + local builds):
  - `src/main.cpp`: `BlockTerminationSignals` no-op on Windows (no
    `sigprocmask`), ANSI `signal()` + `raise()` termination cleanup
    (SIGINT/SIGTERM/SIGBREAK), and `rename_no_replace` via `MoveFileExW`
    WITHOUT `MOVEFILE_REPLACE_EXISTING` (Windows' native atomic no-replace
    rename; `ERROR_ALREADY_EXISTS` → `EEXIST`); the POSIX `link()` fallback
    is gated non-Windows.
  - `CMakeLists.txt`: `test_cli` (fork/wait supervision) gated to
    non-Windows.
  - `src/main.cpp`: explicit `#include <algorithm>` for `std::sort`
    (LLVM-mingw/CLANGARM64 does not supply it transitively the way GCC
    toolchains do — the Windows arm64 CI build failed without it).
- **Not copied**: upstream `.git`, build dirs, `AGENTS.md`.
- Licenses preserved verbatim: `LICENSE` (GPL), `LICENSES/`,
  `THIRD_PARTY_NOTICES.md` stay in place in this tree.

Re-sync policy: refresh by copying the desired upstream tree over this
directory (same exclusions) and update the base commit + delta list here.
FLAC-Chop's use of the code is the built CLI spawned by the Rust core's
accelerated whole-file conversion path (`core/src/chop.rs` `run_ldc_compress`);
see `docs/DEV_NOTE_ldc_acceleration.md`.
