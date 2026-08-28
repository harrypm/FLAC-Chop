//! SoX-driven sample-exact cutter.
//!
//! The actual cut is done by SoX: `sox <in> <out> trim <start>s <len>s`, where
//! the `s` suffix makes the numbers sample counts (per channel). This was
//! validated against a real 115 GB RF capture: a 10 s / 20 MSPS request
//! produced exactly 200,000,000 samples, 8-bit, with the 20 kHz header
//! preserved. The default path is a pure sample-exact trim; optional output
//! conversion controls can also apply rate/bit-depth processing.
//!
//! Two backends, selected at build time:
//!  - default: shell out to a `sox` executable (resolved beside the app exe,
//!    then PATH). Used when SoX is a bundled runtime binary (e.g. inside an
//!    AppImage / .app / extracted portable folder).
//!  - `static-sox` cargo feature: link libSoX statically and run the cut
//!    in-process via the effects-chain C API (no external `sox` binary). Used
//!    for the true single-binary Windows build (and any other self-contained
//!    build that static-links libSoX). The effect ordering mirrors the SoX
//!    CLI: user effects (`trim`, optional `sinc`, optional `dither`) in order,
//!    then the automatic `rate` effect when the output rate differs, then the
//!    output sink — reproducing `sox in -r R -b B out trim Ss Ls sinc … dither …`.

use std::path::{Path, PathBuf};
#[cfg(not(feature = "static-sox"))]
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

// ===========================================================================
// Default backend: shell out to a `sox` executable.
// ===========================================================================

#[cfg(not(feature = "static-sox"))]
mod shelldet {
    use std::env;
    use std::path::{Path, PathBuf};
    use std::process::Command;

    fn binary_name() -> &'static str {
        if cfg!(windows) {
            "sox.exe"
        } else {
            "sox"
        }
    }

    pub(super) fn candidates() -> Vec<PathBuf> {
        let mut out = Vec::new();
        if let Ok(raw) = env::var("FLAC_CHOP_SOX") {
            let trimmed = raw.trim();
            if !trimmed.is_empty() {
                out.push(PathBuf::from(trimmed));
            }
        }
        if let Ok(exe) = env::current_exe() {
            if let Some(dir) = exe.parent() {
                out.push(dir.join(binary_name()));
                #[cfg(target_os = "macos")]
                if let Some(contents_dir) = dir.parent() {
                    out.push(contents_dir.join("Resources").join("bin").join("sox"));
                }
            }
        }
        out.push(PathBuf::from("sox"));
        out
    }

    fn can_run(program: &Path) -> bool {
        Command::new(program)
            .arg("--version")
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
    }

    pub(super) fn resolve() -> Option<PathBuf> {
        candidates().into_iter().find(|p| can_run(p))
    }
}

#[cfg(not(feature = "static-sox"))]
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
    let sox_program = shelldet::resolve().unwrap_or_else(|| PathBuf::from("sox"));
    let mut cmd = Command::new(&sox_program);
    cmd.arg(in_path);

    if let Some(rate) = opts.output_rate_hz {
        if rate > 0 {
            cmd.arg("-r").arg(rate.to_string());
        }
    }

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
            stderr: format!("failed to spawn sox ({}): {e}", sox_program.display()),
        },
    }
}

#[cfg(not(feature = "static-sox"))]
/// True if a bundled or PATH `sox` executable responds to `--version`.
pub fn sox_available() -> bool {
    shelldet::resolve().is_some()
}

// ===========================================================================
// static-sox backend: in-process libSoX effects chain.
// ===========================================================================

#[cfg(feature = "static-sox")]
use crate::sox_ffi;

#[cfg(feature = "static-sox")]
use std::ffi::CString;
#[cfg(feature = "static-sox")]
use std::os::raw::c_char;
#[cfg(feature = "static-sox")]
use std::sync::Once;

#[cfg(feature = "static-sox")]
static SOX_INIT: Once = Once::new();

#[cfg(feature = "static-sox")]
fn ensure_sox_init() -> Result<(), String> {
    static mut INIT_OK: bool = false;
    SOX_INIT.call_once(|| {
        unsafe { INIT_OK = sox_ffi::sox_init() == sox_ffi::SOX_SUCCESS; }
    });
    if unsafe { INIT_OK } { Ok(()) } else { Err("sox_init() failed".to_string()) }
}

/// Add a string-arg effect (trim / sinc / dither / rate) to the chain.
#[cfg(feature = "static-sox")]
unsafe fn add_str_effect(
    chain: *mut sox_ffi::sox_effects_chain_t,
    name: &str,
    args: &[CString],
    in_sig: *mut sox_ffi::sox_signalinfo_t,
    out_sig: *const sox_ffi::sox_signalinfo_t,
) -> Result<(), String> {
    let cname = CString::new(name).map_err(|e| format!("effect name nul: {e}"))?;
    let handler = sox_ffi::sox_find_effect(cname.as_ptr());
    if handler.is_null() {
        return Err(format!("sox_find_effect({name}) returned null"));
    }
    let eff = sox_ffi::sox_create_effect(handler);
    if eff.is_null() {
        return Err(format!("sox_create_effect({name}) returned null"));
    }
    let mut argv: Vec<*mut c_char> = args.iter().map(|s| s.as_ptr() as *mut c_char).collect();
    let rc = sox_ffi::sox_effect_options(eff, argv.len() as i32, argv.as_mut_ptr());
    if rc != sox_ffi::SOX_SUCCESS {
        return Err(format!("sox_effect_options({name}) failed: {rc}"));
    }
    let rc = sox_ffi::sox_add_effect(chain, eff, in_sig, out_sig);
    if rc != sox_ffi::SOX_SUCCESS {
        return Err(format!("sox_add_effect({name}) failed: {rc}"));
    }
    Ok(())
}

/// Add the `input` or `output` sink effect, whose single arg is the
/// `sox_format_t*` (cast to `char*`), per example0.c.
#[cfg(feature = "static-sox")]
unsafe fn add_io_effect(
    chain: *mut sox_ffi::sox_effects_chain_t,
    name: &str,
    fmt: *mut sox_ffi::sox_format_t,
    in_sig: *mut sox_ffi::sox_signalinfo_t,
    out_sig: *const sox_ffi::sox_signalinfo_t,
) -> Result<(), String> {
    let cname = CString::new(name).map_err(|e| format!("name nul: {e}"))?;
    let handler = sox_ffi::sox_find_effect(cname.as_ptr());
    if handler.is_null() {
        return Err(format!("sox_find_effect({name}) null"));
    }
    let eff = sox_ffi::sox_create_effect(handler);
    if eff.is_null() {
        return Err(format!("sox_create_effect({name}) null"));
    }
    let mut argv: [*mut c_char; 1] = [fmt as *mut c_char];
    let rc = sox_ffi::sox_effect_options(eff, 1, argv.as_mut_ptr());
    if rc != sox_ffi::SOX_SUCCESS {
        return Err(format!("sox_effect_options({name}) {rc}"));
    }
    let rc = sox_ffi::sox_add_effect(chain, eff, in_sig, out_sig);
    if rc != sox_ffi::SOX_SUCCESS {
        return Err(format!("sox_add_effect({name}) {rc}"));
    }
    Ok(())
}

#[cfg(feature = "static-sox")]
/// Run the cut in-process via libSoX. Effect ordering mirrors the SoX CLI:
/// input → trim → [sinc] → [dither] → [rate, if output rate differs] → output.
pub fn chop_with_options(
    in_path: &str,
    out_path: &str,
    start_samples: u64,
    length_samples: u64,
    opts: ChopOptions,
) -> ChopResult {
    if let Err(e) = ensure_sox_init() {
        return ChopResult { ok: false, exit_code: -1, stderr: e };
    }

    let (in_c, out_c) = match (CString::new(in_path), CString::new(out_path)) {
        (Ok(i), Ok(o)) => (i, o),
        (Err(e), _) | (_, Err(e)) => {
            return ChopResult { ok: false, exit_code: -1, stderr: format!("path nul: {e}") }
        }
    };

    unsafe {
        let in_fmt = sox_ffi::sox_open_read(
            in_c.as_ptr(), std::ptr::null(), std::ptr::null(), std::ptr::null(),
        );
        if in_fmt.is_null() {
            return ChopResult { ok: false, exit_code: -1, stderr: "sox_open_read failed".into() };
        }

        // Build the output signal/encoding from the input, applying the
        // requested rate / bit-depth overrides (the `-r` / `-b` equivalents).
        let mut out_signal = (*in_fmt).signal;
        let mut out_encoding = (*in_fmt).encoding;
        let mut emulated_six_bit = false;
        if let Some(rate) = opts.output_rate_hz {
            if rate > 0 {
                out_signal.rate = rate as f64;
            }
        }
        if let Some(bits) = opts.output_bits {
            if bits == 6 {
                out_signal.precision = 8;
                out_encoding.bits_per_sample = 8;
                emulated_six_bit = true;
            } else if bits > 0 {
                out_signal.precision = bits;
                out_encoding.bits_per_sample = bits;
            }
        }
        out_signal.length = 0; // let the writer infer length from the chain.

        let out_fmt = sox_ffi::sox_open_write(
            out_c.as_ptr(), &out_signal, &out_encoding,
            std::ptr::null(), std::ptr::null(), None,
        );
        if out_fmt.is_null() {
            sox_ffi::sox_close(in_fmt);
            return ChopResult { ok: false, exit_code: -1, stderr: "sox_open_write failed".into() };
        }

        let chain = sox_ffi::sox_create_effects_chain(&(*in_fmt).encoding, &(*out_fmt).encoding);
        if chain.is_null() {
            sox_ffi::sox_close(out_fmt);
            sox_ffi::sox_close(in_fmt);
            return ChopResult { ok: false, exit_code: -1, stderr: "sox_create_effects_chain failed".into() };
        }

        // `in_sig` is mutated by sox_add_effect as effects change the stream;
        // start from the input signal and let each effect update it.
        let mut in_sig = (*in_fmt).signal;
        let out_sig_ref = (*out_fmt).signal;

        let mut err: Option<String> = None;
        if let Err(e) = add_io_effect(chain, "input", in_fmt, &mut in_sig, &out_sig_ref) {
            err = Some(e);
        }
        if err.is_none() {
            let a0 = CString::new(format!("{}s", start_samples)).unwrap();
            let a1 = CString::new(format!("{}s", length_samples)).unwrap();
            if let Err(e) = add_str_effect(chain, "trim", &[a0, a1], &mut in_sig, &out_sig_ref) {
                err = Some(e);
            }
        }
        if err.is_none() {
            if let Some(rate) = opts.output_rate_hz {
                if rate > 0 && opts.basic_rf_filter {
                    if let Some(cutoff) = rf_filter_cutoff_khz(rate) {
                        let s0 = CString::new("-n").unwrap();
                        let s1 = CString::new("2500").unwrap();
                        let s2 = CString::new(format!("0-{cutoff}")).unwrap();
                        if let Err(e) = add_str_effect(chain, "sinc", &[s0, s1, s2], &mut in_sig, &out_sig_ref) {
                            err = Some(e);
                        }
                    }
                }
            }
        }
        if err.is_none() && emulated_six_bit {
            let d0 = CString::new("-p").unwrap();
            let d1 = CString::new("6").unwrap();
            if let Err(e) = add_str_effect(chain, "dither", &[d0, d1], &mut in_sig, &out_sig_ref) {
                err = Some(e);
            }
        }
        // Automatic `rate` effect: the SoX CLI inserts this when the output
        // rate differs from the stream's current rate. The library API does
        // NOT auto-insert, so add it explicitly before the output sink.
        if err.is_none() {
            if let Some(rate) = opts.output_rate_hz {
                if rate > 0 && (in_sig.rate as u64) != rate {
                    let r = CString::new(rate.to_string()).unwrap();
                    if let Err(e) = add_str_effect(chain, "rate", &[r], &mut in_sig, &out_sig_ref) {
                        err = Some(e);
                    }
                }
            }
        }
        if err.is_none() {
            if let Err(e) = add_io_effect(chain, "output", out_fmt, &mut in_sig, &out_sig_ref) {
                err = Some(e);
            }
        }

        let (ok, stderr) = match err {
            Some(e) => (false, e),
            None => {
                let rc = sox_ffi::sox_flow_effects(chain, None, std::ptr::null_mut());
                if rc == sox_ffi::SOX_SUCCESS {
                    (true, String::new())
                } else {
                    (false, format!("sox_flow_effects failed: {rc}"))
                }
            }
        };

        sox_ffi::sox_delete_effects_chain(chain);
        sox_ffi::sox_close(out_fmt);
        sox_ffi::sox_close(in_fmt);

        ChopResult { ok, exit_code: if ok { 0 } else { -1 }, stderr }
    }
}

#[cfg(feature = "static-sox")]
/// SoX is statically linked, so it is always available.
pub fn sox_available() -> bool {
    true
}

// ===========================================================================
// Shared helpers.
// ===========================================================================

/// Run `sox in out trim <start>s <len>s`. Captures stderr for the GUI.
pub fn chop(in_path: &str, out_path: &str, start_samples: u64, length_samples: u64) -> ChopResult {
    chop_with_options(in_path, out_path, start_samples, length_samples, ChopOptions::default())
}

/// Build a sibling output path: `<dir>/<stem>-cut.<ext>` (ext defaults to
/// flac). If that file already exists, `-cut-2`, `-cut-3`, … are tried so an
/// earlier cut is never silently overwritten by SoX.
pub fn generate_output_path(in_path: &str, out_dir: &str) -> Option<String> {
    let p = Path::new(in_path);
    let stem = p.file_stem()?.to_str()?;
    let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("flac");
    // out_dir selects where the cut is written. An empty out_dir means "next
    // to the source" (the original sibling -cut.flac behaviour); otherwise the
    // cut goes into the user-chosen directory. A non-existent out_dir is not
    // created here — the caller (GUI) ensures it exists before chopping.
    let parent: PathBuf = if !out_dir.is_empty() {
        PathBuf::from(out_dir)
    } else {
        match p.parent() {
            Some(par) if !par.as_os_str().is_empty() => par.to_path_buf(),
            _ => PathBuf::from("."),
        }
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
        let p = generate_output_path("/tmp/foo/bar.flac", "").unwrap();
        assert!(p.ends_with("bar-cut.flac"));
    }

    #[test]
    fn output_path_no_ext_defaults_flac() {
        let p = generate_output_path("/tmp/foo/RAW", "").unwrap();
        assert!(p.ends_with("RAW-cut.flac"));
    }

    #[test]
    fn output_path_no_parent_uses_dot() {
        // When the input has no parent dir, the output is written to the
        // current dir ("."). The exact separator differs by platform
        // ("./local-cut.flac" on Unix, ".\\local-cut.flac" on Windows), so
        // assert on the platform-correct form rather than a hard-coded string.
        let p = generate_output_path("local.flac", "").unwrap();
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
    fn output_path_uses_explicit_out_dir() {
        // A non-empty out_dir redirects the cut into that directory while
        // keeping the <stem>-cut.<ext> naming + clobber avoidance.
        let dir = std::env::temp_dir().join("fc_test_outdir");
        let _ = std::fs::create_dir_all(&dir);
        let p = generate_output_path("/tmp/foo/bar.flac", dir.to_str().unwrap()).unwrap();
        let expected = dir.join("bar-cut.flac").to_string_lossy().into_owned();
        assert_eq!(p, expected);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn output_path_avoids_clobbering_existing_cut() {
        let dir = std::env::temp_dir().join("fc_test_chop");
        let _ = std::fs::create_dir_all(&dir);
        let input = dir.join("tape.flac");
        std::fs::write(&input, b"").unwrap();
        // First call: no existing cut → plain -cut.flac.
        let first = generate_output_path(input.to_str().unwrap(), "").unwrap();
        assert!(first.ends_with("tape-cut.flac"));
        // Simulate an existing previous cut → must pick -cut-2.flac.
        std::fs::write(&first, b"").unwrap();
        let second = generate_output_path(input.to_str().unwrap(), "").unwrap();
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
