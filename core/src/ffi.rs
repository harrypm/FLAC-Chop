//! C ABI surface for the Qt6 GUI.
//!
//! All structs are `#[repr(C)]` with fixed-size error buffers so the C++ side
//! can include a matching `flacchop.h` and link the staticlib directly. Strings
//! cross the boundary as NUL-terminated `const char*`; outputs are written into
//! caller-provided fixed-size arrays to avoid any allocation across the ABI.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;

use crate::{chop, msps, probe, tags};

/// Copy `s` (truncated) into a NUL-terminated fixed `[c_char; N]` buffer.
/// If truncation is needed, it backs off to a UTF-8 character boundary so the
/// C++ side never receives a half character (QString::fromUtf8 would render a
/// replacement glyph).
fn set_str(buf: &mut [c_char], s: &str) {
    if buf.is_empty() {
        return;
    }
    let bytes = s.as_bytes();
    let mut n = bytes.len().min(buf.len() - 1);
    if n < bytes.len() {
        // Truncated: step back past any continuation bytes (10xxxxxx).
        while n > 0 && (bytes[n] & 0b1100_0000) == 0b1000_0000 {
            n -= 1;
        }
    }
    for (i, b) in bytes[..n].iter().enumerate() {
        buf[i] = *b as c_char;
    }
    buf[n] = 0;
}

/// Format a caught panic payload into a short message.
fn panic_msg(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&'static str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown cause".to_string()
    }
}

#[repr(C)]
pub struct FcProbe {
    pub ok: i32,
    pub header_sample_rate: u64,
    /// Raw total_samples from STREAMINFO, before 36-bit wrap correction.
    pub declared_total_samples: u64,
    /// Total samples AFTER 36-bit wrap correction (what the GUI/cutter use).
    pub total_samples: u64,
    pub total_samples_known: i32,
    /// Number of 2^36 wraps added to declared to get total_samples (0 = trusted).
    pub total_samples_wraps: u32,
    /// 1 if the wrap count is an estimate (frame-size stats unavailable).
    pub total_samples_estimated: i32,
    /// 1 if the total was obtained by scanning frame headers (unknown header).
    pub total_samples_scanned: i32,
    /// 1 if the total was inferred from a companion .log/.wav file.
    pub total_samples_from_companion: i32,
    /// 1 if the total was read from a Vorbis RF_TOTAL_SAMPLES tag.
    pub total_samples_from_vorbis: i32,
    /// 1 if the RF rate was confirmed by a Vorbis RF_SAMPLE_RATE tag.
    pub rate_from_vorbis: i32,
    pub bits_per_sample: u32,
    pub channels: u32,
    pub file_size: u64,
    pub audio_offset: u64,
    /// Real sample rate in Hz (header * 1000 for RF, or header for audio).
    pub real_rate_hz: f64,
    /// 1 if the file is treated as RF (rate was ×1000 or msps hint used).
    pub is_rf: i32,
    pub msps: f64,
    pub msps_known: i32,
    pub error: [c_char; 256],
    /// Non-fatal diagnostics (tag-unit corrections, scan misalignment, vorbis
    /// mismatches), "; "-joined. Empty when everything checked out. NOTE: this
    /// field was appended to the struct — the C header (flacchop.h) must be
    /// regenerated to match before the GUI links against this build.
    pub warnings: [c_char; 512],
    /// Sniffed input container format: 0=flac 1=wav 2=u8 3=s8 4=u16 5=s16.
    /// Appended at the end of the struct (ABI-append-only) — flacchop.h must
    /// be updated in lockstep.
    pub format: u32,
}

impl Default for FcProbe {
    fn default() -> Self {
        Self {
            ok: 0,
            header_sample_rate: 0,
            declared_total_samples: 0,
            total_samples: 0,
            total_samples_known: 0,
            total_samples_wraps: 0,
            total_samples_estimated: 0,
            total_samples_scanned: 0,
            total_samples_from_companion: 0,
            total_samples_from_vorbis: 0,
            rate_from_vorbis: 0,
            bits_per_sample: 0,
            channels: 0,
            file_size: 0,
            audio_offset: 0,
            real_rate_hz: 0.0,
            is_rf: 0,
            msps: 0.0,
            msps_known: 0,
            error: [0; 256],
            warnings: [0; 512],
            format: 0,
        }
    }
}

/// Probe a FLAC file's STREAMINFO. Writes into `out`. Safe to call with a null
/// `out` (no-op) or null `path` (writes an error into `out`).
///
/// The probe body is wrapped in `catch_unwind`: the GUI runs this on a
/// QtConcurrent (C++) thread, where a Rust panic cannot unwind and would abort
/// the whole process. Catching it turns a panic into a normal error string in
/// `out.error` so the GUI can show "Probe failed: …" instead of crashing.
#[no_mangle]
pub extern "C" fn fc_probe(path: *const c_char, out: *mut FcProbe) {
    unsafe {
        if out.is_null() {
            return;
        }
        let out = &mut *out;
        *out = FcProbe::default();

        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            fc_probe_impl(path, out)
        }));
        if let Err(payload) = result {
            let msg = panic_msg(payload);
            set_str(&mut out.error, &format!("probe panicked: {msg}"));
        }
    }
}

/// Inner probe body, isolated so `fc_probe` can catch panics around it.
fn fc_probe_impl(path: *const c_char, out: &mut FcProbe) {
    if path.is_null() {
        set_str(&mut out.error, "null path");
        return;
    }
    let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(s) => s,
        Err(_) => {
            set_str(&mut out.error, "path is not valid UTF-8");
            return;
        }
    };

    let res = probe::probe(std::path::Path::new(path_str));
    if !res.ok {
        set_str(&mut out.error, &res.error);
        return;
    }
    out.ok = 1;
    out.header_sample_rate = res.header_sample_rate;
    out.declared_total_samples = res.declared_total_samples;
    out.total_samples = res.total_samples;
    out.total_samples_known = if res.total_samples_known { 1 } else { 0 };
    out.total_samples_wraps = res.total_samples_wraps;
    out.total_samples_estimated = if res.total_samples_estimated { 1 } else { 0 };
    out.total_samples_scanned = if res.total_samples_scanned { 1 } else { 0 };
    out.total_samples_from_companion = if res.total_samples_from_companion { 1 } else { 0 };
    out.total_samples_from_vorbis = if res.total_samples_from_vorbis { 1 } else { 0 };
    out.rate_from_vorbis = if res.rate_from_vorbis { 1 } else { 0 };
    out.bits_per_sample = res.bits_per_sample;
    out.channels = res.channels;
    out.file_size = res.file_size;
    out.audio_offset = res.audio_offset;
    out.real_rate_hz = res.real_rate_hz;
    out.is_rf = if res.is_rf { 1 } else { 0 };
    if let Some(m) = msps::extract_msps(path_str) {
        out.msps = m;
        out.msps_known = 1;
    }
    // 0=flac 1=wav 2=u8 3=s8 4=u16 5=s16 (probe::InputFormat discriminants).
    out.format = res.format as u32;
    set_str(&mut out.warnings, &res.warnings);
}

#[repr(C)]
pub struct FcPlan {
    pub ok: i32,
    pub start_samples: u64,
    pub length_samples: u64,
    pub end_sample: u64,
    pub real_sample_rate_hz: f64,
    pub real_total_seconds: f64,
    pub error: [c_char; 256],
}

impl Default for FcPlan {
    fn default() -> Self {
        Self {
            ok: 0,
            start_samples: 0,
            length_samples: 0,
            end_sample: 0,
            real_sample_rate_hz: 0.0,
            real_total_seconds: 0.0,
            error: [0; 256],
        }
    }
}

/// Compute a sample-exact cut plan from seconds. `real_rate_hz` is the
/// already-resolved real sample rate (from `fc_probe`'s `real_rate_hz` field —
/// header ×1000 for RF captures, or the header rate for real audio).
/// `total_samples` is the real total (from `fc_probe`, possibly from the
/// RF_TOTAL_SAMPLES Vorbis tag). SoX reads each FLAC frame as one real RF
/// sample, so the real rate is correct for computing sample counts.
///
/// Note: for /1000 RF captures, SoX trusts the STREAMINFO total_samples
/// (the real count / 1000, e.g. 208M not 208B) and won't read beyond it.
/// This limits the maximum cuttable range to ~10.4 s at 20 MHz for a typical
/// VHS capture — a separate STREAMINFO-rewrite fix is needed for longer cuts.
/// When total samples are known, the cut is clamped to the file.
#[no_mangle]
pub extern "C" fn fc_plan(
    start_sec: f64,
    len_sec: f64,
    real_rate_hz: f64,
    total_samples: u64,
    total_known: i32,
    out: *mut FcPlan,
) {
    unsafe {
        if out.is_null() {
            return;
        }
        let out = &mut *out;
        *out = FcPlan::default();

        // Guard against panics: like fc_probe, this runs on a C++ thread
        // where a Rust panic cannot unwind and would abort the process.
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            fc_plan_impl(start_sec, len_sec, real_rate_hz, total_samples, total_known, out)
        }));
        if let Err(payload) = result {
            let msg = panic_msg(payload);
            set_str(&mut out.error, &format!("plan panicked: {msg}"));
            out.ok = 0;
        }
    }
}

fn fc_plan_impl(
    start_sec: f64,
    len_sec: f64,
    real_rate_hz: f64,
    total_samples: u64,
    total_known: i32,
    out: &mut FcPlan,
) {
    let real_rate = real_rate_hz;
    if !real_rate.is_finite() || !(real_rate > 0.0) {
        set_str(&mut out.error, "sample rate is zero or invalid");
        return;
    }
    if !len_sec.is_finite() || len_sec <= 0.0 {
        set_str(&mut out.error, "length must be > 0");
        return;
    }
    if !start_sec.is_finite() || start_sec < 0.0 {
        set_str(&mut out.error, "start must be >= 0");
        return;
    }

    let start_s = (start_sec * real_rate).round() as u64;
    let mut len_s = (len_sec * real_rate).round() as u64;

    if total_known != 0 {
        out.real_total_seconds = total_samples as f64 / real_rate;
        // A start at or past the end of the file cannot yield a valid cut;
        // report it instead of silently producing a degenerate 1-sample cut
        // past EOF (the previous behavior).
        if start_s >= total_samples {
            set_str(&mut out.error, "start is at or past the end of the file");
            return;
        }
        if start_s + len_s > total_samples {
            len_s = total_samples - start_s; // start_s < total_samples here
        }
    }
    if len_s == 0 {
        // Sub-sample length request rounded to zero; cut a single sample.
        // (Guaranteed in-bounds: start_s < total_samples when total is known.)
        len_s = 1;
    }

    out.start_samples = start_s;
    out.length_samples = len_s;
    out.end_sample = start_s.saturating_add(len_s);
    out.real_sample_rate_hz = real_rate;
    out.ok = 1;
}

#[repr(C)]
pub struct FcChopResult {
    pub ok: i32,
    pub exit_code: i32,
    pub stderr_buf: [c_char; 1024],
}

impl Default for FcChopResult {
    fn default() -> Self {
        Self {
            ok: 0,
            exit_code: -1,
            stderr_buf: [0; 1024],
        }
    }
}

/// Run the SoX cut/conversion. Blocking — the GUI calls this on a worker thread.
#[no_mangle]
pub extern "C" fn fc_chop(
    in_path: *const c_char,
    out_path: *const c_char,
    start_samples: u64,
    length_samples: u64,
    output_rate_hz: u64,
    output_bits: u32,
    basic_rf_filter: i32,
    is_rf: i32,
    out: *mut FcChopResult,
) {
    unsafe {
        if out.is_null() {
            return;
        }
        let out = &mut *out;
        *out = FcChopResult::default();

        // Guard against panics on the C++ worker thread (same rationale as
        // fc_probe/fc_plan: an uncaught Rust panic there aborts the process).
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            fc_chop_impl(
                in_path,
                out_path,
                start_samples,
                length_samples,
                output_rate_hz,
                output_bits,
                basic_rf_filter,
                is_rf,
                out,
            )
        }));
        if let Err(payload) = result {
            let msg = panic_msg(payload);
            set_str(&mut out.stderr_buf, &format!("chop panicked: {msg}"));
            out.ok = 0;
        }
    }
}

fn fc_chop_impl(
    in_path: *const c_char,
    out_path: *const c_char,
    start_samples: u64,
    length_samples: u64,
    output_rate_hz: u64,
    output_bits: u32,
    basic_rf_filter: i32,
    is_rf: i32,
    out: &mut FcChopResult,
) {
    unsafe {
        if in_path.is_null() || out_path.is_null() {
            set_str(&mut out.stderr_buf, "null path");
            return;
        }
        let i = match CStr::from_ptr(in_path).to_str() {
            Ok(s) => s,
            Err(_) => {
                set_str(&mut out.stderr_buf, "input path is not valid UTF-8");
                return;
            }
        };
        let o = match CStr::from_ptr(out_path).to_str() {
            Ok(s) => s,
            Err(_) => {
                set_str(&mut out.stderr_buf, "output path is not valid UTF-8");
                return;
            }
        };

        let opts = chop::ChopOptions {
            output_rate_hz: if output_rate_hz > 0 { Some(output_rate_hz) } else { None },
            output_bits: if output_bits > 0 { Some(output_bits) } else { None },
            basic_rf_filter: basic_rf_filter != 0,
            is_rf: is_rf != 0,
            // Input format/rate/channels: sniffed from the file itself (magic
            // or extension); raw PCM derives its rate from the <n>msps hint.
            input_format: None,
            input_rate_hz: None,
            input_channels: None,
        };
        let r = chop::chop_with_options(i, o, start_samples, length_samples, opts);
        out.ok = if r.ok { 1 } else { 0 };
        out.exit_code = r.exit_code;
        set_str(&mut out.stderr_buf, &r.stderr);
    }
}

/// Write a `-cut` sibling output path for `in_path` into `out_buf`. Returns 1
/// on success, 0 if the buffer is too small or the path is unusable.
#[no_mangle]
pub extern "C" fn fc_generate_output_path(
    in_path: *const c_char,
    out_dir: *const c_char,
    stem: *const c_char,
    out_buf: *mut c_char,
    buf_len: usize,
) -> i32 {
    unsafe {
        if out_buf.is_null() || buf_len == 0 || in_path.is_null() {
            return 0;
        }
        let i = match CStr::from_ptr(in_path).to_str() {
            Ok(s) => s,
            Err(_) => return 0,
        };
        // out_dir may be NULL or empty → "next to the source".
        let d = if out_dir.is_null() {
            ""
        } else {
            match CStr::from_ptr(out_dir).to_str() {
                Ok(s) => s,
                Err(_) => return 0,
            }
        };
        // stem may be NULL or empty → "<inputstem>-cut" (stock behaviour).
        let s = if stem.is_null() {
            ""
        } else {
            match CStr::from_ptr(stem).to_str() {
                Ok(s) => s,
                Err(_) => return 0,
            }
        };
        let path = match chop::generate_output_path(i, d, s) {
            Some(p) => p,
            None => return 0,
        };
        let bytes = path.as_bytes();
        if bytes.len() + 1 > buf_len {
            return 0;
        }
        ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, out_buf, bytes.len());
        *out_buf.add(bytes.len()) = 0;
        1
    }
}

/// 1 if `sox` is on PATH and runnable, else 0.
#[no_mangle]
pub extern "C" fn fc_sox_available() -> i32 {
    if chop::sox_available() {
        1
    } else {
        0
    }
}

/// Request cancellation of the in-flight cut (if any). Called from the GUI
/// thread while `fc_chop` runs on a worker thread; the shell-out backend kills
/// the sox child promptly.
#[no_mangle]
pub extern "C" fn fc_chop_cancel() {
    chop::cancel_chop();
}

// --- Metadata editor (GUI Metadata Editor tab) -------------------------------
//
// Read / rewrite every Vorbis comment on a *source* FLAC in place, via
// tags::read_all_comments / tags::replace_all_comments. The read path packs
// the comments into a flat little-endian blob the C++ side parses:
//   u32 LE count, then for each comment: u32 LE length + "KEY=value" bytes.
// The vendor string is preserved automatically on write (not exposed here).

/// Copy `s` (truncated at a UTF-8 boundary) into a NUL-terminated buffer given
/// as a raw pointer + length. No-op on a null/empty buffer. Used by the
/// metadata-editor FFI to return error strings into caller-provided buffers.
fn set_str_ptr(buf: *mut c_char, buf_len: usize, s: &str) {
    if buf.is_null() || buf_len == 0 {
        return;
    }
    let bytes = s.as_bytes();
    let mut n = bytes.len().min(buf_len - 1);
    if n < bytes.len() {
        // Truncated: step back past any continuation bytes (10xxxxxx) so the
        // C++ side never receives a half character (QString::fromUtf8 would
        // render a replacement glyph).
        while n > 0 && (bytes[n] & 0b1100_0000) == 0b1000_0000 {
            n -= 1;
        }
    }
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, buf, n);
        *buf.add(n) = 0;
    }
}

/// Pack `(key, value)` pairs into the little-endian blob format the GUI parses:
/// `u32 LE count`, then per comment `u32 LE length` + `"KEY=value"` UTF-8 bytes.
/// Pure helper — used by both the size and read FFI calls so they agree exactly.
fn pack_comments_blob(comments: &[(String, String)]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&(comments.len() as u32).to_le_bytes());
    for (k, v) in comments {
        let s = format!("{}={}", k, v);
        let b = s.as_bytes();
        out.extend_from_slice(&(b.len() as u32).to_le_bytes());
        out.extend_from_slice(b);
    }
    out
}

/// Byte size of the packed Vorbis-comment blob for the FLAC at `path`
/// (4-byte count header + per-comment 4-byte length + "KEY=value"). Returns
/// `>= 4` on success (a tag-less file still yields a 4-byte count=0 blob) and
/// `0` on error (with the message written to `err`). The GUI sizes its buffer
/// with this, then calls [`fc_read_comments_blob`].
#[no_mangle]
pub extern "C" fn fc_comments_blob_size(path: *const c_char, err: *mut c_char, err_len: usize) -> usize {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<usize, String> {
        if path.is_null() {
            return Err("null path".to_string());
        }
        let p = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|_| "path is not valid UTF-8".to_string())?;
        let (_, comments) = tags::read_all_comments(std::path::Path::new(p))?;
        Ok(pack_comments_blob(&comments).len())
    }));
    match result {
        Ok(Ok(size)) => size,
        Ok(Err(e)) => {
            set_str_ptr(err, err_len, &e);
            0
        }
        Err(payload) => {
            set_str_ptr(err, err_len, &format!("read comments panicked: {}", panic_msg(payload)));
            0
        }
    }
}

/// Pack all Vorbis comments of the FLAC at `path` into `buf` (size `buf_len`),
/// which must be at least as large as [`fc_comments_blob_size`] reported.
/// Format: `u32 LE count`, then per comment `u32 LE length` + "KEY=value" bytes.
/// Returns 1 on success, 0 on error (message in `err`).
#[no_mangle]
pub extern "C" fn fc_read_comments_blob(
    path: *const c_char,
    buf: *mut c_char,
    buf_len: usize,
    err: *mut c_char,
    err_len: usize,
) -> i32 {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<(), String> {
        if path.is_null() || buf.is_null() {
            return Err("null path or buffer".to_string());
        }
        let p = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|_| "path is not valid UTF-8".to_string())?;
        let (_, comments) = tags::read_all_comments(std::path::Path::new(p))?;
        let blob = pack_comments_blob(&comments);
        if blob.len() > buf_len {
            return Err(format!("buffer too small: have {}, need {}", buf_len, blob.len()));
        }
        unsafe {
            std::ptr::copy_nonoverlapping(blob.as_ptr() as *const c_char, buf, blob.len());
        }
        Ok(())
    }));
    match result {
        Ok(Ok(())) => 1,
        Ok(Err(e)) => {
            set_str_ptr(err, err_len, &e);
            0
        }
        Err(payload) => {
            set_str_ptr(err, err_len, &format!("read comments panicked: {}", panic_msg(payload)));
            0
        }
    }
}

/// Replace ALL Vorbis comments on the FLAC at `path` with the `n` comments
/// pointed to by `comments` (each a NUL-terminated "KEY=value" C string). The
/// existing vendor string is preserved (or a default is used when the file has
/// no Vorbis comment block). Writes in place (adjacent padding / vendor trim)
/// with a verified temp-file splice fallback, exactly like the cut-tag rewrite.
/// Returns 1 on success, 0 on error (message in `err`).
#[no_mangle]
pub extern "C" fn fc_replace_comments(
    path: *const c_char,
    comments: *const *const c_char,
    n: u32,
    err: *mut c_char,
    err_len: usize,
) -> i32 {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<(), String> {
        if path.is_null() {
            return Err("null path".to_string());
        }
        let p = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|_| "path is not valid UTF-8".to_string())?;
        // Collect the C string array into owned (String) pairs, splitting each
        // "KEY=value" on the FIRST '=' (values may contain '=').
        let mut pairs: Vec<(String, String)> = Vec::with_capacity(n as usize);
        if !comments.is_null() {
            for i in 0..n {
                let pptr = unsafe { *comments.add(i as usize) };
                if pptr.is_null() {
                    return Err(format!("comment {i}: null pointer"));
                }
                let cstr = unsafe { CStr::from_ptr(pptr) }
                    .to_str()
                    .map_err(|_| format!("comment {i}: not valid UTF-8"))?;
                let (k, v) = match cstr.find('=') {
                    Some(eq) => (cstr[..eq].to_string(), cstr[eq + 1..].to_string()),
                    None => (cstr.to_string(), String::new()),
                };
                pairs.push((k, v));
            }
        }
        tags::replace_all_comments(std::path::Path::new(p), &pairs)
    }));
    match result {
        Ok(Ok(())) => 1,
        Ok(Err(e)) => {
            set_str_ptr(err, err_len, &e);
            0
        }
        Err(payload) => {
            set_str_ptr(err, err_len, &format!("replace comments panicked: {}", panic_msg(payload)));
            0
        }
    }
}

/// Compute the standard RF Vorbis-tag template (KEY=value pairs) from the probe
/// context, for a tagless RF FLAC that nonetheless carries enough context for
/// FLAC-Chop to derive the tags (STREAMINFO + filename MSPS + companion). This
/// is the "Apply Template" path: the values the capture tool would have written
/// but didn't.
///
/// Returns pairs in MISRC push order: `RF_SAMPLE_RATE_KHZ`, `RF_SAMPLE_RATE`,
/// then (when the total is known) `RF_TOTAL_SAMPLES`, `DURATION_SECONDS`,
/// `LENGTH`. Empty for non-RF or unknown-rate probes (the GUI gates the button
/// to RF captures anyway).
///
/// The real on-disk sample count is resolved from the probe's `total_samples`:
/// vorbis / companion / scan sources are already the real count; a trusted
/// STREAMINFO header may be in `/1000` units (early MISRC schema, e.g.
/// `2165571` for a 216.557 s / 10 MSPS capture) or real units (later schema,
/// `2165570800`). We decide with the same audio-payload sanity check the probe
/// uses for its "header total is suspect" warning: FLAC never expands, so the
/// real count's uncompressed size must be >= the audio payload; a `/1000` count
/// is ~1000× too small, so we scale it ×1000.
pub fn rf_template_from_probe(p: &FcProbe) -> Vec<(String, String)> {
    if p.is_rf == 0 || p.header_sample_rate == 0 || !(p.real_rate_hz > 0.0) {
        return Vec::new();
    }
    let header_rate = p.header_sample_rate;
    let real_rate = p.real_rate_hz;
    let mut pairs = vec![
        ("RF_SAMPLE_RATE_KHZ".to_string(), header_rate.to_string()),
        ("RF_SAMPLE_RATE".to_string(), format!("{}", real_rate as u64)),
    ];
    if p.total_samples_known != 0 && p.total_samples > 0 {
        // Vorbis / companion / frame-scan totals are already the real on-disk
        // count. A trusted STREAMINFO header may be /1000 (early) or real
        // (later) — scale only when the audio-payload check says /1000.
        let from_real = p.total_samples_from_vorbis != 0
            || p.total_samples_from_companion != 0
            || p.total_samples_scanned != 0;
        let mut real_total = p.total_samples;
        if !from_real {
            let bps = (p.channels as u64) * ((p.bits_per_sample as u64) / 8);
            let audio_bytes = p.file_size.saturating_sub(p.audio_offset);
            if bps > 0 && audio_bytes > 0 {
                let uncompressed = p.total_samples.saturating_mul(bps);
                // /1000-unit header: uncompressed is ~1000× too small for the
                // payload but ×1000 covers it. Scale to the real count.
                if uncompressed < audio_bytes && uncompressed.saturating_mul(1000) >= audio_bytes {
                    real_total = p.total_samples.saturating_mul(1000);
                }
            }
        }
        let duration = real_total as f64 / real_rate;
        let length_ms = (duration * 1000.0).round() as u64;
        pairs.push(("RF_TOTAL_SAMPLES".to_string(), real_total.to_string()));
        pairs.push(("DURATION_SECONDS".to_string(), format!("{duration:.6}")));
        pairs.push(("LENGTH".to_string(), length_ms.to_string()));
    }
    pairs
}

/// Compute the RF tag template from the probe and pack it into `buf` (same blob
/// format as [`fc_read_comments_blob`]: `u32 LE count` + per-comment `u32 LE
/// len` + "KEY=value"). Returns the number of bytes written (`>= 4`; a non-RF
/// probe yields a 4-byte count=0 blob) on success, `0` on error (null pointers
/// or buffer too small — message in `err`). The template is bounded (<= 5
/// pairs) so a 4096-byte buffer is always enough. The GUI merges the pairs into
/// the editor table (only missing keys), adds blank ingest rows, and the user
/// hits Save to write.
#[no_mangle]
pub extern "C" fn fc_rf_template_from_probe(
    probe: *const FcProbe,
    buf: *mut c_char,
    buf_len: usize,
    err: *mut c_char,
    err_len: usize,
) -> usize {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<usize, String> {
        if probe.is_null() || buf.is_null() {
            return Err("null probe or buffer".to_string());
        }
        let p = unsafe { &*probe };
        let blob = pack_comments_blob(&rf_template_from_probe(p));
        if blob.len() > buf_len {
            return Err(format!("buffer too small: have {}, need {}", buf_len, blob.len()));
        }
        unsafe {
            std::ptr::copy_nonoverlapping(blob.as_ptr() as *const c_char, buf, blob.len());
        }
        Ok(blob.len())
    }));
    match result {
        Ok(Ok(n)) => n,
        Ok(Err(e)) => {
            set_str_ptr(err, err_len, &e);
            0
        }
        Err(payload) => {
            set_str_ptr(err, err_len, &format!("rf template panicked: {}", panic_msg(payload)));
            0
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cbuf_to_string(buf: &[c_char]) -> String {
        let bytes: Vec<u8> = buf.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
        String::from_utf8_lossy(&bytes).into_owned()
    }

    #[test]
    fn plan_basic_20msps() {
        let mut out = FcPlan::default();
        // 10 s at 20 MSPS out of a 200 s file — the validated reference case.
        fc_plan(60.0, 10.0, 20_000_000.0, 4_000_000_000, 1, &mut out);
        assert_eq!(out.ok, 1, "{}", cbuf_to_string(&out.error));
        assert_eq!(out.start_samples, 1_200_000_000);
        assert_eq!(out.length_samples, 200_000_000);
        assert_eq!(out.end_sample, 1_400_000_000);
    }

    #[test]
    fn plan_clamps_length_to_eof() {
        let mut out = FcPlan::default();
        fc_plan(9.0, 5.0, 1000.0, 10_000, 1, &mut out);
        assert_eq!(out.ok, 1);
        assert_eq!(out.start_samples, 9_000);
        assert_eq!(out.length_samples, 1_000); // clamped to the file end
    }

    #[test]
    fn plan_start_past_eof_is_an_error_not_a_one_sample_cut() {
        // Previously this clamped start to EOF and then forced len to 1,
        // asking SoX for a cut past the end of the file.
        let mut out = FcPlan::default();
        fc_plan(20.0, 5.0, 1000.0, 10_000, 1, &mut out);
        assert_eq!(out.ok, 0);
        assert!(cbuf_to_string(&out.error).contains("end of the file"));
        // Exactly at EOF is also an empty cut → error.
        let mut out2 = FcPlan::default();
        fc_plan(10.0, 5.0, 1000.0, 10_000, 1, &mut out2);
        assert_eq!(out2.ok, 0);
    }

    #[test]
    fn plan_rejects_non_finite_inputs() {
        let mut out = FcPlan::default();
        fc_plan(f64::NAN, 5.0, 1000.0, 10_000, 1, &mut out);
        assert_eq!(out.ok, 0);
        let mut out2 = FcPlan::default();
        fc_plan(0.0, f64::INFINITY, 1000.0, 10_000, 1, &mut out2);
        assert_eq!(out2.ok, 0);
    }

    #[test]
    fn set_str_truncates_on_char_boundary() {
        // "ééé…" in a 4-byte buffer (3 usable): must keep exactly one 2-byte
        // 'é', never a dangling half character.
        let mut buf = [0 as c_char; 4];
        set_str(&mut buf, "ééé");
        let s = cbuf_to_string(&buf);
        assert_eq!(s, "é");
    }

    #[test]
    fn set_str_no_truncation_untouched() {
        let mut buf = [0 as c_char; 16];
        set_str(&mut buf, "hello");
        assert_eq!(cbuf_to_string(&buf), "hello");
    }

    // --- metadata-editor FFI -------------------------------------------------

    /// Build a minimal FLAC (10000 Hz / 1 ch / 8-bit) with a VORBIS_COMMENT
    /// block carrying `comments`, a 4-byte PADDING, and one fake frame — enough
    /// for the read/replace FFI to operate on (the splice self-check verifies
    /// the frame sync, not full decodability).
    fn make_meta_test_flac(name: &str, comments: &[&str]) -> std::path::PathBuf {
        use std::io::Write;
        let p = std::env::temp_dir().join(name);
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(b"fLaC").unwrap();
        // STREAMINFO: 10000 Hz, 1 ch, 8 bps, total 4096 (one block).
        let packed: u64 = (10000u64 << 44) | (0u64 << 41) | (7u64 << 36) | 4096;
        let mut si = Vec::new();
        si.extend_from_slice(&4096u16.to_be_bytes());
        si.extend_from_slice(&4096u16.to_be_bytes());
        si.extend_from_slice(&[0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
        si.extend_from_slice(&packed.to_be_bytes());
        si.extend_from_slice(&[0u8; 16]);
        f.write_all(&[0x00, 0x00, 0x00, 34]).unwrap();
        f.write_all(&si).unwrap();
        // VORBIS_COMMENT (vendor "test" + the requested comments).
        let mut vc = Vec::new();
        let vendor = b"test";
        vc.extend_from_slice(&(vendor.len() as u32).to_le_bytes());
        vc.extend_from_slice(vendor);
        vc.extend_from_slice(&(comments.len() as u32).to_le_bytes());
        for c in comments {
            let b = c.as_bytes();
            vc.extend_from_slice(&(b.len() as u32).to_le_bytes());
            vc.extend_from_slice(b);
        }
        f.write_all(&[0x04, (vc.len() >> 16) as u8, (vc.len() >> 8) as u8, vc.len() as u8]).unwrap();
        f.write_all(&vc).unwrap();
        // PADDING, last block.
        f.write_all(&[0x81, 0x00, 0x00, 0x04]).unwrap();
        f.write_all(&[0u8; 4]).unwrap();
        // One fake frame: 0xFFF8 sync + junk payload.
        f.write_all(&[0xFF, 0xF8, 0xCC, 0x02, 0x00, 0x0A, 0x1D, 0x4A]).unwrap();
        p
    }

    fn parse_blob(buf: &[c_char]) -> Vec<String> {
        let bytes: &[u8] =
            unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, buf.len()) };
        let mut pos = 0usize;
        let count = u32::from_le_bytes([bytes[pos], bytes[pos + 1], bytes[pos + 2], bytes[pos + 3]])
            as usize;
        pos += 4;
        let mut out = Vec::with_capacity(count);
        for _ in 0..count {
            let len = u32::from_le_bytes([bytes[pos], bytes[pos + 1], bytes[pos + 2], bytes[pos + 3]])
                as usize;
            pos += 4;
            out.push(std::str::from_utf8(&bytes[pos..pos + len]).unwrap().to_string());
            pos += len;
        }
        out
    }

    #[test]
    fn pack_comments_blob_roundtrips() {
        let comments = vec![
            ("PROJECT".to_string(), "demo".to_string()),
            ("NOTES".to_string(), "a=b=c".to_string()), // value with '='
            ("EMPTY".to_string(), String::new()),
        ];
        let blob = pack_comments_blob(&comments);
        let pairs: Vec<(String, String)> = parse_blob(
            &blob.iter().map(|&b| b as c_char).collect::<Vec<_>>(),
        )
        .into_iter()
        .map(|s| match s.find('=') {
            Some(i) => (s[..i].to_string(), s[i + 1..].to_string()),
            None => (s, String::new()),
        })
        .collect();
        assert_eq!(pairs, comments);
    }

    #[test]
    fn fc_comments_blob_size_errors_on_missing_file() {
        let mut err = [0 as c_char; 128];
        let path = b"/nonexistent/fc_no_such.flac\0";
        let size = fc_comments_blob_size(path.as_ptr() as *const c_char, err.as_mut_ptr(), err.len());
        assert_eq!(size, 0, "missing file must report size 0");
        assert!(err[0] != 0, "an error message must be written");
        assert!(
            cbuf_to_string(&err).to_lowercase().contains("open"),
            "error should mention the open failure: {}",
            cbuf_to_string(&err)
        );
    }

    #[test]
    fn fc_read_comments_blob_errors_on_missing_file() {
        let mut err = [0 as c_char; 128];
        let mut buf = [0 as c_char; 64];
        let path = b"/nonexistent/fc_no_such2.flac\0";
        let ok = fc_read_comments_blob(
            path.as_ptr() as *const c_char,
            buf.as_mut_ptr(),
            buf.len(),
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(ok, 0);
        assert!(err[0] != 0);
    }

    #[test]
    fn fc_replace_comments_errors_on_missing_file() {
        let mut err = [0 as c_char; 128];
        let path = b"/nonexistent/fc_no_such3.flac\0";
        let ok = fc_replace_comments(
            path.as_ptr() as *const c_char,
            std::ptr::null(),
            0,
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(ok, 0);
        assert!(err[0] != 0);
    }

    #[test]
    fn fc_read_and_replace_comments_end_to_end() {
        let p = make_meta_test_flac("fc_ffi_meta.flac", &["PROJECT=demo", "OPERATOR=harry"]);
        let path_b = p.to_string_lossy().into_owned() + "\0";
        let mut err = [0 as c_char; 128];

        // size the blob
        let size = fc_comments_blob_size(path_b.as_ptr() as *const c_char, err.as_mut_ptr(), err.len());
        assert!(err[0] == 0, "{}", cbuf_to_string(&err));
        assert!(size >= 4);

        // read it
        let mut buf = vec![0 as c_char; size];
        let ok = fc_read_comments_blob(
            path_b.as_ptr() as *const c_char,
            buf.as_mut_ptr(),
            buf.len(),
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(ok, 1, "{}", cbuf_to_string(&err));
        let got = parse_blob(&buf);
        assert_eq!(got, vec!["PROJECT=demo".to_string(), "OPERATOR=harry".to_string()]);

        // replace: drop OPERATOR, add NOTES, change PROJECT
        let new_comments: Vec<std::ffi::CString> = vec![
            std::ffi::CString::new("PROJECT=demo2").unwrap(),
            std::ffi::CString::new("NOTES=edited").unwrap(),
        ];
        let ptrs: Vec<*const c_char> = new_comments.iter().map(|c| c.as_ptr()).collect();
        let ok2 = fc_replace_comments(
            path_b.as_ptr() as *const c_char,
            ptrs.as_ptr(),
            ptrs.len() as u32,
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(ok2, 1, "{}", cbuf_to_string(&err));

        // read back the written file
        let size2 = fc_comments_blob_size(path_b.as_ptr() as *const c_char, err.as_mut_ptr(), err.len());
        assert!(err[0] == 0);
        let mut buf2 = vec![0 as c_char; size2];
        let ok3 = fc_read_comments_blob(
            path_b.as_ptr() as *const c_char,
            buf2.as_mut_ptr(),
            buf2.len(),
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(ok3, 1);
        let got2 = parse_blob(&buf2);
        assert!(got2.contains(&"PROJECT=demo2".to_string()));
        assert!(got2.contains(&"NOTES=edited".to_string()));
        assert!(!got2.contains(&"OPERATOR=harry".to_string()), "removed key must be gone");

        let _ = std::fs::remove_file(&p);
    }

    // --- Apply Template (rf_template_from_probe) ----------------------------

    fn make_probe(
        is_rf: i32,
        header_rate: u64,
        real_rate: f64,
        total: u64,
        known: i32,
        from_vorbis: i32,
        from_companion: i32,
        scanned: i32,
        file_size: u64,
        audio_offset: u64,
        bits: u32,
        channels: u32,
    ) -> FcProbe {
        let mut p = FcProbe::default();
        p.ok = 1;
        p.is_rf = is_rf;
        p.header_sample_rate = header_rate;
        p.real_rate_hz = real_rate;
        p.total_samples = total;
        p.total_samples_known = known;
        p.total_samples_from_vorbis = from_vorbis;
        p.total_samples_from_companion = from_companion;
        p.total_samples_scanned = scanned;
        p.file_size = file_size;
        p.audio_offset = audio_offset;
        p.bits_per_sample = bits;
        p.channels = channels;
        p
    }

    fn template_map(p: &FcProbe) -> std::collections::HashMap<String, String> {
        rf_template_from_probe(p).into_iter().collect()
    }

    #[test]
    fn rf_template_vorbis_total_used_as_is() {
        // 10 MSPS HiFi: vorbis RF_TOTAL_SAMPLES = 2,165,570,800 (already the
        // real on-disk count). Template must use it without scaling.
        let p = make_probe(
            1, 10000, 10_000_000.0, 2_165_570_800, 1, 1, 0, 0,
            972_426_305, 100, 8, 1,
        );
        let m = template_map(&p);
        assert_eq!(m["RF_SAMPLE_RATE_KHZ"], "10000");
        assert_eq!(m["RF_SAMPLE_RATE"], "10000000");
        assert_eq!(m["RF_TOTAL_SAMPLES"], "2165570800");
        assert_eq!(m["DURATION_SECONDS"], "216.557080");
        assert_eq!(m["LENGTH"], "216557");
    }

    #[test]
    fn rf_template_early_schema_header_scaled_x1000() {
        // Tagless early-schema RF: STREAMINFO = 2,165,571 (/1000 count). The
        // audio-payload sanity check (uncompressed < audio_bytes, *1000 fits)
        // detects /1000 units and scales to the real count.
        let p = make_probe(
            1, 10000, 10_000_000.0, 2_165_571, 1, 0, 0, 0,
            972_426_305, 100, 8, 1,
        );
        let m = template_map(&p);
        assert_eq!(m["RF_TOTAL_SAMPLES"], "2165571000", "early-schema header must be *1000");
        assert_eq!(m["DURATION_SECONDS"], "216.557100");
        assert_eq!(m["LENGTH"], "216557");
    }

    #[test]
    fn rf_template_later_schema_header_not_scaled() {
        // Tagless later-schema RF: STREAMINFO = 2,165,570,800 (real count). The
        // uncompressed size already exceeds the audio payload → no scaling.
        let p = make_probe(
            1, 10000, 10_000_000.0, 2_165_570_800, 1, 0, 0, 0,
            972_426_305, 100, 8, 1,
        );
        let m = template_map(&p);
        assert_eq!(m["RF_TOTAL_SAMPLES"], "2165570800", "later-schema header must not scale");
        assert_eq!(m["DURATION_SECONDS"], "216.557080");
    }

    #[test]
    fn rf_template_unknown_total_only_rate_tags() {
        // Unfinalized capture, no companion, no scan: total unknown → only the
        // two rate tags (the rate is still known from the header / filename).
        let p = make_probe(1, 10000, 10_000_000.0, 0, 0, 0, 0, 0, 1000, 100, 8, 1);
        let pairs = rf_template_from_probe(&p);
        assert_eq!(pairs.len(), 2, "only KHZ + RATE when total unknown");
        assert!(pairs.iter().any(|(k, _)| k == "RF_SAMPLE_RATE_KHZ"));
        assert!(pairs.iter().any(|(k, _)| k == "RF_SAMPLE_RATE"));
        assert!(!pairs.iter().any(|(k, _)| k == "RF_TOTAL_SAMPLES"));
        assert!(!pairs.iter().any(|(k, _)| k == "DURATION_SECONDS"));
        assert!(!pairs.iter().any(|(k, _)| k == "LENGTH"));
    }

    #[test]
    fn rf_template_non_rf_is_empty() {
        // The RF tag schema is for RF captures — a 48 kHz stereo audio file
        // yields no template (the GUI gates the button to RF only).
        let p = make_probe(0, 48000, 48000.0, 1000, 1, 0, 0, 0, 1000, 100, 16, 2);
        assert!(rf_template_from_probe(&p).is_empty());
    }

    #[test]
    fn rf_template_companion_total_is_real() {
        // Companion-derived total = duration * real_rate = real count; no
        // /1000 scaling applies (from_companion path).
        let p = make_probe(
            1, 20000, 20_000_000.0, 8_807_960_000, 1, 0, 1, 0,
            40_000_000_000, 200, 8, 1,
        );
        let m = template_map(&p);
        assert_eq!(m["RF_TOTAL_SAMPLES"], "8807960000");
        assert_eq!(m["RF_SAMPLE_RATE_KHZ"], "20000");
        assert_eq!(m["RF_SAMPLE_RATE"], "20000000");
        assert_eq!(m["DURATION_SECONDS"], "440.398000");
    }

    #[test]
    fn fc_rf_template_from_probe_packs_blob() {
        let p = make_probe(
            1, 10000, 10_000_000.0, 2_165_570_800, 1, 1, 0, 0,
            972_426_305, 100, 8, 1,
        );
        let mut buf = [0 as c_char; 4096];
        let mut err = [0 as c_char; 128];
        let n = fc_rf_template_from_probe(
            &p as *const FcProbe,
            buf.as_mut_ptr(),
            buf.len(),
            err.as_mut_ptr(),
            err.len(),
        );
        assert!(n > 0, "{}", cbuf_to_string(&err));
        // Parse the blob and check it carries the expected pairs.
        let entries = parse_blob(&buf[..n]);
        assert!(entries.iter().any(|e| e == "RF_SAMPLE_RATE_KHZ=10000"), "{:?}", entries);
        assert!(entries.iter().any(|e| e == "RF_SAMPLE_RATE=10000000"));
        assert!(entries.iter().any(|e| e == "RF_TOTAL_SAMPLES=2165570800"));
        assert!(entries.iter().any(|e| e == "DURATION_SECONDS=216.557080"));
        assert!(entries.iter().any(|e| e == "LENGTH=216557"));
    }

    #[test]
    fn fc_rf_template_from_probe_non_rf_yields_empty_blob() {
        // Non-RF → empty template → the blob is just the 4-byte count=0
        // header (still a valid blob, not an error).
        let p = make_probe(0, 48000, 48000.0, 1000, 1, 0, 0, 0, 1000, 100, 16, 2);
        let mut buf = [0 as c_char; 4096];
        let mut err = [0 as c_char; 128];
        let n = fc_rf_template_from_probe(
            &p as *const FcProbe,
            buf.as_mut_ptr(),
            buf.len(),
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(n, 4, "empty template packs to a 4-byte count=0 blob");
        let entries = parse_blob(&buf[..n]);
        assert!(entries.is_empty());
    }

    #[test]
    fn fc_rf_template_from_probe_null_probe_is_zero() {
        let mut buf = [0 as c_char; 4096];
        let mut err = [0 as c_char; 128];
        let n = fc_rf_template_from_probe(
            std::ptr::null(),
            buf.as_mut_ptr(),
            buf.len(),
            err.as_mut_ptr(),
            err.len(),
        );
        assert_eq!(n, 0);
        assert!(err[0] != 0, "an error message must be written");
    }
}
