//! Minimal CLI: probe a FLAC file's STREAMINFO and print the fields the GUI
//! will show. Used to validate the Rust core against real RF captures
//! independent of the Qt GUI.

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: probe_cli <file.flac>");
        std::process::exit(2);
    }
    let path = Path::new(&args[1]);
    let r = flac_chop_core::probe::probe(path);
    if !r.ok {
        eprintln!("probe error: {}", r.error);
        std::process::exit(1);
    }
    let msps = flac_chop_core::msps::extract_msps(&args[1]);
    println!("ok                 : true");
    println!("header_sample_rate : {}", r.header_sample_rate);
    println!("bits_per_sample    : {}", r.bits_per_sample);
    println!("channels           : {}", r.channels);
    println!("file_size          : {} bytes", r.file_size);
    println!("audio_offset       : {} bytes", r.audio_offset);
    println!(
        "declared_total     : {} (raw STREAMINFO, pre-correction)",
        r.declared_total_samples
    );
    println!(
        "total_samples      : {} (known={}, wraps={}, estimated={})",
        r.total_samples, r.total_samples_known, r.total_samples_wraps, r.total_samples_estimated
    );
    println!("msps_from_name     : {:?}", msps);
    let real_rate = msps.map(|m| m * 1e6).unwrap_or(r.header_sample_rate as f64);
    if r.total_samples_known {
        let total_sec = r.total_samples as f64 / real_rate;
        let h = total_sec as u64 / 3600;
        let m = (total_sec as u64 % 3600) / 60;
        let s = total_sec - (h as f64 * 3600.0 + m as f64 * 60.0);
        println!(
            "real_total_seconds : {:.3}  = {:02}:{:02}:{:05.2}  (at real_rate {:.0} Hz)",
            total_sec, h, m, s, real_rate
        );
    }
}
