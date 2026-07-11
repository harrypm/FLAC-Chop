//! Integration test for the C ABI `fc_plan` against the verified Tape_15
//! numbers. This checks the FFI wrapper end-to-end (no GUI, no SoX run):
//!  - header rate 20000, msps 20.0, total 49_528_274_364 (all from soxi/probe_cli)
//!  - 10 real seconds at 20 MSPS must be exactly 200,000,000 samples.

#![allow(clippy::unnecessary_cast)]

use flac_chop_core::ffi::{fc_plan, FcPlan};

#[test]
fn plan_10s_at_20msps_is_exactly_200m_samples() {
    let mut p = FcPlan::default();
    fc_plan(0.0, 10.0, 20.0, 1, 20_000, 49_528_274_364, 1, &mut p as *mut FcPlan);
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
    fc_plan(36000.0, 10.0, 20.0, 1, 20_000, 49_528_274_364, 1, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 1);
    assert_eq!(p.start_samples, 49_528_274_364);
    assert_eq!(p.length_samples, 1);
}

#[test]
fn plan_uses_header_rate_when_msps_unknown() {
    let mut p = FcPlan::default();
    fc_plan(1.0, 1.0, 0.0, 0, 44_100, 0, 0, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 1);
    assert_eq!(p.start_samples, 44_100);
    assert_eq!(p.length_samples, 44_100);
    assert!((p.real_sample_rate_hz - 44_100.0).abs() < 1e-3);
}

#[test]
fn plan_rejects_zero_rate() {
    let mut p = FcPlan::default();
    fc_plan(0.0, 10.0, 0.0, 0, 0, 0, 0, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 0);
}

#[test]
fn plan_rejects_nonpositive_length() {
    let mut p = FcPlan::default();
    fc_plan(0.0, 0.0, 20.0, 1, 20_000, 0, 0, &mut p as *mut FcPlan);
    assert_eq!(p.ok, 0);
}
