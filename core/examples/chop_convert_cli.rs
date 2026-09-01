//! Headless conversion cutter for validation:
//! `chop_convert_cli <in.flac> <out.flac> <start_sec> <len_sec> <out_rate_khz|0> <out_bits|0> <filter 0/1>`
//!
//! Runs the exact chop_with_options path the GUI uses (rate + bit depth +
//! sinc filter + is_rf), so the 6-bit no-dither profile can be validated
//! end-to-end from the command line on real captures.

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 6 {
        eprintln!(
            "usage: chop_convert_cli <in.flac> <out.flac> <start_sec> <len_sec> <out_rate_khz|0=keep> <out_bits|0=source> [filter]"
        );
        std::process::exit(2);
    }
    let in_path = &args[1];
    let out_path = &args[2];
    let start_sec: f64 = args[3].parse().expect("start_sec not a number");
    let len_sec: f64 = args[4].parse().expect("len_sec not a number");
    let rate_khz: u64 = args[5].parse().expect("rate_khz not a number");

    let probe = flac_chop_core::probe::probe(Path::new(in_path));
    if !probe.ok {
        eprintln!("probe error: {}", probe.error);
        std::process::exit(1);
    }

    let mut plan = flac_chop_core::ffi::FcPlan::default();
    flac_chop_core::ffi::fc_plan(
        start_sec,
        len_sec,
        probe.real_rate_hz,
        probe.total_samples,
        if probe.total_samples_known { 1 } else { 0 },
        &mut plan as *mut _,
    );
    if plan.ok == 0 {
        eprintln!("plan error: {}", unsafe {
            std::ffi::CStr::from_ptr(plan.error.as_ptr()).to_string_lossy()
        });
        std::process::exit(1);
    }

    println!(
        "plan: start={} len={} samples (real_rate {:.0} Hz, is_rf={})",
        plan.start_samples, plan.length_samples, plan.real_sample_rate_hz, probe.is_rf
    );

    let opts = flac_chop_core::chop::ChopOptions {
        output_rate_hz: if rate_khz > 0 { Some(rate_khz) } else { None },
        output_bits: Some(6), // 6-bit crush profile under test
        basic_rf_filter: true,
        is_rf: probe.is_rf,
        // Sniff the input container; raw PCM rate comes from the <n>msps hint.
        input_format: None,
        input_rate_hz: None,
        input_channels: None,
    };
    let r = flac_chop_core::chop::chop_with_options(
        in_path,
        out_path,
        plan.start_samples,
        plan.length_samples,
        opts,
    );
    if r.ok {
        println!("ok: wrote {}{}", out_path, if r.stderr.is_empty() { String::new() } else { format!(" ({})", r.stderr) });
    } else {
        eprintln!("sox failed (exit {}): {}", r.exit_code, r.stderr);
        std::process::exit(1);
    }
}
