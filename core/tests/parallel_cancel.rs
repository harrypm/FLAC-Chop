//! Parallel batch processing coverage for `chop_with_options_and_cancel`
//! (the per-job cancel path behind the GUI's Batch Task tab / `fc_chop_ex`).
//!
//! Coverage:
//!  - a pre-set per-job cancel flag makes a chop return "cancelled by user"
//!    and write no output (both the SoX stdin-feed path and the pure-Rust
//!    12-bit trim path honour the flag before doing any work),
//!  - a pre-cancelled job running CONCURRENTLY with a normal job leaves the
//!    normal job untouched (the per-job flag is never reset by another chop
//!    starting — the exact cross-talk the legacy global flag had),
//!  - two concurrent 6-bit conversion jobs (the old pid-only temp-path
//!    collision) both succeed with the exact requested sample counts,
//!  - a cancel landing mid-flight never leaves an output that masquerades as
//!    a complete cut (either no output at all, or not the full count).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI32, Ordering};

use claxon::FlacReader;
use flac_chop_core::chop::{chop_with_options_and_cancel, sox_available, ChopOptions};
use flac_chop_core::enc12;
use flac_chop_core::probe;

fn require_sox() -> bool {
    if sox_available() {
        true
    } else {
        eprintln!("skipping: sox not available (chop requires it)");
        false
    }
}

struct Workspace(PathBuf);

impl Workspace {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("fcpar_it_{}", name));
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

/// Deterministic full-scale 12-bit pattern (every value in [-2048, 2047]).
fn test_data_12bit(n: usize) -> Vec<i16> {
    (0..n)
        .map(|i| ((((i as i64) * 37) % 4096) as i16) - 2048)
        .collect()
}

/// Write a synthetic true 12-bit mono FLAC via the in-tree encoder (no flac
/// CLI needed — only sox is required by these tests).
fn make_12bit_source(ws: &Workspace, name: &str, data: &[i16], rate: u32) -> PathBuf {
    let p = ws.path(name);
    enc12::encode_12bit_mono(data.iter().map(|&s| Ok(s)), rate, data.len() as u64, &p).unwrap();
    p
}

fn decode_samples(path: &Path) -> Vec<i16> {
    let mut reader = FlacReader::open(path).expect("open FLAC");
    reader.samples().map(|s| s.unwrap() as i16).collect()
}

fn assert_cancelled(r: &flac_chop_core::chop::ChopResult) {
    assert!(!r.ok, "a cancelled chop must not report ok (stderr: {})", r.stderr);
    assert!(
        r.stderr.contains("cancelled"),
        "a cancelled chop must say so, got: {}",
        r.stderr
    );
}

#[test]
fn per_job_cancel_flag_preset_cancels_without_output() {
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("preset");
    let data = test_data_12bit(8_192);
    let src = make_12bit_source(&ws, "src12.flac", &data, 48_000);

    // SoX path (12-bit source fed as s16 on stdin, 8-bit sink).
    let flag = AtomicI32::new(1);
    let out8 = ws.path("cut8.flac");
    let r = chop_with_options_and_cancel(
        src.to_str().unwrap(),
        out8.to_str().unwrap(),
        0,
        8_000,
        ChopOptions { output_bits: Some(8), ..Default::default() },
        Some(&flag),
    );
    assert_cancelled(&r);
    assert!(!out8.exists(), "a cancelled chop must not leave an output file");

    // Pure-Rust trim path (default options on a 12-bit source).
    let flag2 = AtomicI32::new(1);
    let out12 = ws.path("cut12.flac");
    let r2 = chop_with_options_and_cancel(
        src.to_str().unwrap(),
        out12.to_str().unwrap(),
        100,
        8_000,
        ChopOptions::default(),
        Some(&flag2),
    );
    assert_cancelled(&r2);
    assert!(!out12.exists(), "a cancelled pure trim must not leave an output file");
}

#[test]
fn pre_cancelled_job_does_not_disturb_concurrent_normal_job() {
    // The legacy global flag was reset at the start of every chop, so a job
    // starting could swallow another job's pending cancel, and one cancel
    // request killed every running job. Per-job flags must have neither
    // effect: job A (flag pre-set → cancels) runs concurrently with job B
    // (own flag, never set → must complete exactly).
    if !require_sox() {
        return;
    }
    let ws_a = Workspace::new("cross_a");
    let ws_b = Workspace::new("cross_b");
    let data = test_data_12bit(300_000);
    let src_a = make_12bit_source(&ws_a, "src_a.flac", &data, 48_000);
    let src_b = make_12bit_source(&ws_b, "src_b.flac", &data, 48_000);
    let out_a = ws_a.path("out_a.flac");
    let out_b = ws_b.path("out_b.flac");

    let flag_a = AtomicI32::new(1); // pre-set: A must cancel
    let flag_b = AtomicI32::new(0); // never set: B must complete

    let (ra, rb) = std::thread::scope(|scope| {
        let a = scope.spawn(|| {
            chop_with_options_and_cancel(
                src_a.to_str().unwrap(),
                out_a.to_str().unwrap(),
                0,
                300_000,
                ChopOptions { output_bits: Some(6), ..Default::default() },
                Some(&flag_a),
            )
        });
        let b = scope.spawn(|| {
            chop_with_options_and_cancel(
                src_b.to_str().unwrap(),
                out_b.to_str().unwrap(),
                0,
                300_000,
                ChopOptions { output_bits: Some(6), ..Default::default() },
                Some(&flag_b),
            )
        });
        (a.join().expect("job A thread"), b.join().expect("job B thread"))
    });

    // A: cancelled, no output — its own flag was honoured and B's run never
    // reset it (the legacy global-flag cross-talk).
    assert_cancelled(&ra);
    assert!(!out_a.exists(), "job A (pre-cancelled) must not leave an output");
    assert!(rb.ok, "job B must not be disturbed by job A's cancel: {}", rb.stderr);
    // B: a full, valid 6-bit cut despite A being cancelled mid-flight.
    let p = probe::probe(&out_b);
    assert!(p.ok, "job B must succeed: {}", p.error);
    assert_eq!(p.bits_per_sample, 8, "6-bit profile stores in an 8-bit container");
    assert!(p.total_samples_known);
    assert_eq!(p.total_samples, 300_000, "job B must hold every requested sample");
    let decoded = decode_samples(&out_b);
    assert_eq!(decoded.len(), 300_000);
    // 6-bit grid: every value is 4·round(v/4) — except the positive clamp
    // ceiling: the ×4 rescale of a full-scale pass-1 value (32) hits 128 and
    // sox clamps to the 8-bit max 127. The ramp data includes full scale.
    assert!(
        decoded.iter().all(|&v| v % 4 == 0 || v == 127),
        "the 6-bit grid output must be multiples of 4 (or the 127 clamp ceiling)"
    );
}

#[test]
fn concurrent_six_bit_jobs_do_not_collide() {
    // Regression shape for the temp-file collision: both jobs stage their
    // 6-bit pass-1 output in a temp FLAC. With the old pid-only temp names,
    // two jobs overlapping in one process wrote the SAME temp file and
    // corrupted each other. Both jobs must now complete exactly.
    if !require_sox() {
        return;
    }
    let ws1 = Workspace::new("par1");
    let ws2 = Workspace::new("par2");
    let d1 = test_data_12bit(2_000_000);
    let d2 = test_data_12bit(2_000_000);
    let src1 = make_12bit_source(&ws1, "src1.flac", &d1, 48_000);
    let src2 = make_12bit_source(&ws2, "src2.flac", &d2, 48_000);
    let out1 = ws1.path("out1.flac");
    let out2 = ws2.path("out2.flac");

    let f1 = AtomicI32::new(0);
    let f2 = AtomicI32::new(0);

    let (r1, r2) = std::thread::scope(|scope| {
        let h1 = scope.spawn(|| {
            chop_with_options_and_cancel(
                src1.to_str().unwrap(),
                out1.to_str().unwrap(),
                0,
                2_000_000,
                ChopOptions { output_bits: Some(6), ..Default::default() },
                Some(&f1),
            )
        });
        let h2 = scope.spawn(|| {
            chop_with_options_and_cancel(
                src2.to_str().unwrap(),
                out2.to_str().unwrap(),
                0,
                2_000_000,
                ChopOptions { output_bits: Some(6), ..Default::default() },
                Some(&f2),
            )
        });
        (h1.join().expect("job 1 thread"), h2.join().expect("job 2 thread"))
    });

    assert!(r1.ok, "concurrent job 1 failed: {}", r1.stderr);
    assert!(r2.ok, "concurrent job 2 failed: {}", r2.stderr);

    for (out, n) in [(&out1, 2_000_000u64), (&out2, 2_000_000u64)] {
        let p = probe::probe(out);
        assert!(p.ok, "probe failed: {}", p.error);
        assert!(p.total_samples_known);
        assert_eq!(
            p.total_samples, n,
            "concurrent jobs must each hold every requested sample (temp collision would corrupt this)"
        );
        assert!(
            decode_samples(out).iter().all(|&v| v % 4 == 0 || v == 127),
            "6-bit grid (multiples of 4, or the 127 clamp ceiling)"
        );
    }
}

#[test]
fn cancel_mid_flight_never_fakes_a_complete_cut() {
    // A cancel landing while a job runs must never leave behind an output that
    // looks like a successful complete cut: either the job already finished
    // (ok + exact count) or it was cancelled (no output / not the full count).
    if !require_sox() {
        return;
    }
    let ws = Workspace::new("midflight");
    let data = test_data_12bit(4_000_000);
    let src = make_12bit_source(&ws, "src.flac", &data, 48_000);
    let out = ws.path("out.flac");
    let flag = AtomicI32::new(0);

    let r = std::thread::scope(|scope| {
        let handle = scope.spawn(|| {
            chop_with_options_and_cancel(
                src.to_str().unwrap(),
                out.to_str().unwrap(),
                0,
                4_000_000,
                ChopOptions { output_bits: Some(6), ..Default::default() },
                Some(&flag),
            )
        });
        // Give the job a moment to get mid-flight, then request cancellation
        // from this (owning) thread.
        std::thread::sleep(std::time::Duration::from_millis(100));
        flag.store(1, Ordering::Relaxed);
        handle.join().expect("job thread")
    });

    if r.ok {
        // The job finished before the cancel landed (very fast machine):
        // then the output must be a complete, exact cut.
        let p = probe::probe(&out);
        assert!(p.ok, "probe failed: {}", p.error);
        assert_eq!(p.total_samples, 4_000_000, "an ok result must be the full cut");
    } else {
        assert_cancelled(&r);
        if out.exists() {
            // A partial file may exist if the cancel landed inside pass 2;
            // it must never parse as the complete requested cut.
            let p = probe::probe(&out);
            assert!(
                !p.ok || !p.total_samples_known || p.total_samples != 4_000_000,
                "a cancelled job must not leave a complete-looking output"
            );
        }
    }
}
