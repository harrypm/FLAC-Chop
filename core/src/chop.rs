//! SoX-driven sample-exact cutter.
//!
//! The actual cut is done by SoX: `sox <in> <out> trim <start>s <len>s`, where
//! the `s` suffix makes the numbers sample counts (per channel). This was
//! validated against a real 115 GB RF capture: a 10 s / 20 MSPS request
//! produced exactly 200,000,000 samples, 8-bit, with the 20 kHz header
//! preserved. The default path is a pure sample-exact trim; optional output
//! conversion controls can also apply rate/bit-depth processing.

use std::path::{Path, PathBuf};
use std::process::Command;
/// Optional post-cut processing controls for SoX.
#[derive(Debug, Clone, Copy, Default)]
pub struct ChopOptions {
    /// Output FLAC header sample rate in Hz (e.g. 20000 for 20 MSPS RF files
    /// that use the /1000 header convention). `None` keeps the source rate.
    pub output_rate_hz: Option<u64>,
    /// Requested output bit depth. `None` keeps source precision.
    ///
    /// Note: SoX/FLAC cannot encode true 6-bit FLAC. When this is `Some(6)`,
    /// the command writes an 8-bit FLAC container and applies `dither -p 6` so
    /// the effective precision is 6-bit ("bit-crushed 6-bit in 8-bit FLAC").
    pub output_bits: Option<u32>,
    /// Apply the wiki-aligned basic RF low-pass profile (`sinc -n 2500 0-...`)
    /// when changing output rate.
    pub basic_rf_filter: bool,
}

/// Map an RF output sample-rate header (Hz) to a basic wiki profile cutoff
/// (kHz) for SoX `sinc`.
fn rf_filter_cutoff_khz(output_rate_hz: u64) -> Option<u32> {
    match output_rate_hz {
        // 16 MSPS (experimental VHS profile)
        16_000 => Some(7650),
        // 20 MSPS VHS profile
        20_000 => Some(9650),
        // 24 MSPS profile
        24_000 => Some(9400),
        // 28.6 MSPS (8fsc) fallback profile (conservative, same as 24 MSPS profile)
        28_600 => Some(9400),
        _ => None,
    }
}

/// Outcome of a SoX cut.
pub struct ChopResult {
    pub ok: bool,
    pub exit_code: i32,
    pub stderr: String,
}

/// Run `sox in out trim <start>s <len>s`. Captures stderr for the GUI.
pub fn chop(in_path: &str, out_path: &str, start_samples: u64, length_samples: u64) -> ChopResult {
    chop_with_options(in_path, out_path, start_samples, length_samples, ChopOptions::default())
}

/// Run `sox in out trim <start>s <len>s` with optional output conversion.
pub fn chop_with_options(
    in_path: &str,
    out_path: &str,
    start_samples: u64,
    length_samples: u64,
    opts: ChopOptions,
) -> ChopResult {
    let start = format!("{}s", start_samples);
    let len = format!("{}s", length_samples);
    let mut cmd = Command::new("sox");
    cmd.arg(in_path);

    // Output format controls must be specified before the output path.
    if let Some(rate) = opts.output_rate_hz {
        if rate > 0 {
            cmd.arg("-r").arg(rate.to_string());
        }
    }

    // FLAC cannot encode 6-bit directly in SoX, so we store 8-bit and apply
    // a 6-bit precision reduction effect after trim.
    let mut emulated_six_bit = false;
    if let Some(bits) = opts.output_bits {
        if bits == 6 {
            cmd.arg("-b").arg("8");
            emulated_six_bit = true;
        } else if bits > 0 {
            cmd.arg("-b").arg(bits.to_string());
        }
    }

    cmd.arg(out_path);
    cmd.arg("trim").arg(&start).arg(&len);

    if let Some(rate) = opts.output_rate_hz {
        if rate > 0 && opts.basic_rf_filter {
            if let Some(cutoff_khz) = rf_filter_cutoff_khz(rate) {
                cmd.arg("sinc")
                    .arg("-n")
                    .arg("2500")
                    .arg(format!("0-{cutoff_khz}"));
            }
        }
    }

    if emulated_six_bit {
        cmd.arg("dither").arg("-p").arg("6");
    }

    let output = cmd.output();

    match output {
        Ok(o) => {
            let code = o.status.code().unwrap_or(-1);
            let stderr = String::from_utf8_lossy(&o.stderr).to_string();
            ChopResult {
                ok: o.status.success(),
                exit_code: code,
                stderr,
            }
        }
        Err(e) => ChopResult {
            ok: false,
            exit_code: -1,
            stderr: format!("failed to spawn sox: {e}"),
        },
    }
}

/// True if a `sox` executable responds to `--version`.
pub fn sox_available() -> bool {
    Command::new("sox")
        .arg("--version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

/// Build a sibling output path: `<dir>/<stem>-cut.<ext>` (ext defaults to
/// flac). If that file already exists, `-cut-2`, `-cut-3`, … are tried so an
/// earlier cut is never silently overwritten by SoX.
pub fn generate_output_path(in_path: &str) -> Option<String> {
    let p = Path::new(in_path);
    let stem = p.file_stem()?.to_str()?;
    let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("flac");
    let parent: PathBuf = match p.parent() {
        Some(par) if !par.as_os_str().is_empty() => par.to_path_buf(),
        _ => PathBuf::from("."),
    };
    let mut out = parent.join(format!("{}-cut.{}", stem, ext));
    let mut n = 2u32;
    while out.exists() && n < 10_000 {
        out = parent.join(format!("{}-cut-{}.{}", stem, n, ext));
        n += 1;
    }
    Some(out.to_string_lossy().into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_path_appends_cut() {
        let p = generate_output_path("/tmp/foo/bar.flac").unwrap();
        assert!(p.ends_with("bar-cut.flac"));
    }

    #[test]
    fn output_path_no_ext_defaults_flac() {
        let p = generate_output_path("/tmp/foo/RAW").unwrap();
        assert!(p.ends_with("RAW-cut.flac"));
    }

    #[test]
    fn output_path_no_parent_uses_dot() {
        // When the input has no parent dir, the output is written to the
        // current dir ("."). The exact separator differs by platform
        // ("./local-cut.flac" on Unix, ".\\local-cut.flac" on Windows), so
        // assert on the platform-correct form rather than a hard-coded string.
        let p = generate_output_path("local.flac").unwrap();
        let expected = {
            let mut pb = std::path::PathBuf::from(".");
            pb.push("local-cut.flac");
            pb.to_string_lossy().into_owned()
        };
        assert_eq!(p, expected);
        // And it must always end with the stem-cut.flac regardless of platform.
        assert!(p.ends_with("local-cut.flac"));
    }

    #[test]
    fn output_path_avoids_clobbering_existing_cut() {
        let dir = std::env::temp_dir().join("fc_test_chop");
        let _ = std::fs::create_dir_all(&dir);
        let input = dir.join("tape.flac");
        std::fs::write(&input, b"").unwrap();
        // First call: no existing cut → plain -cut.flac.
        let first = generate_output_path(input.to_str().unwrap()).unwrap();
        assert!(first.ends_with("tape-cut.flac"));
        // Simulate an existing previous cut → must pick -cut-2.flac.
        std::fs::write(&first, b"").unwrap();
        let second = generate_output_path(input.to_str().unwrap()).unwrap();
        assert!(second.ends_with("tape-cut-2.flac"), "got {second}");
        let _ = std::fs::remove_file(&first);
    }

    #[test]
    fn rf_filter_profiles_match_expected_presets() {
        assert_eq!(rf_filter_cutoff_khz(16_000), Some(7650));
        assert_eq!(rf_filter_cutoff_khz(20_000), Some(9650));
        assert_eq!(rf_filter_cutoff_khz(24_000), Some(9400));
        assert_eq!(rf_filter_cutoff_khz(28_600), Some(9400));
        assert_eq!(rf_filter_cutoff_khz(12_000), None);
    }
}
