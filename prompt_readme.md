# Prompt log — 2026-09-06

Prompt: continue active work — validate STREAMINFO in-place repair on the real HiFi capture, validate 10 MSPS profile (8-bit byte-exact vs manual sox, 6-bit tags+levels), GUI rebuild + docs.

## Commands run / results

1. `git -C /home/harry/FLAC-Chop status --short && git log --oneline -5`
   - Modified: core/examples/chop_convert_cli.rs, core/src/chop.rs, core/src/lib.rs, core/src/probe.rs, gui/main.cpp, gui/mainwindow.cpp; untracked: core/src/streaminfo.rs
   - HEAD d1872b4 "Support multi-format inputs: WAV, raw PCM, .ldf, magic sniffing"
2. `cargo test --manifest-path /home/harry/FLAC-Chop/core/Cargo.toml`
   - FAILED to compile: mismatched delimiters in src/chop.rs (784/849/869) — the static-sox `chop_with_options` had a mangled mid-edit section (broken closure + `match result {` with no arms).
3. Fix: rewrote the mangled section of static-sox `chop_with_options` (core/src/chop.rs ~798-874):
   - sniff format → `in_hints_for` → `maybe_repair_streaminfo` (si_note prepended to stderr after validation, mirroring the shell-out backend)
   - two-pass 6-bit flow via `run_chain` (vol 0.25 → 8-bit sink, then x4 rescale), no dither
   - `validate_cut_output` + panic-safe `rewrite_tags_after_cut` retained
4. `cargo test` after fix: 77 core tests + 6 ffi_plan tests PASS.
   - Cleaned dead test helper `read_rate` in core/src/streaminfo.rs (was an unused warning).
5. Capture for validation: /home/harry/Desktop/Tomas Tracking headsetichc test/VHS_PAL_SP_Tape_03_2026.08.10_00.00.53_hifi_rf_8-bit_10msps.flac (972,426,305 bytes)
   - Work on COPIES in /tmp/fc-val — original file left untouched.
   - Probe: format flac, header 10000 Hz (is_rf, real 10 MSPS), declared_total 2165571 vs RF_TOTAL_SAMPLES 2165570800 (the /1000 mis-declaration).
6. Fail-case demo (unrepaired copy): `sox -D b_ref.flac fail_demo.flac trim 1000000000s 50000000s` → WARN "Start position is after expected end of audio" + "audio shorter than expected", exit 0, 220-byte header-only output.
7. Independent repair of b_ref.flac via python (36-bit field patch, declared 2165571 → 2165570800).
8. Manual sox reference: `sox -D b_ref.flac -r 10000 ref_out.flac trim 1000000000s 50000000s sinc -n 2500 0-3050` → 20,476,753 B, decodes ok.
9. chop CLI on unrepaired a_chop.flac: `chop_convert_cli a_chop.flac chop_out.flac 100 5 10000 0 1` → plan start=1000000000 len=50000000; note "repaired STREAMINFO declared total (2165571 → 2165570800 …)".
   - chop's repair == python repair: 0 differing header bytes.
   - BUG FOUND: chop_out fails `flac -t` (LOST_SYNC). Frames displaced exactly 1 byte by the lofty 0.18 tag rewrite (chop[1296:] == ref[221:] over 20,476,532 B; stray 0x04 after padding; lofty reported success — silent corruption).
10. Fix: tags.rs rewritten as a manual FLAC Vorbis-comment splice (parse chain → replace VC payload → copy other blocks + frames byte-for-byte → self-verify chain/sync/size → atomic rename). lofty removed from Cargo.toml. New tag_rewrite_cli example for isolated repro.
    - Test-failure detour: first self-verification compared the new frame offset to the OLD offset — fixed to expect new-metadata-end and exact size (metadata + original frames).
    - cargo test: 81 core + 6 ffi PASS.
11. Re-validation on real data:
    - tag_rewrite_cli on known-good sox output → flac -t ok, decoded audio unchanged, tags: RF_TOTAL_SAMPLES=50000000000, RF_SAMPLE_RATE=10000000, RF_SAMPLE_RATE_KHZ=10000, DURATION_SECONDS=5000.000000, LENGTH=5000000.
    - Full end-to-end chop_convert_cli (8-bit, fresh copy a_chop2.flac) → flac -t ok; decoded md5 9e4211e7b276476ad37488640cdf89b6 == manual sox reference.
    - 6-bit end-to-end (c6.flac) → flac -t ok; 50,000,000 samples, 0 non-multiples-of-4 (no dither), peak 40; tags correct.
12. GUI: `cmake --build build` → Built target flac-chop (relinked against updated core).
13. Docs: readme.md (10 MSPS HiFi FM profile, self-healing headers, validated cuts + splice tag rewrite, chop_convert_cli usage), dev_notes.md (validation session log).

## Follow-up prompt: adopt MISRC-GUI in-place tag method
14. Read MISRC-GUI method: misrc_tools/common/flac_writer.c reserves a 4096-byte PADDING adjacent to the VC at encode time; gui_record.c `gui_record_finalize_flac_metadata` updates STREAMINFO + VC in place via `FLAC__metadata_simple_iterator` (`use_padding=true`) — replacing the chain-write path that rewrote multi-GB files to a temp copy.
15. tags.rs: added `update_vorbis_comment_in_place` — (1) adjacent padding absorbs the delta, (2) sox cut layout: vendor trimmed by the growth / PADDING inserted for shrink slack, (3) fallback full splice now leaves a MISRC-style 4096-byte PADDING after the VC. lofty stays removed.
    - Test-caught bugs: inserted PADDING header written as type 0 (0x80) instead of type 1 (0x81) in both region builders; fallback did not grow an existing too-small padding to 4096. Fixed.
    - cargo test: 85 core + 6 ffi PASS.
16. Real-data validation: in-place rewrite on the real sox cut → size unchanged (20476753 B), no temp file, `flac -t` ok, decoded audio bit-identical, tags updated. Full end-to-end chop → `flac -t` ok, decoded md5 9e4211e7b276476ad37488640cdf89b6 == manual reference, no temp file.
17. GUI relinked; readme.md feature bullet + dev_notes.md updated.

## Follow-up prompt: add Metadata Editor page / tab dropdown

Prompt: add a metadata editor page / tab dropdown. Clarified scope via Q&A:
QTabWidget tab bar (Chop | Metadata Editor); edit ALL Vorbis comment tags
(full key/value table, add/remove/edit); write in place on the source file
(reuse the tags.rs in-place padding splice); show read-only STREAMINFO summary
at the top of the editor page.

## Commands run / results

1. `cd /home/harry/FLAC-Chop/core && cargo test` (after tags.rs refactor)
   - 95 core tests passed; 0 failed. No regressions from extracting
     write_vc_in_place_core / splice_vc_core.
2. `cd /home/harry/FLAC-Chop/core && cargo test` (after adding metadata FFI)
   - 100 core tests passed (95 + 5 new: pack blob roundtrip, 3 error-path
     FFI tests, 1 end-to-end read→replace→read through the ABI); 0 failed.
3. `cd /home/harry/FLAC-Chop/core && cargo build --release`
   - Finished release profile; staticlib rebuilt with the new symbols.
4. `nm -s core/target/release/libflac_chop_core.a | grep -i fc_`
   - Archive symbol index lists fc_comments_blob_size, fc_read_comments_blob,
     fc_replace_comments alongside fc_probe/fc_chop/fc_plan (plain nm shows
     nothing because the release .a is LTO bitcode; the armap is authoritative).
5. `cmake --build build`
   - Built target flac-chop; compiled mainwindow.cpp; linked the GUI against
     the updated core. No errors / no warnings.
6. `./build/gui/flac-chop --version` → `FLAC-Chop v1.0.0-2-g842384b-dirty`
   - Binary runs; link is sound.

## What changed

- core/src/tags.rs: extracted write_vc_in_place_core + splice_vc_core (the
  existing cut-tag rewrite path now delegates to them, behaviour identical);
  added pub read_all_comments(path) and pub replace_all_comments(path,
  &[(key,value)]) which validate keys ([A-Za-z0-9_]+, upper-cased), preserve
  the existing vendor, try in-place then fall back to the verified temp-file
  splice. New unit tests for read / in-place replace (case+removal+add) /
  key validation / vendor preservation / VC insert when missing / splice
  fallback / round-trip.
- core/src/ffi.rs: added fc_comments_blob_size, fc_read_comments_blob
  (packs u32LE count + per-comment u32LE len + "KEY=value"), fc_replace_comments
  (takes a const char* const* array). pack_comments_blob pure helper + tests.
- gui/flacchop.h: declared the three new C functions.
- gui/mainwindow.h: added QTabWidget/QTableWidget forward decls, FcMetaResult
  struct, editor widgets/slots/methods, m_metaWatcher, m_probeIsRefresh.
- gui/mainwindow.cpp: restructured the single-page UI into a QTabWidget with a
  Chop tab (all existing widgets moved onto a chopPage) and a Metadata Editor
  tab (read-only Stream Info group + editable 2-col Field/Value table +
  Add/Remove/MoveUp/MoveDown/Reload/Save buttons + status). loadMetadata reads
  via the blob FFI; saveMetadata runs fc_replace_comments off-thread via
  QtConcurrent; on success it re-probes (m_probeIsRefresh) so the Chop page
  reflects any changed RF_TOTAL_SAMPLES/RF_SAMPLE_RATE and reloads the editor,
  clamping the existing IN/OUT markers to the new total instead of resetting.
  browse()/dropEvent() now also block while a metadata save is in flight.

## Status / not yet validated

- Core logic is covered by cargo tests (incl. an end-to-end read→replace→read
  through the real FFI on a synthesized FLAC).
- The GUI compiles + links + runs, but the tab/editor/save has NOT been
  confirmed on real data in the running GUI yet (per the user-interactable
  rule). Awaiting user confirmation: load a real FLAC, switch to the Metadata
  Editor tab, edit a tag, Save, and report what happens.

## Follow-up prompt: Apply Template button (tagless RF files)

Prompt: add an Apply Template button so files without tagging but with context
extracted by flac-chop can apply it. Clarified scope via Q&A: populate the
computed RF tags (RF_TOTAL_SAMPLES, RF_SAMPLE_RATE, RF_SAMPLE_RATE_KHZ,
DURATION_SECONDS, LENGTH) from probe context + blank ingest rows (PROJECT,
TAPE_ID, OPERATOR, LOCATION, NOTES) for the user to fill; merge (only add
missing keys, leave existing values); load into the editor table for review
then Save; RF captures only.

## Commands run / results

1. `cd core && cargo test` (after adding rf_template_from_probe + FFI + tests)
   - 109 core tests passed (100 + 9 new template tests); 0 failed. ffi_plan 6.
2. `cd core && cargo build --release` — Finished; staticlib rebuilt.
3. `nm -s target/release/libflac_chop_core.a | grep fc_rf_template` — symbol
   fc_rf_template_from_probe present in the archive index.
4. `cmake --build build` — Built target flac-chop; compiled mainwindow.cpp;
   linked. No errors / no warnings.
5. `./build/gui/flac-chop --version` → `FLAC-Chop v1.0.0-2-g842384b-dirty`.

## What changed

- core/src/ffi.rs: added pub fn rf_template_from_probe(&FcProbe) computing the
  standard RF tag set from probe context. Resolves the real on-disk count:
  vorbis/companion/scan totals are already real; a trusted STREAMINFO header
  may be /1000 (early MISRC schema) or real (later schema) — decided by the
  same audio-payload sanity check the probe uses (uncompressed < audio_bytes
  && *1000 fits => scale *1000). Empty for non-RF / unknown-rate. Added
  fc_rf_template_from_probe C ABI (takes const FcProbe*, writes the packed
  blob, returns bytes written / 0 on error). 9 unit tests: vorbis total as-is,
  early-schema header scaled *1000, later-schema header not scaled, unknown
  total (rate tags only), non-RF empty, companion total is real, FFI blob
  packing, non-RF empty blob, null-probe error.
- gui/flacchop.h: declared fc_rf_template_from_probe.
- gui/mainwindow.{h,cpp}: added m_metaTemplateBtn ("Apply Template") in the
  editor button row, gated to RF FLAC only in setMetaEnabled. applyTemplate()
  calls fc_rf_template_from_probe(&m_probe), parses the blob via the shared
  parseCommentsBlob helper, merges each pair into the table only if the key is
  missing (case-insensitive metaHasKey), then adds blank PROJECT/TAPE_ID/
  OPERATOR/LOCATION/NOTES rows (only missing). Status reports rows added.
  Refactored loadMetadata to reuse parseCommentsBlob (one parser, not two).

## Critical detail verified against hard data

probe.total_samples unit depends on its source (read core/src/probe.rs + the
vorbis schema tests): vorbis/companion/scan => real on-disk count; trusted
STREAMINFO header => /1000 count (early schema, e.g. 2165571 for a 216.557 s
10 MSPS file) OR real count (later schema, 2165570800). A naive
RF_TOTAL_SAMPLES = probe.total_samples would be 1000x wrong for early-schema
tagless files. The template uses the audio-payload sanity check to detect
/1000 units and scale *1000 (validated by the early-schema test:
2165571 -> 2165571000). The later-schema test confirms no scaling
(2165570800 stays). Both match what tags::rewrite_cut_tags would produce
from STREAMINFO for a cut output.

## Status / not yet validated

- Core logic covered by cargo tests (incl. the schema-detection cases).
- GUI compiles + links + runs, but Apply Template has NOT been confirmed in
  the running GUI on real data yet. Awaiting user confirmation: load a tagless
  RF FLAC, click Apply Template, confirm RF_TOTAL_SAMPLES / DURATION_SECONDS
  look right, fill ingest rows, Save.

# Prompt log — 2026-09-27

Prompt: continue active work — CLI/GUI feature branch (A1–A6): create the
12-bit round-trip integration tests, validate the full 12-bit pipeline,
README CLI section, GUI build + CLI smoke, then commit/push.

## Commands run / results

1. `git status` — branch feature/cli-gui-loading; modified core/src/{chop,lib}.rs,
   gui/main.cpp, gui/mainwindow.{h,cpp}; untracked core/src/enc12.rs.
2. Created core/tests/roundtrip12.rs (9 integration tests: 12→12 sample-exact,
   12→16 ×16, 16→12 >>4, pure-trim true-12-bit default cut, full-length
   regression, flac -t, flac -d WAV cross-check, RF tag rewrite, 16-bit default).
3. `cargo test` — first run: 5 roundtrip12 failures. Root-caused each with
   hard data:
   - **SoX cannot READ true 12-bit FLAC at all.** A/B: built a libFLAC C-API
     reference 12-bit encoder (ref12.c, the MISRC method) — `sox ref12.flac -n
     stat` fails identically to our enc12 output: "data encoding or sample
     size was not specified". Root cause in SoX source formats.c
     `sox_precision()`: FLAC precision only for byte-aligned bps (8/16/24/32).
     `flac -t`/`-d`/metaflac/ffprobe/claxon all accept both files.
   - **Pipeline consequence:** `--bits 12` cuts FROM 12-bit sources (the main
     MISRC case) fed the unreadable file to SoX pass 1 → every such cut failed.
   - Probe schema bug: non-RF audio files carrying numeric RF tags (written by
     our own cut-tag rewrite) were misread as early-schema (×1000 → 4,000,000
     instead of 4,000, is_rf flipped true).
4. Fixes in core/src/chop.rs:
   - `run_sox_child` gained an optional stdin feeder (writer thread; feed
     errors fail the cut instead of masking as EOF; cancellation kills the
     child and the writer gets EPIPE).
   - 12-bit FLAC sources with conversions: SoX gets the cut as raw s16 on
     stdin (`-t s16 -r <header_rate> -c <ch> -`) — claxon decode, ×16
     left-justified (byte-identical to the equivalent 16-bit FLAC). All
     conversions (12/16/8/6-bit, rate, sinc) work from that stream.
   - Plain trims of 12-bit sources (no conversion) skip SoX entirely:
     `chop_12bit_source_pure` = claxon decode → trim → enc12 ("keep source
     precision" must yield true 12-bit, which SoX cannot write).
   - static-sox backend: 12-bit FLAC inputs rejected with a clear message
     (libSoX has the same limitation).
5. Fixes in core/src/probe.rs: RF tag schema logic (and the payload sanity
   checks) gated on is_rf — genuine RF files never carry audio-rate headers,
   so the header classification wins and non-RF cuts keep exact counts.
   Sanity-2 uncompressed estimate now uses the exact bit depth (12-bit = 1.5
   B/sample; integer division rounded to 1 and false-tripped the /1000
   rescale). Verified the later-schema semantics against the actual MISRC-GUI
   writer (gui_record.c `gui_record_finalize_flac_metadata`):
   RF_TOTAL_SAMPLES = the raw on-disk encoder count, STREAMINFO patched to
   /1000 kHz-domain — the probe's as-is later-schema handling was already
   correct and stays.
6. Fix in core/src/enc12.rs: STREAMINFO min=max=4096 (canonical fixed-
   blocksize; a min<max made `flac -t` warn "sample or frame number does not
   increase correctly").
7. `cargo test` — all green: 112 core + 6 ffi_plan + 9 roundtrip12.
8. `cmake --build build` — GUI rebuilt against the updated core (release
   staticlib). No errors.
9. CLI smoke on real fixtures (/tmp/fc_smoke): --version; --probe text+JSON
   (stable snake_case, all fields); cuts in seconds mode, --units samples
   mode (identical windows), 16→12 (--bits 12), full-length 12-bit source cut
   (stdin feed), pure-trim 12-bit keep-precision. ffprobe verified every
   output (s16 (12 bit) / bits_per_raw_sample=12 where expected).
   - **BUG FOUND by smoke:** full-length 12-bit source cut produced 8192 of
     10000 samples — the stdin feeder returned on end-of-stream BEFORE
     flushing the final partial 4096-sample batch. The integration tests
     missed it (windows never reached the stream tail). Fixed (flush before
     EOF) + regression test `twelve_bit_source_full_length_conversion_keeps_every_sample`.
   - Rebuilt + rerun: 10000/10000 samples, no trim warning.
10. readme.md: added "Command-line usage" section (all commands, units
    semantics, exit codes, probe JSON, --gui pre-load) + "True 12-bit FLAC
    output" section documenting the SoX read/write limitations and the
    stdin-feed workaround. enc12 module docs note the same.

## Status / not yet validated

- Core: all tests green; 12-bit pipeline validated sample-exact via claxon,
  flac -t, flac -d→WAV, ffprobe.
- CLI smoke: all paths green on synthetic fixtures (16-bit 48 kHz + true
  12-bit). NOT yet validated on a real multi-GB MISRC capture in this session.
- GUI: builds + links; --gui pre-load and the 12-bit selector have NOT been
  confirmed interactively in the running GUI (per the user-interactable rule)
  — awaiting user confirmation.

## Session 2026-09-28 ~04:30 UTC — GUI pre-load verified + Apply Template bug fix

1. GUI pre-load interactively CONFIRMED by user (instance: `flac-chop --gui
   /tmp/fc_smoke/src16.flac --in 0.25 --out 1.5`): file loads with correct
   metadata; IN=0.25 / OUT=1.5 markers set.
2. **BUG REPORTED by user:** "Apply Template button does not load a template
   with a file loaded in."
3. Root cause (hard data):
   - `probe_cli /tmp/fc_smoke/src16.flac` → `is_rf=false` (no RF tags on the
     fixture; real_rate 48000 = header rate).
   - `gui/mainwindow.cpp` `setMetaEnabled()`: template button gated
     `editable && m_probe.is_rf` → disabled (grey) for non-RF FLAC.
   - `applyTemplate()` silently returned for non-RF; core
     `rf_template_non_rf_is_empty` test documents non-RF → empty RF tag set
     (deliberate: RF totals for 48 kHz audio would be nonsense).
4. Fix in `gui/mainwindow.cpp` (no core change):
   - Template button now enabled for ANY loaded FLAC (`setEnabled(editable)`).
   - `applyTemplate()` gate now only needs a loaded FLAC; non-RF files get
     the 5 blank ingest rows (PROJECT/TAPE_ID/OPERATOR/LOCATION/NOTES); RF
     captures additionally get the computed RF tags as before.
   - Visible status on gate failure ("Apply Template needs a loaded FLAC
     file.") and on zero additions ("all standard tags already present").
   - Tooltip updated to say RF tags are for RF captures, ingest rows for any
     FLAC.
5. `cmake --build /home/harry/FLAC-Chop/build --target flac-chop` — OK.
6. GUI restarted with the fixed binary, same pre-load args (new PID 1112518).
   AWAITING user interactive confirmation of Apply Template on src16.flac
   (expect: button enabled, click adds 5 ingest rows, status message shown).
7. 10:30 UTC: relaunched GUI for testing (user request): `flac-chop --gui
   /tmp/fc_smoke/src16.flac --in 0.25 --out 1.5` — PID 3063197, fixed binary
   (built 04:32). Awaiting interactive Apply Template confirmation.
8. 10:32 UTC: Apply Template fix CONFIRMED working by user ("yes it adds
   fine"). Restore-point zip created per user rule:
   /home/harry/FLAC-Chop-restore-points/flac-chop_12bit-apply-template-fix_2026-09-28.zip
   (git HEAD archive + working-tree key files incl. the fix).
9. NEW FEATURE (user request): "addable fields box to add fields like tape
   speed and tape format etc." Implemented in Metadata Editor:
   - New row under the buttons: "Add field:" + editable QComboBox with
     presets TAPE_SPEED, TAPE_FORMAT, MACHINE, DATE_RECORDED, CONDITION,
     SOURCE (free text allowed) + "Add Field" button. Enter in the box also
     adds.
   - Typed spaces become underscores; name upper-cased; validated
     ^[A-Za-z0-9_]+$; duplicate names reported, not re-added. Inserts a blank
     row focused for value entry; only Save writes the file.
   - Enabled/disabled with the rest of the editor (loaded FLAC only).
   Files: gui/mainwindow.cpp, gui/mainwindow.h. Build OK (10:33).
10. 10:33 UTC: GUI restarted with the feature build, same pre-load args
   (PID 3072777). Awaiting interactive user confirmation.
