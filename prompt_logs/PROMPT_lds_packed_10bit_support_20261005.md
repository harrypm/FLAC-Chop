# PROMPT: packed 10-bit `.lds` input support (ld-lds-converter integration, 2026-10-05)

## Inputs (user prompts, in order)
1. "Now to fix and intergrate ld-lds-converter code from tbc-tools as it does not support .lds 10-bit packed files... it should do though."
2. "commit and push current fixes after the intergration fixes"
3. "C:\Users\Harry\Desktop\RF-Sample_2022-12-11_00-00-34.lds - faild" (real test data; the failure was the STALE dist-vfix exe predating the integration — see Verification)

## Reference implementation studied
- `C:\Users\Harry\tbc-tools\src\ld-lds-converter\dataconverter.cpp` (`packFile`/`unpackFile`): the DdD/ld-decode packing — 4 samples per 5 bytes, 10-bit words (centre 512), unpack scaling `s16 = (value - 512) * 64`, pack quantization `(s16 / 64) + 512` (truncating toward zero), default `--sample-rate 40000` (= 40 MSPS /1000 convention). Re-derived in Rust from the bit layout; no code copied.
- Reference binary used for A/B verification: `C:\Users\Harry\tbc-tools\build\bin\ld-lds-converter.exe`.

## Changes made
- `core/src/lds.rs` (new): `unpack_group`/`pack_group` (the reference layout + scaling), `total_samples`, and `unpack_window` — a sample-exact window unpack that reads ONLY the packing groups covering the cut `[start, start+len)` (seek to `(start/4)*5`; the 0..3 leading group-mates ARE included so the inner SoX `trim <skip>s <len>s` has the full window), writes a temp s16 (beside the output, like the `.ldf` remux), polls cancel every 4096 groups.
- `core/src/probe.rs`: `InputFormat::Lds` (FFI code 7, appended last), `.lds` sniffing (was a hard refusal), `probe_lds` — exact totals `(size/5)*4` (+ trailing-byte warning), bits=10, mono, is_rf, rate from an `<n>msps` filename hint or the 40 MSPS ld-decode default WITH a warning (ld-lds-converter's own default; unlike u8/u16 raw the format has a canonical rate).
- `core/src/chop.rs`: `.lds` dispatch in `chop_with_options_and_cancel` → `chop_lds_input` (window unpack → normal SoX/12-bit pipeline on the temp s16 with explicit S16 hints; works in BOTH backends); defensive `Lds` arms in `sox_input_args`/`in_hints_for`.
- `core/src/ffi.rs` + `gui/flacchop.h`: format-code docs now include 7=lds.
- GUI: format labels (`Packed .lds (10-bit)`, `packed 10-bit (rate from filename)`), `*.lds` in the open/add file filters (mainwindow + batchtab), CLI `--probe` JSON/text format names.
- `gui/CMakeLists.txt`: `lds.rs` in `RUST_SOURCES`.
- `readme.md`: `.lds` row in the input-format table + a feature bullet (the "clear errors" bullet no longer lists .lds as unsupported).
- `core/tests/lds_integration.rs` (new): probe exactness; full cut == the reference scaling byte-for-byte; non-aligned-start partial cut sample-exact; true 12-bit output exact (`>>4`); rate+bits conversion math. Cut tests follow the sox_available() skip convention (CI's sox-less cargo-tests job skips them).

## Bugs found by real-data testing (and fixed)
1. Off-by-one partial cut: the first unpack_window dropped the leading `skip` group-mates from the temp, so the inner `trim <skip>s <len>s` window extended 1 sample past the temp end → 1-sample-short cuts + sox "Last 1 position(s) not reached" (reproduced on a real 10msps cut: expected 11,111,110 bytes, got 11,111,108). Fixed: include the skip samples in the temp (the inner trim drops them).
2. Integration-test blind spot: the cargo-test login shell drops the Windows PATH, so `sox_available()` was false and the cut tests silently SKIPPED ("5 passed" was hollow). Local runs must set `FLAC_CHOP_SOX=C:\Program Files\DecodeTools\sox.exe` to make them actually cut. (CI's cargo-tests job has no sox — skip is the designed behaviour there.)
3. Test-isolation race: the leftover-temp scan saw other concurrently-running tests' in-flight window temps → per-test output dirs.
4. The user's "faild" was the STALE `dist-vfix` exe (pre-integration refusal text). Refreshed with the fixed build.

## Verification (commands + hard results)
- cargo test --release (FLAC_CHOP_SOX set so every cut test executes): 146 lib + 6 ffi_plan + 5 lds_integration + 4 parallel_cancel + 11 roundtrip12 = 172 passed, 0 failed. (lds.rs unit tests: golden hand-computed vectors incl. a mixed-words group, pack/unpack roundtrip, truncation semantics, window math, EOF clamps, cancel.)
- Real capture A/B (source: `Desktop\decode\capture_..._8-bit_10msps_chB.flac`, 2 real seconds → s16 → packed to `.lds` BY THE REFERENCE TOOL):
  - reference `-u --flac --sample-rate 10000` vs flac-chop whole-file cut → decoded s16 byte-compare: **0 diffs / 40,000,000 bytes**
  - non-aligned partial cut (start 7,777,777 len 5,555,555): **byte-exact** vs the reference stream (head/tail windows + every 101st byte)
- The user's own file `RF-Sample_2022-12-11_00-00-34.lds` (251,658,240 bytes = 201,326,592 samples, group-aligned):
  - probe: format=lds (code 7), bits 10, mono, totals exact, 40 MSPS default + warning (no msps hint in the name)
  - whole-file cut vs the reference tool's own unpack: **SHA256-identical decoded s16** (`0B513122...55EA5D` both)
  - non-aligned partial cut (start 123,456,789 len 77,654,321): **byte-exact** vs the reference stream
  - 1-second cut from the refreshed dist-vfix portable folder: exit 0
- GUI left running with the file preloaded for the user's own eyes.

## Still awaiting user confirmation (on their screen)
- The GUI with `RF-Sample_...lds` loaded: file info (Packed .lds, 10-bit, totals/rate labels), the ⚠ default-rate warning, and a Process cut.
- If this capture is NOT 40 MSPS: rename the file (e.g. `..._10msps.lds`) or the duration/labels assume the default.
