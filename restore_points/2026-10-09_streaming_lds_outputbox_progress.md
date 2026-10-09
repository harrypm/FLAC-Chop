# Restore point — streaming .lds + Output File box + progress/metadata session (2026-10-09)

User-confirmed working ("good", 2026-10-09): Output File box at the top
(replacing Output Directory) with the Browse button, one-by-one drag-and-drop
additions to the Batch Task queue, `.lds` metadata loading (pre-conversion
editor + embed into the output), live % progress, elapsed finish times,
streaming `.lds` conversion, in-place tag splice.

Archive: `C:\Users\Harry\fc-restore-points\flac-chop_2026-10-09_lds-outputbox.zip`
(git archive of this commit — master). To go back: unzip over a checkout, or
reset to the commit that added this note:
`git log -1 --format=%H -- restore_points/2026-10-09_streaming_lds_outputbox_progress.md`.

## What shipped in this state (since v1.0.9, fd696e1..HEAD)

1. **Live progress %**: SoX `-S` `overall:X.XX%` parsed by the stderr reader
   thread into an atomic (`fc_chop_get_progress`), polled by a 200 ms GUI
   timer; 0-100% bar with text, 100% left showing on success; busy
   probe/save indicators hide the % text; PROGRESS resets to idle at every
   end-of-cut path.
2. **Streaming `.lds` conversion** (`make_lds_s16_feed`): the unpacked window
   flows straight into SoX stdin (the 12-bit feed contract) — no temp s16
   ever materialized (whole-file conversion of a huge `.lds` needs no
   window-sized free space; source read exactly once).
3. **Tag embedding rewrite**: new in-place SHIFT splice (frames move within
   the same file — no temp file, no rename, ~4 KB of new allocation; the fix
   for the dead `.tmp-tagsplice` + markers-not-embedded on full/AV-locked
   disks) as the primary path; the temp-file splice fallback now always
   cleans up and retries Windows lock-failed renames (os error 5/32, ~10 s).
4. **Output metadata embedding** (`fc_set_output_comments`): caller-authored
   rows (the pre-conversion editor) merged into the next single cut's output
   before the owned RF numeric tags (which win); taken at the cut START and
   threaded to the rewrite (per-job batch/sync cuts never touch it).
5. **Output File box replaces Output Directory** at the top row (next to
   Input File): real editable path+name (auto-derived until edited; a bare
   name lands in the input's folder as `<name>.flac`; clearing restores the
   auto path; editable mid-cut for the NEXT cut) + a Browse button that
   picks the output folder for the current name.
6. **Elapsed processing times, all modes**: Chop ("Done in …"), Batch + Sync
   Edit per-row statuses and run totals (`formatElapsedMSecs`).
7. **DATE_RECORDED auto-populate** from the filename (DdD `YYYY-MM-DD[_HH-MM-SS]`
   convention) + the **pre-conversion metadata editor**: loading a convertible
   non-FLAC source (WAV/raw/`.lds`/`.ldf`) pre-populates the editor as the
   authoring surface for the OUTPUT (RF template + DATE_RECORDED from the
   SOURCE's filename + ingest fields), embedded on Process.
8. **Converted outputs auto-load** (the metadata editor works on the fresh
   FLAC output); the finish status surfaces the core's notes/warnings
   (warning popup) instead of hiding them; "Process to FLAC" button label for
   convertible inputs.
9. **Drag-and-drop**: no line edit swallows file drops anymore (every
   `QLineEdit` opts out — the Output File box was the big top-row eater);
   single-file drops add to the Batch/Sync queue when that tab is active
   (one-by-one queue building); mid-cut single-file loads are refused with a
   clear status; queue adds during a run say so.

## Verification summary (hard data)

- cargo test (sox forced locally): **178 passed, 0 failed** — 151 lib
  (incl. new shift-splice grow/shrink + extras-merge tests) + 6 ffi_plan +
  6 lds_integration (incl. the new pending-embed test; the suite's `cut()`
  helper now uses per-job flags so the global pending set can't be stolen —
  8 consecutive green runs) + 4 parallel_cancel + 11 roundtrip12.
- Real-data `.lds` (RF-Sample_2022-12-11_00-00-34.lds): whole-file conversion
  decoded s16 **SHA256-identical** to the reference ld-lds-converter unpack
  (`0b513122…55ea5d`, 402,653,184 bytes); non-aligned partial cut byte-exact;
  markers ffprobe-verified (`RF_TOTAL_SAMPLES=201326592`, etc.); NO
  `.tmp-tagsplice` / temp s16 left behind.
- Test builds stamped `dev-20261008-1830` / `dev-20261009-0010` in the title
  bar so stale windows are identifiable.
- Session logs: `prompt_logs/PROMPT_{progress_output_path_metadata_button,
  lds_streaming_and_tagsplice_fix,outputbox_elapsed_ldsmetadata}_20261008.md`.
