//! Headless cutter: `chop_cli <in.flac> <out.flac> <start_sec> <len_sec>`.
//!
//! Runs the exact same probe -> plan -> SoX chop path as the GUI, so it doubles
//! as both a scriptable cutter and an end-to-end test of the Rust core on real
//! RF captures without needing the Qt frontend.

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 5 {
        eprintln!("usage: chop_cli <in.flac> <out.flac> <start_sec> <len_sec>");
        std::process::exit(2);
    }
    let in_path = &args[1];
    let out_path = &args[2];
    let start_sec: f64 = match args[3].parse() {
        Ok(v) => v,
        Err(_) => {
            eprintln!("start_sec not a number");
            std::process::exit(2);
        }
    };
    let len_sec: f64 = match args[4].parse() {
        Ok(v) => v,
        Err(_) => {
            eprintln!("len_sec not a number");
            std::process::exit(2);
        }
    };

    let probe = flac_chop_core::probe::probe(Path::new(in_path));
    if !probe.ok {
        eprintln!("probe error: {}", probe.error);
        std::process::exit(1);
    }

    // fc_plan computes sample counts for SoX, which reads the file at its
    // STREAMINFO (on-disk) rate — NOT the real RF rate. For /1000 RF captures
    // the header rate is the /1000 "kHz" value and declared_total_samples is
    // the real count / 1000; passing the real values would produce counts
    // 1000× too large for SoX. Pass the STREAMINFO values (header_sample_rate
    // + declared_total_samples). For non-RF audio these equal the real values.
    let mut plan = flac_chop_core::ffi::FcPlan::default();
    flac_chop_core::ffi::fc_plan(
        start_sec,
        len_sec,
        probe.header_sample_rate as f64,
        probe.declared_total_samples,
        if probe.total_samples_known { 1 } else { 0 },
        &mut plan as *mut _,
    );
    if plan.ok == 0 {
        let msg = unsafe {
            std::ffi::CStr::from_ptr(plan.error.as_ptr())
                .to_string_lossy()
                .into_owned()
        };
        eprintln!("plan error: {}", msg);
        std::process::exit(1);
    }

    println!(
        "plan: start={} len={} samples (real_rate {:.0} Hz, is_rf={})",
        plan.start_samples, plan.length_samples, plan.real_sample_rate_hz, probe.is_rf
    );

    let r = flac_chop_core::chop::chop(in_path, out_path, plan.start_samples, plan.length_samples);
    if r.ok {
        println!("ok: wrote {}", out_path);
    } else {
        eprintln!("sox failed (exit {}): {}", r.exit_code, r.stderr);
        std::process::exit(1);
    }
}
