//! Infer a capture's real duration from companion files that share its base
//! name, as a fast exact alternative to scanning FLAC frame headers when the
//! STREAMINFO `total_samples` is 0/unknown (an unfinalized/piped capture).
//!
//! The DdD / cxadc / MISRC pipeline writes several siblings per capture, all
//! sharing a base prefix through the timestamp, e.g. for an RF file
//! `..._2026.07.10_22.32.43_video_rf_8-bit_20msps.flac` there are usually:
//!   `<base>_misrc_capture.log`        — contains an explicit `duration=Ns` line
//!   `<base>_baseband_stereo_ch1_ch2.wav` — finalized WAV header (exact frames)
//!   `<base>_video_rf_8-bit_10msps.flac`  — a different-MSPS RF with a known total
//!
//! All of these agree on the real duration (verified on Tape_12: 01:42:00 from
//! four independent sources). We prefer the log (smallest, most explicit), then
//! a WAV header, then fall back to the caller's frame scan.

use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

/// Extract the capture base prefix: everything up to and including the first
/// `YYYY.MM.DD_HH.MM.SS` timestamp in the file stem. Companions share this
/// prefix. Returns the full stem if no timestamp is found (callers then require
/// a sibling that starts with the whole stem, which still matches same-named
/// companions that only differ by a trailing suffix).
fn capture_base(stem: &str) -> &str {
    let b = stem.as_bytes();
    let mut i = 0;
    while i + 19 <= b.len() {
        // YYYY.MM.DD_HH.MM.SS  (10 date + 1 sep + 8 time = 19)
        let is_ts = b[i].is_ascii_digit()
            && b[i + 1].is_ascii_digit()
            && b[i + 2].is_ascii_digit()
            && b[i + 3].is_ascii_digit()
            && b[i + 4] == b'.'
            && b[i + 5].is_ascii_digit()
            && b[i + 6].is_ascii_digit()
            && b[i + 7] == b'.'
            && b[i + 8].is_ascii_digit()
            && b[i + 9].is_ascii_digit()
            && b[i + 10] == b'_'
            && b[i + 11].is_ascii_digit()
            && b[i + 12].is_ascii_digit()
            && b[i + 13] == b'.'
            && b[i + 14].is_ascii_digit()
            && b[i + 15].is_ascii_digit()
            && b[i + 16] == b'.'
            && b[i + 17].is_ascii_digit()
            && b[i + 18].is_ascii_digit();
        if is_ts {
            return &stem[..i + 19];
        }
        i += 1;
    }
    stem
}

/// Parse a `duration=<float>s` (or `duration=<float> s`) line out of a log
/// file's text. Returns the value in seconds. Tolerant of whitespace and of
/// either `duration=` or `Duration:` prefixes; scans only the first ~64 KB so
/// even a multi-MB log is cheap.
fn parse_log_duration(text: &str) -> Option<f64> {
    let needle = "duration";
    let bytes = text.as_bytes();
    let limit = bytes.len().min(64 * 1024);
    let mut i = 0;
    while i < limit {
        // find next occurrence of "duration" (case-insensitive)
        if i + needle.len() <= limit
            && bytes[i..i + needle.len()].eq_ignore_ascii_case(needle.as_bytes())
        {
            let mut j = i + needle.len();
            // skip optional ':' / '=' / whitespace
            while j < limit && (bytes[j] == b':' || bytes[j] == b'=' || bytes[j].is_ascii_whitespace())
            {
                j += 1;
            }
            // read a number
            let num_start = j;
            while j < limit && (bytes[j].is_ascii_digit() || bytes[j] == b'.') {
                j += 1;
            }
            if j > num_start {
                if let Ok(s) = std::str::from_utf8(&bytes[num_start..j]) {
                    if let Ok(v) = s.parse::<f64>() {
                        return Some(v);
                    }
                }
            }
        }
        i += 1;
    }
    None
}

/// Parse a RIFF/WAVE header from `path` and return the duration in seconds.
/// Handles the common PCM case; returns None for RF64 or anything it can't
/// make sense of (the caller falls back to another source). Only the first
/// ~128 bytes are read, so even a multi-GB WAV is cheap to probe.
fn parse_wav_duration(path: &Path) -> Option<f64> {
    let mut f = fs::File::open(path).ok()?;
    let mut head = [0u8; 128];
    let n = f.read(&mut head).ok()?;
    let h = &head[..n];
    if n < 12 || &h[0..4] != b"RIFF" || &h[8..12] != b"WAVE" {
        return None; // not a plain RIFF WAVE (RF64 etc. unsupported here)
    }
    // Walk chunks: fmt (1) for rate/ch/bps, data for size.
    let mut i = 12usize;
    let mut sample_rate: Option<u32> = None;
    let mut channels: Option<u16> = None;
    let mut bits_per_sample: Option<u16> = None;
    let mut data_size: Option<u32> = None;
    while i + 8 <= h.len() {
        let id = &h[i..i + 4];
        let sz = u32::from_le_bytes([h[i + 4], h[i + 5], h[i + 6], h[i + 7]]) as usize;
        let body = i + 8;
        if id == b"fmt " && body + 16 <= h.len() {
            channels = Some(u16::from_le_bytes([h[body + 2], h[body + 3]]));
            sample_rate = Some(u32::from_le_bytes([
                h[body + 4],
                h[body + 5],
                h[body + 6],
                h[body + 7],
            ]));
            bits_per_sample = Some(u16::from_le_bytes([h[body + 14], h[body + 15]]));
        } else if id == b"data" {
            data_size = Some(sz as u32);
            // data is usually last and huge; we don't need to walk past it.
            break;
        }
        // chunks are word-aligned; advance past body + pad
        let next = body + sz + (sz & 1);
        if next <= i {
            break;
        }
        i = next;
    }
    let sr = sample_rate? as f64;
    let ch = channels? as f64;
    let bps = bits_per_sample? as f64;
    let ds = data_size? as f64;
    if sr <= 0.0 || ch <= 0.0 || bps <= 0.0 {
        return None;
    }
    let frames = ds / (ch * (bps / 8.0));
    Some(frames / sr)
}

/// Sibling files in `dir` whose stem starts with `base` and end in `ext`.
fn siblings_with_ext(dir: &Path, base: &str, ext: &str) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = fs::read_dir(dir) {
        for ent in rd.flatten() {
            let p = ent.path();
            if let Some(x) = p.extension() {
                if x.eq_ignore_ascii_case(ext) {
                    if let Some(stem) = p.file_stem().and_then(|s| s.to_str()) {
                        if stem.starts_with(base) {
                            out.push(p);
                        }
                    }
                }
            }
        }
    }
    out
}

/// Try to infer the real capture duration (seconds) from a companion file.
/// Order: sibling `*.log` with a `duration=Ns` line, then a sibling `.wav`
/// RIFF header. Returns None if no companion yields a value.
pub fn companion_duration(rf_path: &Path) -> Option<f64> {
    let dir = rf_path.parent()?;
    let stem = rf_path.file_stem()?.to_str()?;
    let base = capture_base(stem);

    // 1) logs — prefer one that actually contains a duration line.
    for log in siblings_with_ext(dir, base, "log") {
        if let Ok(bytes) = fs::read(&log) {
            // bound the read: only scan up to 64 KB of the file
            let cap = &bytes[..bytes.len().min(64 * 1024)];
            if let Ok(text) = std::str::from_utf8(cap) {
                if let Some(secs) = parse_log_duration(text) {
                    if secs > 0.0 {
                        return Some(secs);
                    }
                }
            }
        }
    }

    // 2) WAV header.
    for wav in siblings_with_ext(dir, base, "wav") {
        if let Some(secs) = parse_wav_duration(&wav) {
            if secs > 0.0 {
                return Some(secs);
            }
        }
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn capture_base_finds_timestamp() {
        assert_eq!(
            capture_base("VHS_PAL_SP_Tape_12_2026.07.10_22.32.43_video_rf_8-bit_20msps"),
            "VHS_PAL_SP_Tape_12_2026.07.10_22.32.43"
        );
    }

    #[test]
    fn capture_base_no_timestamp_returns_whole_stem() {
        assert_eq!(capture_base("plain_file"), "plain_file");
    }

    #[test]
    fn parse_log_duration_misrc_format() {
        let log = "...Recording stopped: duration=6120.27s (01.42.00) rawA=...";
        assert!((parse_log_duration(log).unwrap() - 6120.27).abs() < 1e-6);
    }

    #[test]
    fn parse_log_duration_case_insensitive_and_colon() {
        let log = "Capture Duration: 3517.5 seconds";
        // "duration" found, then ':', then 3517.5
        assert!((parse_log_duration(log).unwrap() - 3517.5).abs() < 1e-6);
    }

    #[test]
    fn parse_log_duration_absent_returns_none() {
        assert_eq!(parse_log_duration("no timing info here"), None);
    }
}
