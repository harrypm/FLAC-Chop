//! SoX-driven sample-exact cutter.
//!
//! The actual cut is done by SoX: `sox <in> <out> trim <start>s <len>s`, where
//! the `s` suffix makes the numbers sample counts (per channel). This was
//! validated against a real 115 GB RF capture: a 10 s / 20 MSPS request
//! produced exactly 200,000,000 samples, 8-bit, with the 20 kHz header
//! preserved. SoX preserves bit depth and the FLAC sample-rate header when
//! input and output are both `.flac`, so no extra encoding flags are needed.

use std::path::{Path, PathBuf};
use std::process::Command;

/// Outcome of a SoX cut.
pub struct ChopResult {
    pub ok: bool,
    pub exit_code: i32,
    pub stderr: String,
}

/// Run `sox in out trim <start>s <len>s`. Captures stderr for the GUI.
pub fn chop(in_path: &str, out_path: &str, start_samples: u64, length_samples: u64) -> ChopResult {
    let start = format!("{}s", start_samples);
    let len = format!("{}s", length_samples);

    let output = Command::new("sox")
        .arg(in_path)
        .arg(out_path)
        .arg("trim")
        .arg(&start)
        .arg(&len)
        .output();

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
}
