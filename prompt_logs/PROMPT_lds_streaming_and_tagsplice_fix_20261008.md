# PROMPT: .lds streaming conversion + dead `.tmp-tagsplice` fix (2026-10-08)

## Inputs (user prompts, in order)
1. "bug when handling .lds it makes a '.temp-tagsplice' instead of the clean direct flac markers embedding this leaves a dead file."
2. "lds works does not stream and comrpoess a file while upacking it trys to unpack the whole thing also"

## Diagnosis (code + hard data)
Two independent bugs in the .lds → FLAC conversion flow, both rooted in the pipeline writing everything to disk before compressing:
1. **Dead `.tmp-tagsplice` + markers not embedded**: the post-cut RF tag embed (`rewrite_cut_tags`) on a fresh SoX output (no PADDING block) always fell back to `splice_vc_core` — a FULL second copy of the output written to `<output>.flac.tmp-tagsplice`, then `std::fs::rename` over the output. Two failure modes, both leaving the dead temp + no markers: (a) a full disk dies mid-copy (C: had only ~250 MB–2.3 GB free; the splice needs output-sized free space), and (b) Windows rename-over-existing fails with os error 5/32 while antivirus/indexer still holds the just-written file. The rename-error path and the mid-copy write-error path were the ONLY splice paths that did NOT remove the temp. The GUI also ignored `r.stderr` on success, so the "warning: tag rewrite failed" was invisible.
2. **No streaming**: `chop_lds_input` unpacked the ENTIRE covering window to a temp s16 on disk beside the output (a 10 s @ 40 MSPS window = 800 MB; a whole 352 GB .lds ≈ 704 GB), then SoX read that file back to compress it — double disk usage + a full extra pass over the data. Reproduced: 10 s conversion attempt failed "packed .lds: write failed: There is not enough space on the disk. (os error 112)".

## Changes made
- `core/src/chop.rs`:
  - NEW `make_lds_s16_feed` + `LdsFeedPlan`: the .lds unpacking STREAMS straight into SoX's stdin (`-t s16 -r <stream rate> -c 1 -`), group-by-group from a buffered reader (8 MB BufReader/Batches; `read_exact_group` refilled), the leading group-mates included and dropped by the inner `trim <skip>s`. BrokenPipe = benign end-of-feed (SoX stops reading once the trim window is satisfied) — same contract as the true-12-bit feed. Shell-out backend only.
  - `chop_native_input` split into a wrapper + `chop_native_input_ex(ext_feed)`; the ext-feed branch takes the caller's stdin input args and "-" as the input path.
  - `chop_lds_input`: cfg-split — shell-out streams (no temp at all); static-sox keeps the temp-s16 window (in-process libSoX reads files); identical stderr note.
  - `lds_window_temp_path` now `#[cfg(feature = "static-sox")]`.
- `core/src/lds.rs`: `read_exact_group` → `pub(crate)` + generic over `impl Read`.
- `core/src/tags.rs`:
  - NEW `shift_splice_in_place` (+`ShiftResult` Done/CleanErr/DamagedErr): insert/replace the metadata head by moving the audio frames WITHIN the same file — no temp file, no rename, and the only new disk allocation is the metadata growth (~4.3 KB: grow = extend first, move frames backward from the end, write the prebuilt head; shrink = move forward, write head, truncate). Verifies: chain re-parse, exact size, frame sync at the new offset, and the first/last 16 frame bytes preserved. Now the PRIMARY path for the cut-tag embed; the temp splice is the fallback.
  - `chain_with_vc_and_padding` extracted (shared by both splices).
  - `splice_vc_core` hardened: every failure path now removes the temp (the write block is wrapped; the block-too-large check included), and the rename goes through `rename_over_with_retry` (retries os error 5/32 for ~10 s — Windows AV/indexer locks clear on their own; other errors fail fast) with temp cleanup on final failure. A shift-decline + splice-failure now reports BOTH reasons.
  - Tests: `shift_splice_grows_in_place_without_temp`, `shift_splice_shrinks_in_place` (fixture bug found on first run: the 4596-byte PADDING header was written 0x12F4=4852 — fixed to 0x11F4).
- `gui/mainwindow.cpp` `onChopFinished`: on success the core's notes/warnings in `r.stderr` are no longer dropped — warnings ("warning:") pop a message box, notes append to the "Done. Output:" status line (a failed tag embed used to be invisible next to a dead temp file).

## Verification (commands + hard results)
- Build (MSYS2 MINGW64, rustup gnu): clean; `gui/flac-chop.exe` linked.
- `cargo test` (FLAC_CHOP_SOX set): **150 lib + 6 ffi_plan + 5 lds_integration + 4 parallel_cancel + 11 roundtrip12 = 176 passed, 0 failed**.
- Real data (source: `C:\Users\Harry\Desktop\RF-Sample_2022-12-11_00-00-34.lds`, 251,658,240 B = 201,326,592 samples @ 40 MSPS; outputs on L:, 101.7 GB free; C: too full):
  - **Whole-file conversion** (streaming feed): `ok: wrote L:\flac-chop-repro\ours-full.flac` (73,357,763 B); the directory contains ONLY the output — **no `.tmp-tagsplice`, no temp s16**. Decoded s16 vs the reference `ld-lds-converter.exe -u --s16 --sample-rate 40000` unpack: **SHA256-identical `0b513122...55ea5d`, 402,653,184 bytes** (all 201,326,592 samples).
  - **Markers embedded (ffprobe)**: RF_TOTAL_SAMPLES=201326592, RF_SAMPLE_RATE=40000000, RF_SAMPLE_RATE_KHZ=40000, DURATION_SECONDS=5.033165, LENGTH=5033; stream = 201,326,592 samples, 40 kHz header (the /1000 convention), mono s16; SoX's own comment preserved.
  - **Non-aligned partial cut** (start 3.086419725 s = sample 123,456,789, len 0.025 s = 1,000,000 samples): decoded s16 vs the reference slice at the same offset — **SHA256-identical `d5d3f603...69cd6`**; markers exact (RF_TOTAL_SAMPLES=1000000, DURATION_SECONDS=0.025000, LENGTH=25).
- `dist-vfix\flac-chop.exe` refreshed with the new build (7,045,851 B, 2026-10-08 17:19).

## Still awaiting user confirmation (on their screen, from dist-vfix)
1. A .lds conversion shows the live % progress and no long "unpack the whole thing to a temp" phase first (Task Manager: no giant `.tmp.s16` beside the output).
2. The output FLAC has the RF markers embedded (after the auto-load, the Metadata Editor tab shows RF_TOTAL_SAMPLES / RF_SAMPLE_RATE / etc.).
3. NO `.tmp-tagsplice` file appears next to the output, even on the nearly-full C: (though big outputs still need room for the FLAC itself — point the Output Directory at L:/E:).
4. The status line after "Done. Output:" shows the .lds provenance note (and a popup if anything warns).
