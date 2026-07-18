//! Integration test for the C ABI `fc_plan` against the verified Tape_15
//! numbers. fc_plan now takes the already-resolved real_rate_hz directly
//! (the probe resolves header*1000 for RF / header for audio).
//!  - real rate 20,000,000 Hz (20 MSPS), total 49_528_274_364
//!  - 10 real seconds at 20 MSPS must be exactly 200,000,000 samples.

#![allow(clippy::unnecessary_cast)]

use flac_chop_core::ffi::{fc_plan, FcPlan};

/// Read a NUL-terminated fixed C char buffer back into a Rust String.
fn cbuf_to_string(buf: &[std::os::raw::c_char]) -> String {
    let bytes: Vec<u8> = buf.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

#[test]
fn plan_10s_at_20msps_is_exactly_200m_samples() {
    let mut p = FcPlan::default();
    fc_plan(0.0, 10.0, 20_000_000.0, 49_528_274_364, 1, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 1);
    assert_eq!(p.start_samples, 0);
    assert_eq!(p.length_samples, 200_000_000, "10s @ 20MSPS must be 200,000,000 samples");
    assert_eq!(p.end_sample, 200_000_000);
    assert!((p.real_sample_rate_hz - 20_000_000.0).abs() < 1e-3);
    assert!((p.real_total_seconds - 2476.413_718_2).abs() < 1e-3);
}

#[test]
fn plan_start_past_eof_is_an_error_not_a_clamped_cut() {
    // 36000 s @ 20 MSPS = 720e9 samples, far past the 49.5e9-sample file.
    // As of the catch_unwind rewrite, a start at or past EOF is an explicit
    // error (ok=0) rather than silently clamping to a degenerate 1-sample cut
    // past the end (which would ask SoX to read beyond the file).
    let mut p = FcPlan::default();
    fc_plan(36000.0, 10.0, 20_000_000.0, 49_528_274_364, 1, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 0);
    assert!(
        cbuf_to_string(&p.error).contains("end of the file"),
        "got: {}",
        cbuf_to_string(&p.error)
    );
}

#[test]
fn plan_clamps_length_when_start_is_in_bounds_but_request_overruns() {
    // Start 1 s before EOF, request 10 s: length must clamp to the remaining
    // 1 s (20e6 samples), not error and not overrun.
    let total = 49_528_274_364u64;
    let start_sec = (total as f64 / 20_000_000.0) - 1.0; // 1 s before EOF
    let mut p = FcPlan::default();
    fc_plan(start_sec, 10.0, 20_000_000.0, total, 1, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 1, "{}", cbuf_to_string(&p.error));
    assert_eq!(p.start_samples, (start_sec * 20_000_000.0).round() as u64);
    // Remaining samples = total - start, which is ~20,000,000 (1 s).
    let remaining = total - p.start_samples;
    assert_eq!(p.length_samples, remaining);
    assert_eq!(p.end_sample, total);
}

#[test]
fn plan_audio_44100_used_as_is() {
    // 44.1 kHz audio: real_rate_hz = 44100 (not multiplied).
    let mut p = FcPlan::default();
    fc_plan(1.0, 1.0, 44_100.0, 0, 0, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 1);
    assert_eq!(p.start_samples, 44_100);
    assert_eq!(p.length_samples, 44_100);
    assert!((p.real_sample_rate_hz - 44_100.0).abs() < 1e-3);
}

#[test]
fn plan_rejects_zero_rate() {
    let mut p = FcPlan::default();
    fc_plan(0.0, 10.0, 0.0, 0, 0, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 0);
}

#[test]
fn plan_rejects_nonpositive_length() {
    let mut p = FcPlan::default();
    fc_plan(0.0, 0.0, 20_000_000.0, 0, 0, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 0);
}
