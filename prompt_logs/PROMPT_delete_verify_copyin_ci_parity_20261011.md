# Prompt log — delete-after-verify, ld-compress-ng copy-in, CI hardening, CLI cleanup + CLI→GUI parity (2026-10-11)

User request (multiple parts): ld-compress-ng code must be a direct copy-in
integration (no vendoring); add a delete-after-verify box to Batch mode;
commit + push + run a GH Actions build test; CI tests need real sample
updating; review/cleanup the CLI commands; add CLI-->GUI load testing like
MISRC GUI for automated feature testing with 1:1 CLI/GUI parity.

Plan: 6138988d-f9e8-4210-8677-3e49298340b3 (approved as-is, including the
defaulted decisions: permanent delete, --rate validation, commit cadence).

## Phase 1 — Delete-after-verify (Batch tab) — DONE, verified

Changes:
- gui/batchtab.h/.cpp: "Delete source after PASS verify" checkbox (default
  OFF, persisted batch/deleteAfterVerify, disabled while verify is off —
  no proof no delete; both batch checkboxes now persist on toggle, closing
  the missing batch/verify save gap). Worker deletes the input ONLY on an
  exact `verify: PASS` verdict, with an output≠input canonical-path guard
  on top of the existing write-protected-input re-routing. Row status gets
  "source deleted (verify PASS)"; summary appends "Deleted N source(s)
  after PASS."
- Automation surface (the first piece of the CLI-->GUI parity work):
  BatchTab::automatedAddAndRun + setVerifyOutputOption/setDeleteAfterVerifyOption;
  MainWindow env hook FLAC_CHOP_GUI_BATCH_FILES (";"-separated) +
  FLAC_CHOP_GUI_BATCH_RUN + FLAC_CHOP_GUI_BATCH_VERIFY/DELETE overrides +
  FLAC_CHOP_GUI_TEST_OUT (any batch run writes a JSON report: per-job
  status/verify/deleted_source/elapsed_ms/source_exists + summary) +
  FLAC_CHOP_GUI_TEST_QUIT (self-exit after the report; failure popups are
  suppressed while the report is active so automation never blocks).
- readme.md + docs/DEV_NOTE_ldc_acceleration.md: "the core never deletes
  sources itself; the Batch tab's opt-in delete-on-PASS is the automated
  form of the PASS verdict".

Build: cmake --build build-vfix clean; exe staged into dist-vfix.

Real-data verification (L:\fc-batchtest, group-aligned 4 MB .lds truncations
of the Desktop RF-Sample at 40 MSPS + a CLI-converted FLAC):
- Automated GUI batch (VERIFY=1 DELETE=1 RUN=1): pass1.lds + pass2.lds →
  GPU whole-file conversion, `verify: PASS (3200000 samples, bit-perfect)`,
  deleted_source=true, source_exists=false (files GONE from disk);
  keep1-cut.flac (FLAC source, no verify note) → status ok, kept
  (source_exists=true). Summary: "Batch complete: 3 done ... Verified: 2
  passed, 0 flagged. Deleted 2 source(s) after PASS." GUI self-exited 0.
  Report at L:\fc-batchtest\report.json.
- PowerShell note: this GUI-subsystem exe is NOT waited on by `&` — a CLI
  chop "disappears" while it runs in the background (first fixture attempt
  deleted the input mid-conversion that way). Always Start-Process -Wait
  (or poll) for CLI invocations from pwsh.

GUI relaunched (dev-20261010-vfy build) for the user's on-screen check of
the checkbox + behaviour.

## Phase 2..4 — in progress (see later sections)

## Phase 2 — ld-compress-ng copy-in — DONE, verified

- flac-chop/ld-compress-ng/ = direct copy-in of the ld-compress-ng working
  tree (WITH the uncommitted Windows port): src/, shaders/, cmake/, tests/,
  tools/, docs/, LICENSES/, CMakeLists.txt, LICENSE, THIRD_PARTY_NOTICES.md,
  README.md, CHANGELOG.md (86 files, 1.7 MB). Upstream base 7a60555 + the
  port deltas recorded in ld-compress-ng/COPIED_FROM.md. No submodule, no
  build-time clone.
- Top-level CMakeLists.txt: FLAC_CHOP_BUILD_LDC (default ON) ->
  add_subdirectory(ld-compress-ng) with BUILD_TESTING forced OFF (cargo owns
  testing); gui/CMakeLists.txt POST_BUILD-copies ld-compress-ng(.exe) beside
  flac-chop so ldcdet finds it in dev trees. .gitignore covers
  /ld-compress-ng/build*/.
- Local build (MSYS2 MINGW64, OpenCL + Vulkan both enabled): 41 targets,
  clean. Verified from the build tree: `devices` lists the GTX 1080 Ti
  (OpenCL 3.0 CUDA); real 4 MB 40-MSPS .lds whole-file conversion 0.97 s,
  verify: PASS, sha256 note. NOTE: running the un-bundled build-tree exe
  from pwsh fails 0xC0000139 (foreign Qt on PATH) — dev-tree runs belong in
  the MSYS2 env; end-user runs come from the bundle.
- dist-vfix re-bundled with the in-repo-built exes; cleaner pass clean
  (0 system DLLs; zlib1.dll is the MSYS2 one). Automated GUI batch from the
  bundle: verify PASS in 689 ms, self-exit 0 (report2.json).
- Harness bug found + fixed while verifying: an all-skipped batch returned
  BEFORE the automation report/quit (a parity run would hang forever). The
  report is now a shared lambda covering BOTH exit paths (all-skipped gets
  status "skipped" + quit).
- Cosmetic CLI issue logged for phase 4: runCli prints "note: note: ..."
  (doubled prefix from the notes already starting with "note: ").

## Phase 3 — CI real tests + ldc everywhere — DONE (pending CI green)

- Core: pub ldc_path() (both cfg variants) + lds_integration
  ldc_device_present() (the CLI `devices` report lists "[N] name" rows);
  the two required-backend tests now assert the clean no-device failure and
  SKIP the byte-exact assertions on GPU-less runners (rate-guard/partial/
  fallback tests are device-independent, unchanged). Local suite with the
  copy-in binary + GPU: lds_integration 11 + parallel_cancel 4 + roundtrip12
  11 + ffi_plan + lib all green, 0 failed.
- tests.yml: per-OS SoX install (apt/brew/choco — every cut test ran
  skipped before); Linux job builds the copy-in CLI (cmake standalone
  configure of ld-compress-ng, BUILD_TESTING=OFF) + FLAC_CHOP_LDC via
  GITHUB_ENV.
- build.yml: cargo-tests preflight gets SoX + the CLI build + env. Every
  packaging job builds the subproject in-tree and bundles the CLI beside
  flac-chop: AppImage usr/bin + linuxdeploy -e; Windows x86_64 (full
  Vulkan/OpenCL/glslang/flac/libogg MSYS2 packages; the existing ldd
  fixpoint bundles its DLLs); Windows arm64 (CLANGARM64 flac+libogg,
  Vulkan/OpenCL OFF -> CPU-libFLAC backend); macOS (Metal; brew flac+libogg;
  the .app staging bundles it with the same otool/install_name_tool pattern
  as sox). Artifact assertions require ld-compress-ng beside flac-chop in
  the portable ZIPs, installers, and the staged .app (--version check).
- docs/DEV_NOTE_ldc_acceleration.md: copy-in policy section + CI section
  rewritten (done 2026-10-11).
- Phase-1 baseline CI on e39960e: Tests run 38120200011 SUCCESS; Build run
  38120226148 in progress at the time.

### Phase 3 addendum — CI exposed a real core bug (fixed)

The phase-1 baseline Build run (38120226148, e39960e) failed its
"Real-world data tests" job (the sox-having runner in build.yml I had not
mapped -- it also runs the .lds/FLAC/raw example-CLI probe/cut steps on
synthetic real-format data): lds_gpu_rejected_for_non_40msps_lds +
lds_partial_cut_stays_on_sox_even_with_ldc_backend FAILED. Root cause in
chop_lds_input: the Ldc not-found error fired BEFORE the whole-file/rate
guards, so (a) a partial cut with GPU selected failed "not found" instead
of just running SoX, and (b) a non-40-MSPS whole-file cut reported
not-found instead of the 40-MSPS constraint. Fix: the required-binary check
now follows the rate guard's rule (whole-file + plain + 40 MSPS only).
Verified locally BOTH ways: lds_integration 11/11 with sox only (the exact
CI mode) and 11/11 with the GPU path. The 9cc7055 runs still carry the old
code (their real-world job will fail; the packaging jobs still validate the
copy-in per platform).

### Phase 3 addendum 2 — libFLAC version on the Linux runners

The 9cc7055 "Cargo tests" preflight failed compiling the copy-in CLI:
ubuntu-22.04 apt libflac-dev is 1.3.4; flac_codec.cpp uses the 1.5
chained-stream APIs (FLAC__STREAM_DECODER_END_OF_LINK,
FLAC__stream_decoder_set_decode_chained_stream). MSYS2 + Homebrew ship 1.5
(the local/macOS/Windows builds were fine). Fix: scripts/ci_build_flac.sh
builds libFLAC 1.5.0 into /usr/local (cmake + ldconfig + PKG_CONFIG_PATH via
GITHUB_ENV); the three Linux CI locations call it instead of libflac-dev.
In-flight 3507935 runs cancelled (they carry the old yml); re-pushed +
re-dispatched.

## Phase 4 — CLI cleanup + CLI-->GUI load testing — DONE, verified

CLI (gui/main.cpp):
- --help / -h exit 0 (was 2); --probe without a file prints a clear usage
  error (exit 2); --rate validated to the five RF output modes; usage text
  documents the --lds-backend alias; chop success notes print once-indented
  (the old form doubled the prefix: "note: note:") with blank lines skipped.

GUI automation (the MISRC model):
- --smoke-test: full GUI startup then exit 0; headless-safe with
  QT_QPA_PLATFORM=offscreen (verified). --config <dir>: QSettings INI
  override applied before any settings use (missing dir = clear exit 2).
- FLAC_CHOP_GUI_TEST=1 + _OUT (+ _QUIT / _RUN) on a --gui launch: the Chop
  tab dumps probe + plan + ldc_available state; with _RUN it drives the
  exact Process click path and onChopFinished dumps the run report
  (ok / verify / sha brief / full notes / output / elapsed). Guards:
  first-only probe dump, once-only auto-run trigger, auto-load skipped
  during test runs (no re-probe in flight at exit), popups suppressed.
- scripts/gui_cli_parity.sh (the 1:1 gate): a 20 MSPS .lds fixture (GPU
  never engages -> deterministic SoX on both sides) through the CLI
  (probe --json + chop) and the GUI (TEST hooks), asserting probe-field
  parity, verify-PASS-with-count parity, identical STREAMINFO (sox --i),
  and byte-identical decoded audio (sox -> raw -> sha256; containers
  legitimately differ — the GUI embeds the pre-conversion editor rows).
  Script fixes along the way: the GUI output name drops the .lds extension
  (completeBaseName semantics), and sox/soxi resolve from FLAC_CHOP_SOX's
  directory when not on PATH (sox --i replaces a separate soxi).
- Verified locally: parity gate ALL CHECKS PASSED (decoded audio sha
  4606d35f...755272); --help 0, --probe-no-file 2, --rate-validation 2,
  --config-missing 2, --smoke-test 0 (offscreen).
- tests.yml Linux leg runs the parity gate as a CI step (offscreen,
  -DFLAC_CHOP_BUILD_LDC=OFF); readme documents the automation surface.

### Phase 3/4 addendum — the 6517565 CI failure round (three causes, fixed)

1. Windows Tests: choco has no "sox" package -> sox.portable is the real ID.
2. Linux Tests + Build preflight: exit 126 — scripts committed from Windows
   carry no executable bit. git update-index --chmod=+x on
   scripts/ci_build_flac.sh + scripts/gui_cli_parity.sh.
3. macOS Tests (parallel_cancel, first time it ever RAN there — sox was
   never installed on mac CI before): cancel_mid_flight asserted on
   "cancelled" in stderr, but the kill path replaced the message with the
   killed sox's buffered -S progress dump (macOS timing; Windows kills
   before any stderr). Core fix: the cancel result ALWAYS names the cancel,
   keeping the child stderr only as a diagnostic tail. Local re-verified:
   parallel_cancel 4/4.

### Phase 3/4 addendum — the 5168e6f CI failure round (two causes, fixed)

1. Ubuntu: ld-compress-ng HAS a CPU backend — on the GPU-less runner the
   required-backend run COMPLETED via cpu backend (my no-device assumption
   was wrong), and its output was not claxon-readable there ("Ill-formed
   FLAC stream" — the tag rewrite + verify layers flagged it loudly, sha256
   still computed). The two accelerated tests now branch on the RESULT:
   !ok -> assert the clean no-backend failure; ok but output not a readable
   FLAC -> skip with a loud note (the copy-in CPU backend is not CI-gated;
   the verify layer catches it in practice); readable -> full byte-exact
   assertions (local GPU run: 11/11 full). ldc_device_present + its devices
   probe removed; pub ldc_path stays (tooling API).
2. Windows: choco sox.portable broke the whole .lds->SoX pipeline (stdin-fed
   AND file-based cuts failed with no usable sox error text — the shim is
   unfit). tests.yml Windows leg now uses MSYS2 MinGW64 sox via
   setup-msys2 + FLAC_CHOP_SOX=cygpath -w /mingw64/bin/sox.exe — the exact
   build the Windows artifacts bundle.

### Phase 3/4 addendum — the e20d520 Build run (two fixes)

Tests workflow FULLY GREEN on all three OSes (first ever run of the real
suite with sox everywhere + the copy-in ldc on Linux). Build run: Cargo
tests preflight, Linux AppImage x86_64+arm64, Windows x86_64, macOS arm64 +
x86_64, Real-world data tests ALL GREEN (the copy-in builds + bundles on
every platform). Two failures:
1. Windows arm64: "no member named 'sort' in namespace 'std'" — LLVM-mingw
   does not supply <algorithm> transitively (GCC does). Fix: explicit
   #include <algorithm> in the copy-in src/main.cpp + COPIED_FROM.md delta.
2. macOS universal DMG: the documented transient hdiutil "Resource busy"
   flake — three fast retries were not enough this time. Fix: pre-detach +
   5 attempts 15 s apart.
