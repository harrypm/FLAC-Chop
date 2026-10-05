# HARD DEV NOTE — a release build must never report "dev-<sha>"

Status: HARD RULE for every release this repo ships. Shipped on v1.0.8 (2026-10-03): the release exe's window title, `--version` output, and update check all reported `dev-e744de2` while the artifact FILE names said `v1.0.8`. The in-app version and the artifact name must always agree.

## The bug (root cause, verified from the v1.0.8 CI logs + the shipped binary)

1. `actions/checkout@v5` defaults to a SHALLOW, TAGLESS checkout — `git describe --tags --match 'v*'` FAILS in CI, so the CMake fallback (`dev-<short-sha>`) was baked into every platform's binary. The CI "Resolve build version" step knew the right version (from `GITHUB_REF`) but only used it to name the output FILES, never passing it into the build.
2. The shipped `windows_FLAC-Chop_v1.0.8_x86_64.exe` contained the literal string `dev-e744de2` and no `v1.0.8` (binary string search); its GUI title read "FLAC-Chop dev-e744de2 — RF capture cutter"; every packaging job's cmake configure printed `-- FLAC-Chop version: dev-e744de2` in the logs.

## The fix (all of it, not one piece)

1. `CMakeLists.txt`: `FLAC_CHOP_VERSION` is overridable — the git-describe derivation is wrapped in `if(NOT DEFINED FLAC_CHOP_VERSION)` and remains only the LOCAL-build fallback.
2. `.github/workflows/build.yml`, every build job: `-DFLAC_CHOP_VERSION="${{ steps.version.outputs.version }}"` on the cmake configure + `fetch-depth: 0` on checkout (so the fallback also resolves when no override is passed).
3. CI PACKAGING TESTS (fail the build, do not eyeball): every platform asserts the built exe's `--version` output equals the resolved version EXACTLY; Windows additionally expands the portable ZIP and runs the inner exe, and silent-installs the Inno installer, runs the installed exe's `--version`, then silent-uninstalls it; Linux runs the AppImage itself with `--version` (AppRun forwards CLI args headless; APPIMAGE_EXTRACT_AND_RUN=1 for the FUSE-less runners); macOS runs the staged .app binary.

## How to verify (hard data, not "looks fine")

- Never trust a title bar: `flac-chop --version` (the CLI prints in real consoles — the WIN32-subsystem fix re-attaches the parent console) must print `FLAC-Chop <tag>`; grep the binary for the `dev-` literal as a second check.
- The CI Verify steps above are the gate — a release tag push whose exe reports `dev-` can no longer produce artifacts.

## Checklist for any version/packaging change

1. Pass the resolved version INTO the build (`-DFLAC_CHOP_VERSION=…`) — never rely on git state inside CI.
2. Keep the local fallback working: no `-D` on a local dirty tree → `v<lasttag>-dirty` is CORRECT there.
3. Every packaging job gets an assert-style Verify step (built exe, packaged artifact, installed exe).
4. `flac-chop --version` on the shipped artifact before publishing anything.
