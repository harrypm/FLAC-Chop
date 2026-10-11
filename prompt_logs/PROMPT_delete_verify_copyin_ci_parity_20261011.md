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
