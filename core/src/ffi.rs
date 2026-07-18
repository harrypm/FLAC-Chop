//! C ABI surface for the Qt6 GUI.
//!
//! All structs are `#[repr(C)]` with fixed-size error buffers so the C++ side
//! can include a matching `flacchop.h` and link the staticlib directly. Strings
//! cross the boundary as NUL-terminated `const char*`; outputs are written into
//! caller-provided fixed-size arrays to avoid any allocation across the ABI.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;

use crate::{chop, msps, probe};

/// Copy `s` (truncated) into a NUL-terminated fixed `[c_char; N]` buffer.
fn set_str(buf: &mut [c_char], s: &str) {
    if buf.is_empty() {
        return;
    }
    let bytes = s.as_bytes();
    let n = bytes.len().min(buf.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        buf[i] = *b as c_char;
    }
    buf[n] = 0;
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
            let msg = if let Some(s) = payload.downcast_ref::<&'static str>() {
                (*s).to_string()
            } else if let Some(s) = payload.downcast_ref::<String>() {
                s.clone()
            } else {
                "probe panicked (unknown cause)".to_string()
            };
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

/// Compute a sample-exact cut plan from seconds. `real_rate_hz` is the already-
/// resolved real sample rate (from `fc_probe`'s `real_rate_hz` field — header
/// ×1000 for RF captures, or the header rate for real audio). When total
/// samples are known, the cut is clamped to the file.
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

        let real_rate = real_rate_hz;
        if !(real_rate > 0.0) {
            set_str(&mut out.error, "sample rate is zero");
            return;
        }
        if len_sec <= 0.0 {
            set_str(&mut out.error, "length must be > 0");
            return;
        }
        if start_sec < 0.0 {
            set_str(&mut out.error, "start must be >= 0");
            return;
        }

        let mut start_s = (start_sec * real_rate).round() as u64;
        let mut len_s = (len_sec * real_rate).round() as u64;

        if total_known != 0 {
            out.real_total_seconds = total_samples as f64 / real_rate;
            if start_s > total_samples {
                start_s = total_samples;
            }
            if start_s + len_s > total_samples {
                len_s = total_samples.saturating_sub(start_s);
            }
        }
        if len_s == 0 {
            len_s = 1;
        }

        out.start_samples = start_s;
        out.length_samples = len_s;
        out.end_sample = start_s.saturating_add(len_s);
        out.real_sample_rate_hz = real_rate;
        out.ok = 1;
    }
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

/// Run the SoX cut. Blocking — the GUI calls this on a worker thread.
#[no_mangle]
pub extern "C" fn fc_chop(
    in_path: *const c_char,
    out_path: *const c_char,
    start_samples: u64,
    length_samples: u64,
    out: *mut FcChopResult,
) {
    unsafe {
        if out.is_null() {
            return;
        }
        let out = &mut *out;
        *out = FcChopResult::default();

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

        let r = chop::chop(i, o, start_samples, length_samples);
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
        let path = match chop::generate_output_path(i) {
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
