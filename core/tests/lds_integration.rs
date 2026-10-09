//! Integration: packed 10-bit `.lds` inputs through the real probe + cutter.
//!
//! Fixture generation needs no external tools (`flac_chop_core::lds::pack_group`
//! builds the `.lds` bytes directly). All SoX-driven cuts require a `sox`
//! executable; those tests skip with a note when none is available (the same
//! convention as roundtrip12 / parallel_cancel).
//!
//! Every exactness assertion is derived from the reference ld-lds-converter
//! semantics (tbc-tools src/ld-lds-converter/dataconverter.cpp): the unpacked
//! stream is `(value - 512) * 64` s16, mono, at the /1000-convention header
//! rate (a 20 MSPS `.lds` reads as a 20000 Hz stream, exactly what that tool
//! writes for `--sample-rate 20000`).

use flac_chop_core::chop::{
    chop_with_options, chop_with_options_and_cancel, sox_available, ChopOptions,
};
use flac_chop_core::lds::pack_group;
use flac_chop_core::probe;

fn temp(name: &str) -> std::path::PathBuf {
    std::env::temp_dir().join(name)
}

/// Pack `samples` into a `.lds` file; returns its path. All values are kept on
/// the 10-bit grid (multiples of 64) so the round-trip is byte-exact.
fn write_lds(name: &str, samples: &[i16]) -> std::path::PathBuf {
    let mut bytes = Vec::new();
    for g in samples.chunks(4) {
        bytes.extend_from_slice(&pack_group(&[g[0], g[1], g[2], g[3]]));
    }
    let p = temp(name);
    std::fs::write(&p, &bytes).unwrap();
    p
}

/// Ramp of on-grid samples: sample i -> ((i % 1024) - 512) * 64.
fn ramp(n: usize) -> Vec<i16> {
    (0..n)
        .map(|i| (((i % 1024) as i32) - 512) as i16 * 64)
        .collect()
}

fn opts_rf() -> ChopOptions {
    ChopOptions {
        is_rf: true,
        ..Default::default()
    }
}

/// Run a cut with the output in its OWN per-test directory, so the leftover
/// temp scan in a test never sees another (concurrently running) test's
/// in-flight window temp — the temp s16 lands beside the output file.
fn cut(in_path: &std::path::Path, out_name: &str, start: u64, len: u64, opts: ChopOptions) -> (flac_chop_core::chop::ChopResult, std::path::PathBuf) {
    let dir = temp(&format!(
        "fc_lds_it_{}",
        out_name.trim_end_matches(".flac")
    ));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let out = dir.join(out_name);
    // A per-job cancel flag (the production batch/sync model): these cuts
    // never touch the global pending output comments, so a concurrently
    // running test's chop can never steal the pending set that another
    // test's single-cut (Global) chop should consume — the tests run in
    // parallel threads of ONE process and share that global.
    let cancel = std::sync::atomic::AtomicI32::new(0);
    let r = chop_with_options_and_cancel(
        &in_path.to_string_lossy(),
        &out.to_string_lossy(),
        start,
        len,
        opts,
        Some(&cancel),
    );
    (r, out)
}

fn read_flac(path: &std::path::Path) -> (claxon::metadata::StreamInfo, Vec<i32>) {
    let mut reader = claxon::FlacReader::open(path).unwrap();
    let si = reader.streaminfo();
    let samples: Vec<i32> = reader.samples().map(|s| s.unwrap()).collect();
    (si, samples)
}

#[test]
fn lds_probe_reports_exact_totals_and_rate() {
    // 20_000 samples = 5000 groups, group-aligned; the 20msps name hint pins
    // the rate (no default-rate warning, no trailing-byte warning).
    let samples = ramp(20_000);
    let p = write_lds("fc_lds_probe_20msps.lds", &samples);
    let r = probe::probe(&p);
    assert!(r.ok, "{}", r.error);
    assert_eq!(r.format, probe::InputFormat::Lds);
    assert_eq!(r.total_samples, 20_000);
    assert!(r.total_samples_known);
    assert_eq!(r.bits_per_sample, 10);
    assert_eq!(r.channels, 1);
    assert!(r.is_rf);
    assert!((r.real_rate_hz - 20_000_000.0).abs() < 1e-6);
    assert_eq!(r.header_sample_rate, 20_000);
    assert!(r.warnings.is_empty(), "{}", r.warnings);
    let _ = std::fs::remove_file(&p);
}

#[test]
fn lds_full_cut_roundtrips_the_reference_scaling() {
    if !sox_available() {
        eprintln!("skipping: sox not available (cut requires it)");
        return;
    }
    let samples = ramp(20_000);
    let in_path = write_lds("fc_lds_full_20msps.lds", &samples);
    let (r, out) = cut(&in_path, "fc_lds_full_out.flac", 0, 20_000, opts_rf());
    assert!(r.ok, "{}", r.stderr);
    assert!(r.stderr.contains("packed .lds input"), "{}", r.stderr);

    // The output is a native FLAC whose samples are EXACTLY the unpacked
    // (value - 512) * 64 stream — byte-for-byte what the reference
    // ld-lds-converter writes for the same input — 16-bit mono at the
    // /1000-convention header rate (20000 Hz).
    let (si, got) = read_flac(&out);
    assert_eq!((si.channels, si.bits_per_sample, si.sample_rate), (1, 16, 20_000));
    assert_eq!(si.samples, Some(20_000));
    assert_eq!(got.len(), samples.len());
    for (i, (g, w)) in got.iter().zip(samples.iter()).enumerate() {
        assert_eq!(*g, i32::from(*w), "sample {i}");
    }

    // The window temp s16 (placed beside the OUTPUT file) must be gone. The
    // output lives in a per-test directory, so this scan cannot see another
    // concurrently running test's in-flight temp.
    let leftovers: Vec<_> = std::fs::read_dir(out.parent().unwrap())
        .unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().contains("flac-chop-ldssrc"))
        .collect();
    assert!(leftovers.is_empty(), "lds temp s16 not cleaned up");
    let _ = std::fs::remove_file(&in_path);
    let _ = std::fs::remove_dir_all(out.parent().unwrap());
}

#[test]
fn lds_partial_cut_at_a_non_aligned_start_is_sample_exact() {
    if !sox_available() {
        eprintln!("skipping: sox not available (cut requires it)");
        return;
    }
    let samples = ramp(20_000);
    let in_path = write_lds("fc_lds_part_20msps.lds", &samples);
    // Start at sample 5001 (group 1250, skip 1): the unpack window starts at
    // sample 5000 and the inner trim drops the 1 leading sample — the cut is
    // sample-exact even though the packing groups don't align to it.
    let (r, out) = cut(&in_path, "fc_lds_part_out.flac", 5001, 3777, opts_rf());
    assert!(r.ok, "{}", r.stderr);
    let (_, got) = read_flac(&out);
    assert_eq!(got.len(), 3777);
    for (i, g) in got.iter().enumerate() {
        assert_eq!(*g, i32::from(samples[5001 + i]), "sample {i}");
    }
    let _ = std::fs::remove_file(&in_path);
    let _ = std::fs::remove_file(&out);
}

#[test]
fn lds_cut_to_true_12bit_flac_is_exact() {
    if !sox_available() {
        eprintln!("skipping: sox not available (cut requires it)");
        return;
    }
    let samples = ramp(20_000);
    let in_path = write_lds("fc_lds_12bit_20msps.lds", &samples);
    let mut o = opts_rf();
    o.output_bits = Some(12);
    let (r, out) = cut(&in_path, "fc_lds_12bit_out.flac", 0, 20_000, o);
    assert!(r.ok, "{}", r.stderr);
    // True 12-bit FLAC (STREAMINFO bps = 12, the MISRC standard), and the
    // samples are the unpacked s16 arithmetic-shifted >>4 — exact on the
    // 10-bit grid ((value - 512) * 64 >> 4 == (value - 512) * 4).
    let (si, got) = read_flac(&out);
    assert_eq!((si.channels, si.bits_per_sample, si.sample_rate), (1, 12, 20_000));
    assert_eq!(got.len(), 20_000);
    for (i, g) in got.iter().enumerate() {
        assert_eq!(*g, i32::from(samples[i]) >> 4, "sample {i}");
    }
    let _ = std::fs::remove_file(&in_path);
    let _ = std::fs::remove_file(&out);
}

#[test]
fn lds_cut_embeds_pending_output_comments() {
    if !sox_available() {
        eprintln!("skipping: sox not available (cut requires it)");
        return;
    }
    // The GUI's pre-conversion metadata editor: rows authored while a
    // (tag-less) .lds is loaded are set right before Process and embedded
    // into the converted output by the post-cut tag rewrite, alongside the
    // core's own RF numeric tags (which win over an extra with the same key).
    let samples = ramp(20_000);
    let in_path = write_lds("fc_lds_meta_20msps.lds", &samples);
    flac_chop_core::chop::set_pending_output_comments(vec![
        "DATE_RECORDED=2022-12-11 00:00:34".to_string(),
        "PROJECT=demo".to_string(),
        "RF_TOTAL_SAMPLES=999999".to_string(), // owned: must be overridden
    ]);
    // A single-cut (Global-cancel) chop — the GUI's Process path that owns
    // and consumes the pending set (the `cut` helper uses per-job flags,
    // which never take it).
    let dir = temp("fc_lds_it_meta");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let out = dir.join("fc_lds_meta_out.flac");
    let r = chop_with_options(
        &in_path.to_string_lossy(),
        &out.to_string_lossy(),
        0,
        20_000,
        opts_rf(),
    );
    assert!(r.ok, "{}", r.stderr);
    let (_, comments) = flac_chop_core::tags::read_all_comments(&out).unwrap();
    let get = |k: &str| {
        comments
            .iter()
            .find(|(ck, _)| ck.eq_ignore_ascii_case(k))
            .map(|(_, v)| v.clone())
    };
    assert_eq!(get("DATE_RECORDED").as_deref(), Some("2022-12-11 00:00:34"));
    assert_eq!(get("PROJECT").as_deref(), Some("demo"));
    // The core's RF numeric tags win over the extras and are exact for the
    // cut: 20_000 samples, 20 MSPS real rate (20000 Hz /1000 header).
    assert_eq!(get("RF_TOTAL_SAMPLES").as_deref(), Some("20000"));
    assert_eq!(get("RF_SAMPLE_RATE").as_deref(), Some("20000000"));
    assert_eq!(get("RF_SAMPLE_RATE_KHZ").as_deref(), Some("20000"));
    let _ = std::fs::remove_file(&in_path);
    let _ = std::fs::remove_dir_all(out.parent().unwrap());
}

#[test]
fn lds_cut_with_rate_and_bits_conversion_keeps_the_math() {
    if !sox_available() {
        eprintln!("skipping: sox not available (cut requires it)");
        return;
    }
    // 20 MSPS input, 20_000 samples (1 ms of RF): convert to 10 MSPS 8-bit —
    // exactly the GUI's "10 MSPS (HiFi FM)" + 8-bit batch settings.
    let samples = ramp(20_000);
    let in_path = write_lds("fc_lds_conv_20msps.lds", &samples);
    let mut o = opts_rf();
    o.output_rate_hz = Some(10_000);
    o.output_bits = Some(8);
    o.basic_rf_filter = false; // keep the sample math deterministic here
    let (r, out) = cut(&in_path, "fc_lds_conv_out.flac", 0, 20_000, o);
    assert!(r.ok, "{}", r.stderr);
    let (si, got) = read_flac(&out);
    assert_eq!((si.channels, si.bits_per_sample, si.sample_rate), (1, 8, 10_000));
    assert_eq!(got.len(), 10_000); // 20k stream samples resampled /2
    let _ = std::fs::remove_file(&in_path);
    let _ = std::fs::remove_file(&out);
}
