# PROMPT: Output File box at top + elapsed times + .lds pre-conversion metadata (2026-10-08/09 evening)

## Inputs (user prompts, in order)
1. "There is no metdata auto population dispite context of media in the file name" / "There is also no settable output named path i.g file name"
2. "run the fixed build then"
3. "No it just shows a greyed out path"
4. "I also want the finished total processing time added when files are finsihed for all modes"
5. "No the output file stuff should be at the top..." / "'output directory' should be replaced" / "There is no logic changes just remove the non-needed box with the Output File box"
6. "good now its just the .lds metadata loading"
7. "also add a browse button back to set the output directory manually"

## Changes made (gui/, all on the Chop tab; Batch/Sync tabs untouched)
- **Output File box replaced the Output Directory box at the top row** (next to Input File): the full auto-derived path sits in it as REAL black editable text (not a gray placeholder — the placeholder-era design read as a disabled field). Any edit marks it custom (kept through marker/mode changes); clearing restores the auto path; a bare name resolves against the input's folder with `.flac` appended; a full path wins outright. Stays editable while a cut runs (edits apply to the NEXT cut). The old out-dir box/widgets/slots/persistence (QSettings "output/dir") were removed; `effectiveOutDir()` = the input file's folder. A **Browse... button** beside the box opens a folder picker and moves the current name there (the browsed path becomes the custom path via textChanged).
- **Finished total processing time, all modes**: shared `formatElapsedMSecs` (stemutil.h). Chop: "Done in %1. Output: ..." (+ Cancelled/FAILED variants). Batch: per-row "Done in %1 — <path>" / "Failed (after %1) — ..." / "Cancelled (after %1)" + "Total time: %N." in the summary. Sync Edit: the same per-row + total.
- **DATE_RECORDED auto-populate** (`dateRecordedFromFilename`): a `YYYY-MM-DD[_HH-MM-SS]` token in the filename (the DdD/ld-decode convention) fills `DATE_RECORDED` in the Metadata Editor when not already present (loads of FLAC outputs).
- **.lds pre-conversion metadata loading** (`populatePreConversionMetadata`): loading a convertible non-FLAC source (WAV/raw/packed .lds/Ogg .ldf) now pre-populates the Metadata Editor as the authoring surface for the OUTPUT — RF template tags from the probe, DATE_RECORDED from the SOURCE's filename (so the context survives output renames), and the ingest fields (PROJECT/TAPE_ID/OPERATOR/LOCATION/NOTES) to fill; Save/Reload disabled (the tag-less source has nothing to write); Apply Template works there too.
- **Embed on Process**: `process()` gathers the editor rows (validated; an invalid row blocks the cut with a status message) and passes them via the new `fc_set_output_comments` FFI; a FLAC input always clears the pending set.

## Core changes
- `chop.rs`: `PENDING_OUTPUT_COMMENTS: Mutex<Vec<String>>` + `pub fn set_pending_output_comments` (the FFI setter). The take happens at the **cut START** of the single-cut (Global-cancel) path in `chop_with_options_and_cancel` — threaded through `chop_lds_input` / `chop_native_input(_ex)` (both backends) into `rewrite_tags_after_cut` → `tags::rewrite_cut_tags(path, is_rf, extra)`. Per-job (batch/sync) cuts never touch it; the take-at-start (not at-rewrite) keeps the consume window to one statement (no cross-chop theft).
- `tags.rs`: `merge_extra_comments` — each extra "KEY=value" replaces the same-key comment (case-insensitive) BEFORE the owned RF numeric updates, which always win (their values come from the output's STREAMINFO). Applied in both the in-place and splice paths.
- `ffi.rs` + `flacchop.h`: `void fc_set_output_comments(const char* const* comments, uint32_t n)` (n=0/NULL clears).
- Tests: `rewrite_cut_tags_merges_extras_and_owned_wins` (merge + owned-win semantics), `lds_cut_embeds_pending_output_comments` (integration: set pending → real .lds cut → DATE_RECORDED/PROJECT embedded, RF tags exact and beating a conflicting extra).

## Drag-and-drop fixes (follow-up prompts)
- "fix the file input drag and drop feature": every `QLineEdit` in every tab accepted file drops by default (inserting the path as text) — the Output File box at the top swallowed drops meant to load the file. All line edits now opt out (`findChildren<QLineEdit*>()->setAcceptDrops(false)`), so a drop anywhere routes to the window handler. Plus a guard: a single-file drop while a Chop cut runs asks to wait (queue adds stay allowed).
- "batch task is not working it should allow 1 by one drag and drop additions": a single dropped file on the **Batch Task tab** now adds to the queue (the Sync tab already had this exception; single-file drops on other tabs still load the Chop tab). A busy batch run says "in progress — add after it finishes" instead of the misleading "already queued".
- Flaky-test hardening: the lds integration suite's shared `cut()` helper now uses per-job cancel flags (never touches the global pending set), so concurrent tests can't steal another test's pending single-cut embed set (8 consecutive green runs + full suite).

## Verification (commands + hard results)
- Full suite (FLAC_CHOP_SOX set): **178 passed, 0 failed** (151 lib + 6 ffi_plan + 6 lds_integration incl. the new embed test + 4 parallel_cancel + 11 roundtrip12).
- Local test-build version stamps so stale windows are identifiable: `dev-20261008-1830`, then `dev-20261009-0010` (in the title bar).
- `dist-vfix\flac-chop.exe` = dev-20261009-0010 (7,070,387 B, 2026-10-09 01:11), launched with the .lds preloaded.
- Earlier phases of today (streaming .lds, shift splice, marker embeds) are covered in PROMPT_lds_streaming_and_tagsplice_fix_20261008.md.

## Still awaiting user confirmation (on their screen, dev-20261009-0010)
1. Top row: Input File (left) + Output File (right) with the real editable path; Browse... beside it moves the current name into a picked folder.
2. Loading the .lds: the Metadata Editor tab pre-populates (RF template + DATE_RECORDED=2022-12-11 00:00:34 from the filename + ingest fields) — edit rows, then Process.
3. After the conversion: the output carries those rows (check ffprobe/the re-loaded editor) AND the exact RF markers; no `.tmp-tagsplice`.
4. Finish lines show the elapsed time in all three tabs.
