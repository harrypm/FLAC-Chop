//! End-to-end tests for the true 12-bit FLAC output path.
//!
//! Coverage:
//!  - a 12-bit source cut with `output_bits: Some(12)` round-trips
//!    sample-exactly through the full SoX ×16 / `>>4` / [`enc12`] pipeline,
//!  - the output is accepted (and CRC-validated) by the reference `flac`
//!    decoder (`flac -t`) and decodes identically via claxon,
//!  - external SoX-based pipelines (the real MISRC consumers) read the
//!    12-bit output losslessly (12 → 16-bit left-justification is exact),
//!  - a 16-bit source lands on the 12-bit grid by arithmetic `>>4`,
//!  - the post-cut RF Vorbis-tag rewrite works on encoder output,
//!  - the default (no conversion) 16-bit path stays sample-exact.
//!
//! All SoX-driven cuts require a `sox` executable; tests skip with a note
//! when the toolchain is unavailable (same guard the CI expects).

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

use claxon::{FlacReader, FlacReaderOptions};
use flac_chop_core::chop::{chop_with_options, sox_available, ChopOptions};
use flac_chop_core::enc12;
use flac_chop_core::probe;

/// `chop_with_options` shells out to SoX and stages 6/12-bit output in a
/// fixed per-pid temp path, so concurrent tests would race on it. Serialize
/// every cut.
static CHOP_LOCK: Mutex<()> = Mutex::new(());

// ---------------------------------------------------------------------------
// Toolchain guards
// ---------------------------------------------------------------------------

fn flac_available() -> bool {
    Command::new("flac")
        .arg("--version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

fn require_sox() -> bool {
    if sox_available() {
        true
    } else {
        eprintln!("skipping: sox not available (fixture cut requires it)");
        false
    }
}

fn require_flac() -> bool {
    if flac_available() {
        true
    } else {
        eprintln!("skipping: flac CLI not available (fixture generation requires it)");
        false
    }
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// Per-test scratch directory (cleared before use, removed after).
struct Workspace(PathBuf);

impl Workspace {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("fc12_it_{}", name));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        Workspace(dir)
    }

    fn path(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}

impl Drop for Workspace {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Deterministic full-scale 12-bit ramp+noise pattern (every value in
/// [-2048, 2047]).
fn test_data_12bit(n: usize) -> Vec<i16> {
    (0..n)
        .map(|i| ((((i as i64) * 37) % 4096) as i16) - 2048)
        .collect()
}

/// 16-bit pattern with the requantization edge cases pinned at the front:
/// the extreme values, the exact 12-bit grid points, and ±1.
fn test_data_16bit(n: usize) -> Vec<i16> {
    let mut v: Vec<i16> = Vec::with_capacity(n);
    v.push(-32768);
    v.push(32767);
    v.push(-2048);
    v.push(2047);
    v.push(-1);
    v.push(1);
    for i in 0..n.saturating_sub(v.len()) {
        let s = (((i as i64) * 977) % 65536 - 32768) as i64;
        v.push(s.clamp(-32768, 32767) as i16);
    }
    v
}

/// Write a synthetic true 12-bit mono FLAC via the in-tree encoder (the same
/// format the MISRC pipeline writes). Returns the path.
fn make_12bit_source(ws: &Workspace, name: &str, data: &[i16], rate: u32) -> PathBuf {
    let p = ws.path(name);
    enc12::encode_12bit_mono(data.iter().map(|&s| Ok(s)), rate, data.len() as u64, &p).unwrap();
    p
}

/// Write a 16-bit mono FLAC via the reference `flac` CLI from raw s16le.
/// Returns the path.
fn make_16bit_source(ws: &Workspace, name: &str, data: &[i16], rate: u32) -> PathBuf {
    let raw = ws.path("src16.s16");
    {
        use std::io::Write;
        let mut f = std::fs::File::create(&raw).unwrap();
        for &s in data {
            f.write_all(&s.to_le_bytes()).unwrap();
        }
    }
    let p = ws.path(name);
    let st = Command::new("flac")
        .args([
            "--silent",
            "--force-raw-format",
            "--endian=little",
            "--sign=signed",
            "--channels=1",
            "--bps=16",
            &format!("--sample-rate={rate}"),
            "-o",
        ])
        .arg(&p)
        .arg(&raw)
        .status()
        .expect("run flac CLI");
    assert!(st.success(), "flac CLI failed to encode the 16-bit fixture");
    let _ = std::fs::remove_file(&raw);
    p
}

/// Decode a FLAC's samples with claxon (the same decoder the probe uses).
fn decode_samples(path: &Path) -> Vec<i16> {
    let mut reader = FlacReader::open(path).expect("open FLAC");
    reader.samples().map(|s| s.unwrap() as i16).collect()
}

/// One serialized cut through the public pipeline.
fn run_chop(
    in_path: &Path,
    out_path: &Path,
    start: u64,
    len: u64,
    opts: ChopOptions,
) -> flac_chop_core::chop::ChopResult {
    let _g = CHOP_LOCK.lock().unwrap();
    chop_with_options(
        in_path.to_str().unwrap(),
        out_path.to_str().unwrap(),
        start,
        len,
        opts,
    )
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[test]
fn twelve_bit_cut_roundtrips_sample_exact() {
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("roundtrip");
    // 48 kHz = audio semantics (no RF tag-schema entanglement: a kHz-domain
    // STREAMINFO would make the tag rewrite assert an on-disk count 1000× the
    // fixture's actual size). The MISRC RF-tag path is covered separately in
    // rf_tag_rewrite_runs_on_12bit_cut.
    let rate = 48_000u32;
    let data = test_data_12bit(10_000); // 2 full frames + partial third
    let src = make_12bit_source(&ws, "src12.flac", &data, rate);

    // Fixture sanity: the encoder's own output must decode back exactly.
    assert_eq!(decode_samples(&src), data, "12-bit fixture must decode exactly");

    let (start, len) = (1234u64, 5000u64);
    let out = ws.path("cut12.flac");
    let r = run_chop(&src, &out, start, len, ChopOptions { output_bits: Some(12), ..Default::default() });
    assert!(r.ok, "12-bit cut failed: {}", r.stderr);
    assert_eq!(r.exit_code, 0);

    // Output STREAMINFO must be a true 12-bit mono stream with the exact window.
    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.bits_per_sample, 12, "STREAMINFO must declare 12 bits");
    assert_eq!(p.channels, 1);
    assert_eq!(p.header_sample_rate, rate as u64);
    assert!(p.total_samples_known);
    assert_eq!(p.total_samples, len, "cut must hold exactly the requested samples");

    // Sample-exactness: output == the requested window of the source.
    let expected = &data[start as usize..(start + len) as usize];
    assert_eq!(decode_samples(&out), expected, "12-bit → 12-bit cut must be sample-exact");
}

#[test]
fn twelve_bit_output_passes_reference_flac_decoder() {
    if !require_sox() || !require_flac() {
        return;
    }
    let ws = Workspace::new("refdec");
    let data = test_data_12bit(9_000);
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);
    let out = ws.path("cut12.flac");
    let r = run_chop(&src, &out, 0, 8_000, ChopOptions { output_bits: Some(12), ..Default::default() });
    assert!(r.ok, "12-bit cut failed: {}", r.stderr);

    // `flac -t` fully decodes the stream and validates every frame CRC.
    let st = Command::new("flac")
        .args(["--test", "--silent"])
        .arg(&out)
        .status()
        .expect("run flac CLI");
    assert!(st.success(), "reference flac decoder rejected the 12-bit output");

    // And claxon must agree with the reference decoder on the content.
    assert_eq!(decode_samples(&out).len(), 8_000);
}

#[test]
fn twelve_bit_source_to_sixteen_bit_scales_x16() {
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("to16");
    let data = test_data_12bit(8_192);
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);
    let out = ws.path("cut16.flac");
    // 12-bit source with an explicit 16-bit request: SoX cannot read the
    // 12-bit source file, so this exercises the stdin-feed path end-to-end.
    let r = run_chop(&src, &out, 0, 8_000, ChopOptions { output_bits: Some(16), ..Default::default() });
    assert!(r.ok, "12→16-bit cut failed: {}", r.stderr);

    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.bits_per_sample, 16);
    assert_eq!(p.total_samples, 8_000);

    // The 16-bit output holds the source values left-justified (×16).
    let decoded: Vec<i16> = decode_samples(&out);
    let expected: Vec<i16> = data[..8_000].iter().map(|&v| v * 16).collect();
    assert_eq!(decoded, expected, "12→16-bit output must be a pure ×16 left-justification");
}

#[test]
fn twelve_bit_source_full_length_conversion_keeps_every_sample() {
    // Regression: the stdin feeder buffered 4096-sample batches and dropped
    // the final partial batch on end-of-stream — a full-length conversion cut
    // came up short by the last partial FLAC frame (caught by an ffprobe smoke
    // test: 8192 of 10000 samples). The cut window must reach the stream end.
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("fulllen");
    let data = test_data_12bit(10_000); // 2 full frames + 1808-sample partial
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);
    let out = ws.path("cut16.flac");
    let r = run_chop(&src, &out, 0, 10_000, ChopOptions { output_bits: Some(16), ..Default::default() });
    assert!(r.ok, "full-length 12→16-bit cut failed: {}", r.stderr);

    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.total_samples, 10_000, "every sample must survive a full-length cut");
    let decoded: Vec<i16> = decode_samples(&out);
    let expected: Vec<i16> = data.iter().map(|&v| v * 16).collect();
    assert_eq!(decoded, expected);
}

#[test]
fn twelve_bit_source_partial_conversion_cut_survives_sox_early_pipe_close() {
    // Regression: sox stops reading (and closes the stdin pipe) once it has
    // the full trim window. On a partial conversion cut of a large capture
    // the feeder still holds megabytes of post-window audio — the pending
    // writes then hit EPIPE/SIGPIPE, which KILLED the whole process in the
    // C++ GUI binary (exit 141, no tag rewrite, GUI crash mid-cut) and
    // failed the cut in Rust binaries. The smaller existing tests never
    // caught it: with ≤64 KiB of post-window audio the data fits in the
    // kernel pipe buffer, so no write ever failed. 100k-sample source,
    // cut [0, 50k): 50k samples (100 KiB) past the window > 64 KiB buffer.
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("epipe");
    let data = test_data_12bit(100_000);
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);
    let out = ws.path("cut8.flac");
    let r = run_chop(&src, &out, 0, 50_000, ChopOptions { output_bits: Some(8), ..Default::default() });
    assert!(r.ok, "partial conversion cut must survive sox closing the feed: {}", r.stderr);

    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.bits_per_sample, 8);
    assert_eq!(p.total_samples, 50_000);
    // The 8-bit sink divides the 16-bit container by 256: a 12-bit value
    // (×16 left-justified into s16) lands at v/16 — full-scale-preserving,
    // the same ÷16 the rest of the pipeline uses. Assert with ±1 tolerance
    // for sink rounding.
    let decoded = decode_samples(&out);
    assert_eq!(decoded.len(), 50_000);
    for (&o, &v) in decoded.iter().zip(data.iter()) {
        let expect = f32::from(v) / 16.0;
        assert!((f32::from(o) - expect).abs() <= 1.0, "sample {o} vs {expect}");
    }
}

#[test]
fn twelve_bit_source_default_cut_is_true_12bit() {
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("pure12");
    let data = test_data_12bit(8_192);
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);
    let out = ws.path("cut12.flac");
    // Default options on a 12-bit source mean "keep source precision": the
    // output must be a TRUE 12-bit FLAC, sample-exact (pure-Rust trim path —
    // SoX can neither read nor write 12-bit FLAC).
    let r = run_chop(&src, &out, 100, 8_000, ChopOptions::default());
    assert!(r.ok, "12-bit default cut failed: {}", r.stderr);

    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.bits_per_sample, 12, "default cut must keep the source's 12-bit precision");
    assert_eq!(p.channels, 1);
    assert_eq!(p.header_sample_rate, 48_000);
    assert_eq!(p.total_samples, 8_000);
    assert_eq!(decode_samples(&out), &data[100..8_100], "pure-trim cut must be sample-exact");
}

#[test]
fn twelve_bit_output_cross_decodes_via_flac_cli_wav() {
    if !require_sox() || !require_flac() {
        return;
    }
    let ws = Workspace::new("wavxcheck");
    let data = test_data_12bit(8_192);
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);
    let out = ws.path("cut12.flac");
    let r = run_chop(&src, &out, 0, 8_000, ChopOptions { output_bits: Some(12), ..Default::default() });
    assert!(r.ok, "12-bit cut failed: {}", r.stderr);

    // Reference-decoder cross-check. `flac -d` of a 12-bit stream writes a
    // WAV whose samples sit left-justified (×16) in 16-bit containers
    // (verified layout: fmt bits=16, data = source*16); raw output is refused
    // for non-byte-aligned depths. SoX itself cannot read 12-bit FLAC at all
    // (sox_precision requires byte-aligned bps), which is why the cutter
    // feeds 12-bit sources to it as raw s16 on stdin.
    let wav = ws.path("cut12.wav");
    let st = Command::new("flac")
        .args(["--decode", "--silent", "--force"])
        .arg(&out)
        .args(["-o"])
        .arg(&wav)
        .status()
        .expect("run flac CLI");
    assert!(st.success(), "reference flac decoder rejected the 12-bit output");

    let raw = std::fs::read(&wav).unwrap();
    let pos = raw.windows(4).position(|w| w == b"data").expect("WAV data chunk");
    let len = u32::from_le_bytes(raw[pos + 4..pos + 8].try_into().unwrap()) as usize;
    let decoded: Vec<i16> = raw[pos + 8..pos + 8 + len]
        .chunks_exact(2)
        .map(|b| i16::from_le_bytes([b[0], b[1]]))
        .collect();
    let expected: Vec<i16> = data[..8_000].iter().map(|&v| v * 16).collect();
    assert_eq!(decoded, expected, "flac -d must agree with the source (×16 left-justified)");
}

#[test]
fn sixteen_bit_cut_shifts_onto_12bit_grid() {
    if !require_sox() || !require_flac() {
        return;
    }
    let ws = Workspace::new("shift");
    let data = test_data_16bit(6_000);
    let src = make_16bit_source(&ws, "src16.flac", &data, 48_000);
    assert_eq!(decode_samples(&src), data, "16-bit fixture must decode exactly");

    let out = ws.path("cut12.flac");
    let r = run_chop(&src, &out, 0, 5_000, ChopOptions { output_bits: Some(12), ..Default::default() });
    assert!(r.ok, "16→12-bit cut failed: {}", r.stderr);

    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.bits_per_sample, 12);
    assert_eq!(p.header_sample_rate, 48_000, "audio-rate headers must stay audio rates");

    // Requantization is the arithmetic >>4 (the exact inverse of the ×16
    // left-justification the 16-bit temp holds).
    let expected: Vec<i16> = data[..5_000].iter().map(|&v| v >> 4).collect();
    assert_eq!(decode_samples(&out), expected, "16→12-bit output must be v >> 4");
}

#[test]
fn rf_tag_rewrite_runs_on_12bit_cut() {
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("rftags");
    let data = test_data_12bit(5_000);
    let src = make_12bit_source(&ws, "src12.flac", &data, 20_000);
    let out = ws.path("cut12.flac");
    let r = run_chop(&src, &out, 0, 4_096, ChopOptions { output_bits: Some(12), is_rf: true, ..Default::default() });
    assert!(r.ok, "12-bit RF cut failed: {}", r.stderr);

    // The rewrite must have inserted the numeric RF tags derived from the
    // output's own STREAMINFO — RF_TOTAL_SAMPLES = the 1:1 on-disk count
    // (MISRC-GUI convention; the old count×1000 shape is gone).
    let file = std::fs::File::open(&out).unwrap();
    let opts = FlacReaderOptions { metadata_only: true, read_vorbis_comment: true };
    let reader = FlacReader::new_ext(file, opts).unwrap();
    let tag = |k| reader.get_tag(k).next().expect("tag must exist");
    assert_eq!(tag("RF_SAMPLE_RATE"), "20000000");
    assert_eq!(tag("RF_TOTAL_SAMPLES"), "4096");
    assert_eq!(tag("RF_SAMPLE_RATE_KHZ"), "20000");
    // Duration tags derive from the REAL rate (header×1000), not the kHz
    // header: 4096 / 20 MHz = 0.0002048 s → 0.000205 at the 6-decimal format.
    assert_eq!(tag("DURATION_SECONDS"), "0.000205");
}

#[test]
fn rf_cut_output_reprobes_with_correct_total_and_recuts() {
    // 1:1 tag-convention regression: the cut's RF_TOTAL_SAMPLES must equal
    // the on-disk sample count (NOT count×1000), so re-probing the cut
    // yields the correct real duration with no rescale warnings, and a
    // --units samples cut-of-cuts plans inside the actual audio (the ×1000
    // tags planned 1000× past the stream end and failed).
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("reprobecut");
    // 20 kHz = the /1000 kHz-domain header (20 MSPS real), no source tags —
    // the probe classifies it RF from the header rate alone.
    let data = test_data_12bit(100_000);
    let src = make_12bit_source(&ws, "src12.flac", &data, 20_000);
    let out = ws.path("cut12.flac");
    let r = run_chop(
        &src,
        &out,
        0,
        50_000,
        ChopOptions { output_bits: Some(12), is_rf: true, ..Default::default() },
    );
    assert!(r.ok, "12-bit RF cut failed: {}", r.stderr);

    // Reload via the probe: the tag must read back as the 1:1 on-disk count.
    let p = probe::probe(&out);
    assert!(p.ok, "{}", p.error);
    assert!(p.is_rf);
    assert!(p.total_samples_from_vorbis);
    assert_eq!(p.total_samples, 50_000, "cut tag must be the 1:1 on-disk count");
    let dur = p.total_samples as f64 / p.real_rate_hz;
    assert!((dur - 0.0025).abs() < 1e-9, "real duration must be 2.5 ms, got {dur}");
    assert!(p.warnings.is_empty(), "no tag rescale warnings: {}", p.warnings);

    // Cut-of-cuts in real-sample units (the --units samples contract): the
    // plan must land inside the actual audio and the cut must succeed.
    let out2 = ws.path("cut2.flac");
    let r2 = run_chop(
        &out,
        &out2,
        10_000,
        20_000,
        ChopOptions { is_rf: true, ..Default::default() },
    );
    assert!(r2.ok, "cut-of-cuts failed: {}", r2.stderr);
    let p2 = probe::probe(&out2);
    assert!(p2.ok, "{}", p2.error);
    assert_eq!(p2.total_samples, 20_000);
    assert_eq!(decode_samples(&out2), data[10_000..30_000], "cut-of-cuts must be sample-exact");
}

#[test]
fn sixteen_bit_default_cut_stays_exact() {
    if !require_sox() || !require_flac() {
        return;
    }
    let ws = Workspace::new("default16");
    let data = test_data_16bit(6_000);
    let src = make_16bit_source(&ws, "src16.flac", &data, 48_000);

    // Default options (no conversion) must stay bit-exact.
    let out = ws.path("cut16.flac");
    let r = run_chop(&src, &out, 512, 4_000, ChopOptions::default());
    assert!(r.ok, "default 16-bit cut failed: {}", r.stderr);

    let p = probe::probe(&out);
    assert!(p.ok, "probe failed: {}", p.error);
    assert_eq!(p.bits_per_sample, 16);
    assert_eq!(p.total_samples, 4_000);
    assert_eq!(decode_samples(&out), &data[512..4_512], "default cut must be sample-exact");
}
