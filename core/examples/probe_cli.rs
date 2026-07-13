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
    println!("ok                 : true");
    println!("header_sample_rate : {} Hz", r.header_sample_rate);
    println!("bits_per_sample    : {}", r.bits_per_sample);
    println!("channels           : {}", r.channels);
    println!("file_size          : {} bytes", r.file_size);
    println!("audio_offset       : {} bytes", r.audio_offset);
    println!("real_rate_hz       : {:.0}  (is_rf={})", r.real_rate_hz, r.is_rf);
    println!(
        "declared_total     : {} (raw STREAMINFO, pre-correction)",
        r.declared_total_samples
    );
    let provenance = if r.total_samples_from_vorbis {
        "vorbis-tag"
    } else if r.total_samples_from_companion {
        "companion-file"
    } else if r.total_samples_scanned {
        "scanned-from-frames"
    } else if r.total_samples_wraps > 0 {
        "wrap-corrected"
    } else {
        "header"
    };
    println!(
        "total_samples      : {} (known={}, wraps={}, estimated={}, provenance={})",
        r.total_samples, r.total_samples_known, r.total_samples_wraps, r.total_samples_estimated,
        provenance
    );
    if r.total_samples_known {
        let total_sec = r.total_samples as f64 / r.real_rate_hz;
        let h = total_sec as u64 / 3600;
        let m = (total_sec as u64 % 3600) / 60;
        let s = total_sec - (h as f64 * 3600.0 + m as f64 * 60.0);
        println!(
            "real_total_seconds : {:.3}  = {:02}:{:02}:{:05.2}  (at real_rate {:.0} Hz)",
            total_sec, h, m, s, r.real_rate_hz
        );
    }
}
