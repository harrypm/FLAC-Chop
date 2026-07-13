//! Integration test for the C ABI `fc_plan` against the verified Tape_15
//! numbers. fc_plan now takes the already-resolved real_rate_hz directly
//! (the probe resolves header*1000 for RF / header for audio).
//!  - real rate 20,000,000 Hz (20 MSPS), total 49_528_274_364
//!  - 10 real seconds at 20 MSPS must be exactly 200,000,000 samples.

#![allow(clippy::unnecessary_cast)]

use flac_chop_core::ffi::{fc_plan, FcPlan};

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
fn plan_clamps_to_total_when_request_exceeds_file() {
    let mut p = FcPlan::default();
    fc_plan(36000.0, 10.0, 20_000_000.0, 49_528_274_364, 1, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 1);
    assert_eq!(p.start_samples, 49_528_274_364);
    assert_eq!(p.length_samples, 1);
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
