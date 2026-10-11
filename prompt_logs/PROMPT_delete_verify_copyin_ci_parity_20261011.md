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
