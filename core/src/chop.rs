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
//!    CLI: user effects (`trim`, optional `sinc`) in order, then the automatic
//!    `rate` effect when the output rate differs, then the output sink —
//!    reproducing `sox in -r R -b B out trim Ss Ls sinc …`.
//!
//! No dither is ever applied: the 6-bit profile is a pure bit-shift
//! requantization (MISRC-style scaling math), which also compresses better.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::probe::{self, InputFormat};
#[cfg(not(feature = "static-sox"))]
use std::process::Command;
#[cfg(not(feature = "static-sox"))]
use std::io::Read;
#[cfg(not(feature = "static-sox"))]
use std::process::Stdio;

// Cancellation flag for an in-flight cut. The GUI sets this via
// `cancel_chop()` / `fc_chop_cancel()`; the shell-out backend polls it while
// waiting on the sox child and kills the child promptly when it is set. The
// static-sox backend checks it before starting the effects chain (in-process
// libSoX has no mid-flow cancellation hook, so a static-sox cut can only be
// stopped before it begins — the shipping builds use the shell-out backend).
static CANCEL: AtomicBool = AtomicBool::new(false);

/// Optional post-cut processing controls for SoX.
#[derive(Debug, Clone, Copy, Default)]
pub struct ChopOptions {
    // NOTE: `is_rf` defaults to `false` via `Default` (plain audio), so
    // existing callers that don't set it get non-RF tag semantics.
    /// Output FLAC header sample rate in Hz (e.g. 20000 for 20 MSPS RF files
    /// that use the /1000 header convention). `None` keeps the source rate.
    pub output_rate_hz: Option<u64>,
    /// Requested output bit depth. `None` keeps source precision.
    ///
    /// Note: SoX/FLAC cannot encode true 6-bit FLAC. When this is `Some(6)`,
    /// the command writes an 8-bit FLAC container and applies pure bit-shift
    /// requantization (no dither, matching MISRC-style capture scaling math).
    pub output_bits: Option<u32>,
    /// Apply the wiki-aligned basic RF low-pass profile (`sinc -n 2500 0-...`)
    /// when changing output rate.
    pub basic_rf_filter: bool,
    /// Whether the source is an RF capture (uses the /1000 header convention).
    /// Drives the post-cut Vorbis-tag rewrite (RF_SAMPLE_RATE = header*1000,
    /// RF_TOTAL_SAMPLES = streaminfo*1000, + RF_SAMPLE_RATE_KHZ) so the
    /// output's embedded metadata reflects the new altered capture params.
    /// Also pins non-FLAC inputs to the /1000 SoX stream rate (see
    /// [`sox_input_args`]).
    pub is_rf: bool,
    /// Input container format. `None` (default) sniffs the file by magic or
    /// extension (`probe::sniff_format`) at chop time — the normal path.
    pub input_format: Option<probe::InputFormat>,
    /// Real input sample rate (Hz) for non-FLAC inputs whose stream rate SoX
    /// cannot infer. `None` = derive: raw PCM takes it from the `<n>msps`
    /// filename hint, WAV from its fmt chunk. For RF sources SoX reads the
    /// input pinned to the /1000 convention rate (real/1000) so every
    /// sinc cutoff and resample keeps its FLAC-path semantics; audio sources
    /// use the real rate as-is. Ignored for FLAC (SoX reads the header).
    pub input_rate_hz: Option<f64>,
    /// Channel count for headerless raw PCM (no header to sniff it from).
    /// `None` = 1 (cxadc/DdD RF captures are mono).
    pub input_channels: Option<u32>,
}

/// SoX CLI args that must precede the input path for the sniffed input
/// format. FLAC: nothing (SoX sniffs the `fLaC` marker). WAV: normally
/// nothing, but a real-rate RF WAV (header > 1 MHz) is pinned to the /1000
/// convention rate so every sinc/rate effect keeps its FLAC-path semantics
/// (the cutoffs are specified against the kHz-rate stream). Raw PCM has no
/// header at all and needs the full `-t <type> -r <rate> -c <ch>` set; its
/// rate is the /1000-convention header rate for RF (msps*1000, exactly what
/// SoX sees for the equivalent FLAC) or the real rate for audio.
/// Returns Err when a raw file has no derivable rate (the probe requires an
/// `<n>msps` filename hint, so this mirrors the probe's own hard error).
fn sox_input_args(
    fmt: InputFormat,
    in_path: &str,
    opts: &ChopOptions,
) -> Result<Vec<String>, String> {
    Ok(match fmt {
        InputFormat::Flac => Vec::new(), // SoX sniffs the fLaC marker
        InputFormat::Wav => {
            // Pin only real-rate RF WAVs (header > 1 MHz): override SoX's
            // view of the input rate to the /1000 convention so the sinc
            // cutoffs and the output header stay in FLAC-pipeline units.
            // kHz-convention WAVs and audio WAVs need nothing.
            if opts.is_rf {
                if let Some(real) = wav_header_real_rate(in_path)? {
                    if real > 1_000_000.0 {
                        vec!["-r".into(), (real / 1000.0).to_string()]
                    } else {
                        Vec::new()
                    }
                } else {
                    Vec::new()
                }
            } else {
                Vec::new()
            }
        }
        InputFormat::U8 | InputFormat::S8 | InputFormat::U16 | InputFormat::S16 => {
            let mut args = vec![
                "-t".to_string(),
                fmt.sox_type().unwrap().to_string(),
            ];
            // Raw has no header: the stream rate is REQUIRED (SoX would assume
            // 8 kHz otherwise). RF sources use the /1000 convention rate —
            // identical to the stream rate SoX sees for the equivalent FLAC.
            let real = input_real_rate_hz(in_path, opts)?;
            let stream_rate = if opts.is_rf { real / 1000.0 } else { real };
            args.push("-r".into());
            args.push(stream_rate.to_string());
            args.push("-c".into());
            args.push(opts.input_channels.unwrap_or(1).to_string());
            args
        }
    })
}

/// Real input rate (Hz): explicit override, else the `<n>msps` filename hint.
/// Used only for non-FLAC inputs (SoX reads FLAC/WAV headers itself).
fn input_real_rate_hz(in_path: &str, opts: &ChopOptions) -> Result<f64, String> {
    if let Some(r) = opts.input_rate_hz {
        if r > 0.0 {
            return Ok(r as f64);
        }
    }
    crate::msps::extract_msps(in_path)
        .map(|m| m * 1_000_000.0)
        .ok_or_else(|| {
            "raw input needs a sample rate: rename the file with the rate (e.g. ..._8-bit_20msps.u8) — the <n>msps hint is required".to_string()
        })
}

/// Real rate (Hz) of a WAV input, resolved from its fmt chunk by the probe
/// (the single source of the rate rules). Used to decide whether a WAV needs
/// the /1000-convention rate pin (real-rate RF headers only). Errors when the
/// header can't be parsed — the caller surfaces it instead of guessing.
fn wav_header_real_rate(in_path: &str) -> Result<Option<f64>, String> {
    let res = probe::probe(Path::new(in_path));
    if res.ok {
        Ok(Some(res.real_rate_hz))
    } else {
        Err(res.error)
    }
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
/// Spawn a sox command and poll it until exit (supporting cancellation).
fn run_sox_child(mut cmd: Command) -> Result<(std::process::ExitStatus, String), ChopResult> {
    cmd.stdout(Stdio::null());
    cmd.stderr(Stdio::piped());
    CANCEL.store(false, Ordering::Relaxed);

    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            return Err(ChopResult {
                ok: false,
                exit_code: -1,
                stderr: format!("failed to spawn sox: {e}"),
            })
        }
    };

    // Poll try_wait (non-blocking) so a cancel request can kill sox without
    // waiting for the whole (potentially long) cut to finish. Checked every
    // 50 ms — low overhead, sub-100ms cancel latency.
    let status = loop {
        if let Some(s) = child.try_wait().unwrap_or(None) {
            break s;
        }
        if CANCEL.load(Ordering::Relaxed) {
            let _ = child.kill();
            let _ = child.wait(); // reap the killed child
            let mut stderr = String::new();
            if let Some(mut se) = child.stderr.take() {
                let _ = se.read_to_string(&mut stderr);
            }
            CANCEL.store(false, Ordering::Relaxed); // consume the flag
            return Err(ChopResult {
                ok: false,
                exit_code: -1,
                stderr: if stderr.trim().is_empty() {
                    "cancelled by user".to_string()
                } else {
                    stderr
                },
            });
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    };

    let mut stderr = String::new();
    if let Some(mut se) = child.stderr.take() {
        let _ = se.read_to_string(&mut stderr);
    }
    Ok((status, stderr))
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
    // 6-bit is emulated on a 6-bit grid in an 8-bit FLAC container via a
    // pure bit-shift requantization (no dither — MISRC-style capture math).
    // SoX has no mid-chain quantizer, so this needs two passes: pass 1
    // quantizes the signal onto the 6-bit grid (vol 0.25 + 8-bit sink
    // rounding), pass 2 rescales x4 so the file keeps the MISRC full-scale
    // container convention (values = 4·round(v/4), never clipped). The x4
    // rescale is lossless on integer samples.
    // Sniff the input container (FLAC / WAV / raw PCM) and build the SoX
    // args that must precede the input path (filetype + rate pin for raw;
    // rate pin only for real-rate RF WAVs).
    let in_fmt = match opts.input_format.map_or_else(
        || probe::sniff_format(Path::new(in_path)),
        Ok,
    ) {
        Ok(f) => f,
        Err(e) => return ChopResult { ok: false, exit_code: -1, stderr: e },
    };
    let in_args = match sox_input_args(in_fmt, in_path, &opts) {
        Ok(a) => a,
        Err(e) => return ChopResult { ok: false, exit_code: -1, stderr: e },
    };

    let six_bit = matches!(opts.output_bits, Some(6));
    let tmp_path = if six_bit {
        Some(
            std::env::temp_dir().join(format!("flac-chop-6bit-{}.flac", std::process::id())),
        )
    } else {
        None
    };

    let mut cmd = Command::new(&sox_program);
    // Global -D: no automatic dithering anywhere (dither noise is
    // incompressible and hurts both SNR and FLAC compression efficiency).
    cmd.arg("-D");
    // Input options (filetype/rate/channels) must precede the input path.
    for a in &in_args {
        cmd.arg(a);
    }
    cmd.arg(in_path);

    if let Some(rate) = opts.output_rate_hz {
        if rate > 0 {
            cmd.arg("-r").arg(rate.to_string());
        }
    }

    if let Some(bits) = opts.output_bits {
        if bits == 6 {
            cmd.arg("-b").arg("8");
        } else if bits > 0 {
            cmd.arg("-b").arg(bits.to_string());
        }
    }

    let out_target: PathBuf = if six_bit {
        tmp_path.clone().unwrap()
    } else {
        PathBuf::from(out_path)
    };
    cmd.arg(&out_target);
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

    if six_bit {
        // Pass 1 sink: quantize to the 6-bit grid (¼ amplitude, round-to-nearest).
        cmd.arg("vol").arg("0.25");
    }

    let (status, stderr) = match run_sox_child(cmd) {
        Ok(v) => v,
        Err(cancelled) => {
            // Clean up the partial pass-1 temp file on failure/cancel.
            if let Some(tmp) = &tmp_path {
                let _ = std::fs::remove_file(&tmp);
            }
            return cancelled;
        }
    };
    let code = status.code().unwrap_or(-1);
    let mut r = ChopResult {
        ok: status.success(),
        exit_code: code,
        stderr,
    };

    // Pass 2 (6-bit only): rescale x4 back to full container scale. Lossless
    // on integer samples (every value is already a multiple of 4 after pass 1).
    if r.ok && six_bit {
        let mut cmd2 = Command::new(&sox_program);
        cmd2.arg("-D");
        cmd2.arg(tmp_path.as_ref().unwrap());
        cmd2.arg("-b").arg("8");
        cmd2.arg(out_path);
        cmd2.arg("vol").arg("4");
        match run_sox_child(cmd2) {
            Ok((st, _se)) => {
                if !st.success() {
                    r.ok = false;
                    r.exit_code = st.code().unwrap_or(-1);
                    if r.stderr.is_empty() {
                        r.stderr = format!("6-bit rescale pass failed (exit {})", r.exit_code);
                    }
                }
            }
            Err(cr) => {
                r = cr;
            }
        }
        // The temp file is no longer needed in any path.
        let _ = std::fs::remove_file(tmp_path.as_ref().unwrap());
    }

    // Rewrite the RF Vorbis tags on the output to reflect the new cut
    // (MISRC-GUI embedding model). Non-fatal: the cut already succeeded, so a
    // tag-rewrite failure is surfaced as a warning in stderr, not a hard fail.
    if r.ok {
        rewrite_tags_after_cut(out_path, opts.is_rf, &mut r);
    }
    r
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
/// Input hints for `sox_open_read` with headerless raw PCM: libSoX cannot
/// guess signal/encoding/filetype without a header. FLAC/WAV pass None
/// (headers are sniffed). A real-rate RF WAV gets only the rate pin.
struct InHints {
    signal: sox_ffi::sox_signalinfo_t,
    encoding: sox_ffi::sox_encodinginfo_t,
    filetype: Option<CString>,
}

#[cfg(feature = "static-sox")]
/// Build the `sox_open_read` hints for the sniffed input format. Mirrors the
/// CLI semantics in [`sox_input_args`]: raw PCM gets filetype + rate +
/// channels (+ signedness), a real-rate RF WAV gets the /1000 rate pin only.
fn in_hints_for(
    fmt: InputFormat,
    in_path: &str,
    opts: &ChopOptions,
) -> Result<Option<InHints>, String> {
    let hints = match fmt {
        InputFormat::Flac => None,
        InputFormat::Wav => match (opts.is_rf, wav_header_real_rate(in_path)?) {
            (true, Some(real)) if real > 1_000_000.0 => Some(InHints {
                signal: sox_ffi::sox_signalinfo_t {
                    rate: real / 1000.0,
                    ..sox_ffi::sox_signalinfo_t::default()
                },
                encoding: sox_ffi::sox_encodinginfo_t::default(),
                filetype: None,
            }),
            _ => None,
        },
        raw => {
            let real = input_real_rate_hz(in_path, opts)?;
            let stream_rate = if opts.is_rf { real / 1000.0 } else { real };
            let (bits, encoding) = match fmt {
                InputFormat::U8 => (8u32, sox_ffi::SOX_ENCODING_UNSIGNED),
                InputFormat::S8 => (8, sox_ffi::SOX_ENCODING_SIGN2),
                InputFormat::U16 => (16, sox_ffi::SOX_ENCODING_UNSIGNED),
                _ => (16, sox_ffi::SOX_ENCODING_SIGN2),
            };
            Some(InHints {
                signal: sox_ffi::sox_signalinfo_t {
                    rate: stream_rate,
                    channels: opts.input_channels.unwrap_or(1),
                    precision: bits,
                    length: 0,
                    mult: std::ptr::null_mut(),
                },
                encoding: sox_ffi::sox_encodinginfo_t {
                    encoding,
                    bits_per_sample: bits,
                    ..sox_ffi::sox_encodinginfo_t::default()
                },
                filetype: Some(CString::new(fmt.sox_type().unwrap())?),
            })
        }
    };
    Ok(hints)
}

#[cfg(feature = "static-sox")]
/// Run one in-process libSoX effects chain: input → trim → [sinc] →
/// [rate, if output rate differs] → [vol] → output. No dither is ever added.
/// `in_hints` carries the headerless-raw/rate-pin input hints (None for
/// FLAC/WAV). Pass 2 of the 6-bit flow reads a temp FLAC → None.
unsafe fn run_chain(
    in_path: &str,
    out_path: &str,
    start_samples: u64,
    length_samples: u64,
    opts: &ChopOptions,
    quantize_vol: Option<f64>,
    in_hints: Option<&InHints>,
) -> Result<(), String> {
    let (in_c, out_c) = match (CString::new(in_path), CString::new(out_path)) {
        (Ok(i), Ok(o)) => (i, o),
        (Err(e), _) | (_, Err(e)) => return Err(format!("path nul: {e}")),
    };

    // Pointers for sox_open_read: nulls for FLAC/WAV (sniffed), the hint
    // structs for raw/rate-pinned inputs ( CString outlives the call ).
    let (sig_ptr, enc_ptr, ft_ptr) = match in_hints {
        Some(h) => (
            &h.signal as *const sox_ffi::sox_signalinfo_t,
            &h.encoding as *const sox_ffi::sox_encodinginfo_t,
            h.filetype.as_ref().map(|c| c.as_ptr()).unwrap_or(std::ptr::null()),
        ),
        None => (
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
        ),
    };

    unsafe {
        let in_fmt = sox_ffi::sox_open_read(
            in_c.as_ptr(), sig_ptr, enc_ptr, ft_ptr,
        );
        if in_fmt.is_null() {
            return Err("sox_open_read failed".into());
        }

        // Build the output signal/encoding from the input, applying the
        // requested rate / bit-depth overrides (the `-r` / `-b` equivalents).
        let mut out_signal = (*in_fmt).signal;
        let mut out_encoding = (*in_fmt).encoding;
        if let Some(rate) = opts.output_rate_hz {
            if rate > 0 {
                out_signal.rate = rate as f64;
            }
        }
        if let Some(bits) = opts.output_bits {
            if bits == 6 {
                out_signal.precision = 8;
                out_encoding.bits_per_sample = 8;
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
            return Err("sox_open_write failed".into());
        }

        let chain = sox_ffi::sox_create_effects_chain(&(*in_fmt).encoding, &(*out_fmt).encoding);
        if chain.is_null() {
            sox_ffi::sox_close(out_fmt);
            sox_ffi::sox_close(in_fmt);
            return Err("sox_create_effects_chain failed".into());
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
            if let Some(v) = quantize_vol {
                let a = vol_effect_args(v);
                if let Err(e) = add_str_effect(chain, "vol", &[a], &mut in_sig, &out_sig_ref) {
                    err = Some(e);
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

        match (err, ok) {
            (Some(e), _) => Err(e),
            (None, true) => Ok(()),
            (None, false) => Err("sox_flow_effects failed".into()),
        }
    }
}

#[cfg(feature = "static-sox")]
unsafe fn vol_effect_args(v: f64) -> CString {
    CString::new(format!("{v}")).unwrap()
}

#[cfg(feature = "static-sox")]
/// Run the cut in-process via libSoX. The 6-bit profile is a pure bit-shift
/// requantization (no dither): pass 1 quantizes onto the 6-bit grid
/// (vol 0.25 + 8-bit sink rounding), pass 2 rescales x4 back to the MISRC
/// full-scale container convention (values = 4·round(v/4), lossless on
/// integer samples, never clipped).
pub fn chop_with_options(
    in_path: &str,
    out_path: &str,
    start_samples: u64,
    length_samples: u64,
    opts: ChopOptions,
) -> ChopResult {
    CANCEL.store(false, Ordering::Relaxed);
    // The static-sox backend runs the cut in-process via sox_flow_effects,
    // which has no mid-flow cancellation hook. Honour a cancel that was
    // requested before the chain starts; once flow begins it runs to
    // completion (the shipping builds use the shell-out backend, which can
    // cancel mid-cut).
    if CANCEL.load(Ordering::Relaxed) {
        return ChopResult { ok: false, exit_code: -1, stderr: "cancelled by user".into() };
    }
    if let Err(e) = ensure_sox_init() {
        return ChopResult { ok: false, exit_code: -1, stderr: e };
    }

    // Sniff the input container and build the sox_open_read hints for
    // headerless raw PCM / real-rate RF WAV rate pins (None for FLAC/WAV).
    let result: Result<(), String> = (|| {
        let in_fmt = opts
            .input_format
            .map_or_else(|| probe::sniff_format(Path::new(in_path)), Ok)?;
        let hints = in_hints_for(in_fmt, in_path, &opts)?;
        let six_bit = matches!(opts.output_bits, Some(6));
        unsafe {
            if six_bit {
                // Pass 1: quantize to the 6-bit grid (vol 0.25, 8-bit sink) into
                // a temp file; pass 2 rescales x4 back to full container scale.
                let tmp = std::env::temp_dir()
                    .join(format!("flac-chop-6bit-{}.flac", std::process::id()));
                let tmp_str = tmp.to_string_lossy().into_owned();
                if let Err(e) = run_chain(
                    in_path,
                    &tmp_str,
                    start_samples,
                    length_samples,
                    &opts,
                    Some(0.25),
                    hints.as_ref(),
                ) {
                    return Err(e);
                }
                // Pass 2: rescale x4 (lossless on integer samples). No rate
                // change, no filter — the temp file is already the finished cut.
                let r2 = run_chain(&tmp_str, out_path, 0, length_samples, &opts, Some(4.0), None);
                let _ = std::fs::remove_file(&tmp_str);
                r2
            } else {
                run_chain(
                    in_path,
                    out_path,
                    start_samples,
                    length_samples,
                    &opts,
                    None,
                    hints.as_ref(),
                )
            }
        }
    })();

    let mut r = match result {
        Ok(()) => ChopResult { ok: true, exit_code: 0, stderr: String::new() },
        Err(e) => ChopResult { ok: false, exit_code: -1, stderr: e },
    };
    // Rewrite the RF Vorbis tags on the output to reflect the new cut
    // (MISRC-GUI embedding model). Non-fatal: the cut already succeeded, so a
    // tag-rewrite failure is surfaced as a warning in stderr, not a hard fail.
    if r.ok {
        rewrite_tags_after_cut(out_path, opts.is_rf, &mut r);
    }
    r
}

#[cfg(feature = "static-sox")]
/// SoX is statically linked, so it is always available.
pub fn sox_available() -> bool {
    true
}

// ===========================================================================
// Shared helpers.
// ===========================================================================

/// Rewrite the RF Vorbis tags on a just-cut output file to reflect the new
/// altered metadata (MISRC-GUI embedding model). Non-fatal: on failure,
/// appends a warning to `r.stderr` but leaves `r.ok` true (the cut itself
/// succeeded).
fn rewrite_tags_after_cut(out_path: &str, is_rf: bool, r: &mut ChopResult) {
    match crate::tags::rewrite_cut_tags(std::path::Path::new(out_path), is_rf) {
        Ok(()) => {}
        Err(e) => {
            let note = format!("warning: tag rewrite failed: {e}");
            if r.stderr.is_empty() {
                r.stderr = note;
            } else {
                r.stderr.push('\n');
                r.stderr.push_str(&note);
            }
        }
    }
}

/// Request cancellation of the currently-running cut (if any). Safe to call
/// from the GUI thread while `chop_with_options` runs on a worker thread.
pub fn cancel_chop() {
    CANCEL.store(true, Ordering::Relaxed);
}

/// Run `sox in out trim <start>s <len>s`. Captures stderr for the GUI.
pub fn chop(in_path: &str, out_path: &str, start_samples: u64, length_samples: u64) -> ChopResult {
    chop_with_options(in_path, out_path, start_samples, length_samples, ChopOptions::default())
}

/// Build a sibling output path: `<dir>/<stem>-cut.<ext>` (ext defaults to
/// flac). If that file already exists, `-cut-2`, `-cut-3`, … are tried so an
/// earlier cut is never silently overwritten by SoX.
///
/// `stem_override` (when non-empty) replaces the input's stem, so the GUI can
/// rename the output to reflect the new altered metadata — e.g. an input named
/// `20msps_8-bit` cut to 16 MSPS / 6-bit becomes `16msps_6-bit` (matching the
/// MISRC capture naming convention). Clobber avoidance then appends `-2`,
/// `-3`, … to that stem.
pub fn generate_output_path(in_path: &str, out_dir: &str, stem_override: &str) -> Option<String> {
    let p = Path::new(in_path);
    let src_stem = p.file_stem()?.to_str()?;
    let stem = if !stem_override.is_empty() { stem_override } else { src_stem };
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
    let suffix = if stem_override.is_empty() { "-cut" } else { "" };
    let mut out = parent.join(format!("{}{}.{}", stem, suffix, ext));
    let mut n = 2u32;
    while out.exists() && n < 10_000 {
        out = parent.join(format!("{}{}-{}.{}", stem, suffix, n, ext));
        n += 1;
    }
    Some(out.to_string_lossy().into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_path_appends_cut() {
        let p = generate_output_path("/tmp/foo/bar.flac", "", "").unwrap();
        assert!(p.ends_with("bar-cut.flac"));
    }

    #[test]
    fn output_path_no_ext_defaults_flac() {
        let p = generate_output_path("/tmp/foo/RAW", "", "").unwrap();
        assert!(p.ends_with("RAW-cut.flac"));
    }

    #[test]
    fn output_path_no_parent_uses_dot() {
        // When the input has no parent dir, the output is written to the
        // current dir ("."). The exact separator differs by platform
        // ("./local-cut.flac" on Unix, ".\\local-cut.flac" on Windows), so
        // assert on the platform-correct form rather than a hard-coded string.
        let p = generate_output_path("local.flac", "", "").unwrap();
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
        let p = generate_output_path("/tmp/foo/bar.flac", dir.to_str().unwrap(), "").unwrap();
        let expected = dir.join("bar-cut.flac").to_string_lossy().into_owned();
        assert_eq!(p, expected);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn output_path_uses_stem_override() {
        // A non-empty stem_override renames the output to reflect the new
        // altered metadata (e.g. 20msps_8-bit -> 16msps_6-bit), with no
        // `-cut` suffix and clobber avoidance as `<stem>-2`.
        let dir = std::env::temp_dir().join("fc_test_stem");
        let _ = std::fs::create_dir_all(&dir);
        let base = dir.join("20msps_8-bit.flac");
        std::fs::write(&base, b"").unwrap();
        let p = generate_output_path(base.to_str().unwrap(), dir.to_str().unwrap(), "16msps_6-bit").unwrap();
        let expected = dir.join("16msps_6-bit.flac").to_string_lossy().into_owned();
        assert_eq!(p, expected, "got {p}, expected {expected}");
        // Simulate an existing file -> must pick -2.
        std::fs::write(&expected, b"").unwrap();
        let p2 = generate_output_path(base.to_str().unwrap(), dir.to_str().unwrap(), "16msps_6-bit").unwrap();
        assert!(p2.ends_with("16msps_6-bit-2.flac"), "got {p2}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn output_path_avoids_clobbering_existing_cut() {
        let dir = std::env::temp_dir().join("fc_test_chop");
        let _ = std::fs::create_dir_all(&dir);
        let input = dir.join("tape.flac");
        std::fs::write(&input, b"").unwrap();
        // First call: no existing cut → plain -cut.flac.
        let first = generate_output_path(input.to_str().unwrap(), "", "").unwrap();
        assert!(first.ends_with("tape-cut.flac"));
        // Simulate an existing previous cut → must pick -cut-2.flac.
        std::fs::write(&first, b"").unwrap();
        let second = generate_output_path(input.to_str().unwrap(), "", "").unwrap();
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

    fn opts(rf: bool) -> ChopOptions {
        ChopOptions {
            is_rf: rf,
            ..Default::default()
        }
    }

    #[test]
    fn input_args_flac_and_wav_are_empty() {
        assert_eq!(sox_input_args(probe::InputFormat::Flac, "x.flac", &opts(true)).unwrap(), Vec::<String>::new());
        // Audio WAV: no pin at all.
        assert_eq!(
            sox_input_args(probe::InputFormat::Wav, "/tmp/whatever.wav", &opts(false)).unwrap(),
            Vec::<String>::new()
        );
    }

    #[test]
    fn input_args_raw_rf_uses_convention_rate() {
        // 40 msps raw u8 named with the msps hint: SoX must see -t u8 -r 40000
        // (the /1000 convention rate — identical to the equivalent FLAC).
        let o = ChopOptions { is_rf: true, ..Default::default() };
        let a = sox_input_args(probe::InputFormat::U8, "/tmp/tape_8-bit_40msps.u8", &o).unwrap();
        assert_eq!(a, vec!["-t", "u8", "-r", "40000", "-c", "1"]);
    }

    #[test]
    fn input_args_raw_audio_uses_real_rate() {
        let o = ChopOptions { is_rf: false, input_rate_hz: Some(48_000.0), ..Default::default() };
        let a = sox_input_args(probe::InputFormat::S16, "/tmp/song_48k.s16", &o).unwrap();
        assert_eq!(a, vec!["-t", "s16", "-r", "48000", "-c", "1"]);
        // And an explicit channel count is honoured.
        let o2 = ChopOptions { input_channels: Some(2), input_rate_hz: Some(48000.0), ..Default::default() };
        let a2 = sox_input_args(probe::InputFormat::U8, "x.u8", &o2).unwrap();
        assert_eq!(a2, vec!["-t", "u8", "-r", "48000", "-c", "2"]);
    }

    #[test]
    fn input_args_raw_without_rate_hint_is_an_error() {
        let o = ChopOptions::default();
        let e = sox_input_args(probe::InputFormat::U8, "/tmp/nohint.u8", &o).unwrap_err();
        assert!(e.contains("msps"), "got: {e}");
    }
}
