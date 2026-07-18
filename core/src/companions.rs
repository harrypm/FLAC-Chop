//! Infer a capture's real duration from companion files that share its base
//! name, as a fast exact alternative to scanning FLAC frame headers when the
//! STREAMINFO `total_samples` is 0/unknown (an unfinalized/piped capture).
//!
//! The DdD / cxadc / MISRC pipeline writes several siblings per capture, all
//! sharing a base prefix through the timestamp, e.g. for an RF file
//! `..._2026.07.10_22.32.43_video_rf_8-bit_20msps.flac` there are usually:
//!   `<base>_misrc_capture.log`        — contains an explicit `duration=Ns` line
//!   `<base>_baseband_stereo_ch1_ch2.wav` — finalized WAV header (exact frames)
//!
//! These agree on the real duration (verified on Tape_12: 01:42:00 from
//! independent sources). We prefer the log (smallest, most explicit), then a
//! WAV header, then fall back to the caller's frame scan.

use std::fs;
use std::io::{Read, Seek, SeekFrom};
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
/// Walks the chunk list with seeks, so `bext` (Broadcast WAV), `LIST`, `JUNK`
/// and other pre-`data` chunks of any size are handled. Handles the common PCM
/// case; returns None for RF64, unfinalized/streamed data sizes (0 or
/// 0xFFFFFFFF), or anything it can't make sense of (the caller falls back to
/// another source). Only chunk headers and the 16-byte fmt body are read, so
/// even a multi-GB WAV is cheap to probe.
fn parse_wav_duration(path: &Path) -> Option<f64> {
    let mut f = fs::File::open(path).ok()?;
    let mut riff = [0u8; 12];
    f.read_exact(&mut riff).ok()?;
    if &riff[0..4] != b"RIFF" || &riff[8..12] != b"WAVE" {
        return None; // not a plain RIFF WAVE (RF64 etc. unsupported here)
    }
    let mut sample_rate: Option<u32> = None;
    let mut channels: Option<u16> = None;
    let mut bits_per_sample: Option<u16> = None;
    let mut data_size: Option<u64> = None;
    // Bound the walk: no sane WAV has hundreds of chunks before `data`.
    for _ in 0..256 {
        let mut hdr = [0u8; 8];
        if f.read_exact(&mut hdr).is_err() {
            break; // clean EOF before a data chunk
        }
        let id = [hdr[0], hdr[1], hdr[2], hdr[3]];
        let sz = u32::from_le_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]);
        if &id == b"fmt " {
            if sz < 16 {
                return None; // malformed fmt chunk
            }
            let mut fmt = [0u8; 16];
            f.read_exact(&mut fmt).ok()?;
            channels = Some(u16::from_le_bytes([fmt[2], fmt[3]]));
            sample_rate = Some(u32::from_le_bytes([fmt[4], fmt[5], fmt[6], fmt[7]]));
            bits_per_sample = Some(u16::from_le_bytes([fmt[14], fmt[15]]));
            // Skip any fmt extension plus the word-alignment pad byte.
            let rest = (sz as u64 - 16) + u64::from(sz & 1);
            f.seek(SeekFrom::Current(rest as i64)).ok()?;
        } else if &id == b"data" {
            // 0 or 0xFFFFFFFF means the writer never finalized the header
            // (streamed / interrupted capture) — the size is meaningless.
            if sz == 0 || sz == u32::MAX {
                return None;
            }
            data_size = Some(u64::from(sz));
            break; // data is what we came for; no need to walk further
        } else {
            // Any other chunk (JUNK, LIST, bext, fact, …): skip body + pad.
            let skip = u64::from(sz) + u64::from(sz & 1);
            f.seek(SeekFrom::Current(skip as i64)).ok()?;
        }
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
        if let Ok(f) = fs::File::open(&log) {
            // Bound the READ (not just the scan) to 64 KB so a multi-MB log is
            // never loaded whole, and decode lossily so a multibyte character
            // split at the cap doesn't discard the log.
            let mut bytes = Vec::with_capacity(64 * 1024);
            if f.take(64 * 1024).read_to_end(&mut bytes).is_ok() {
                let text = String::from_utf8_lossy(&bytes);
                if let Some(secs) = parse_log_duration(&text) {
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

    use std::io::Write;

    /// Build a minimal WAV: RIFF/WAVE, optional junk chunks, PCM fmt, data.
    fn make_wav(pre_chunks: &[(&[u8; 4], usize)], data_size: u32) -> Vec<u8> {
        let mut v: Vec<u8> = Vec::new();
        v.extend_from_slice(b"RIFF");
        v.extend_from_slice(&0u32.to_le_bytes()); // riff size, unused by parser
        v.extend_from_slice(b"WAVE");
        for (id, sz) in pre_chunks {
            v.extend_from_slice(*id);
            v.extend_from_slice(&(*sz as u32).to_le_bytes());
            v.extend(std::iter::repeat(0u8).take(sz + (sz & 1)));
        }
        // fmt chunk: PCM, 2 ch, 48000 Hz, 16-bit
        v.extend_from_slice(b"fmt ");
        v.extend_from_slice(&16u32.to_le_bytes());
        v.extend_from_slice(&1u16.to_le_bytes()); // PCM
        v.extend_from_slice(&2u16.to_le_bytes()); // channels
        v.extend_from_slice(&48000u32.to_le_bytes()); // rate
        v.extend_from_slice(&(48000u32 * 4).to_le_bytes()); // byte rate
        v.extend_from_slice(&4u16.to_le_bytes()); // block align
        v.extend_from_slice(&16u16.to_le_bytes()); // bits
        v.extend_from_slice(b"data");
        v.extend_from_slice(&data_size.to_le_bytes());
        // parser never reads the data body, so we can omit it
        v
    }

    fn write_temp(name: &str, bytes: &[u8]) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(name);
        let mut f = fs::File::create(&p).unwrap();
        f.write_all(bytes).unwrap();
        p
    }

    #[test]
    fn wav_duration_plain() {
        // 1 second of 48 kHz 16-bit stereo = 192000 bytes.
        let p = write_temp("fc_test_plain.wav", &make_wav(&[], 192_000));
        let d = parse_wav_duration(&p).unwrap();
        assert!((d - 1.0).abs() < 1e-9);
    }

    #[test]
    fn wav_duration_with_bext_and_junk_before_data() {
        // Broadcast WAVs carry a 602+ byte bext chunk, and many writers add
        // JUNK padding — both used to push fmt/data past the old 128-byte
        // window and silently fail.
        let p = write_temp(
            "fc_test_bext.wav",
            &make_wav(&[(b"bext", 602), (b"JUNK", 512)], 384_000),
        );
        let d = parse_wav_duration(&p).unwrap();
        assert!((d - 2.0).abs() < 1e-9);
    }

    #[test]
    fn wav_duration_odd_sized_chunk_is_word_aligned() {
        let p = write_temp("fc_test_odd.wav", &make_wav(&[(b"LIST", 33)], 96_000));
        let d = parse_wav_duration(&p).unwrap();
        assert!((d - 0.5).abs() < 1e-9);
    }

    #[test]
    fn wav_unfinalized_data_size_rejected() {
        // Streamed/interrupted writers leave 0 or 0xFFFFFFFF in the data size;
        // any duration derived from those would be garbage.
        let p0 = write_temp("fc_test_unfin0.wav", &make_wav(&[], 0));
        assert_eq!(parse_wav_duration(&p0), None);
        let pf = write_temp("fc_test_unfinf.wav", &make_wav(&[], u32::MAX));
        assert_eq!(parse_wav_duration(&pf), None);
    }

    #[test]
    fn wav_non_riff_rejected() {
        let p = write_temp("fc_test_notwav.wav", b"FORM....AIFF");
        assert_eq!(parse_wav_duration(&p), None);
    }

    #[test]
    fn companion_log_read_is_bounded_and_lossy_safe() {
        // A log bigger than the 64 KB cap, with the duration line inside the
        // cap and a multibyte char straddling the boundary — must still parse.
        let dir = std::env::temp_dir().join("fc_test_companion");
        let _ = fs::create_dir_all(&dir);
        let base = "Tape_X_2026.07.10_22.32.43";
        let log_path = dir.join(format!("{base}_misrc_capture.log"));
        let mut body = String::from("Recording stopped: duration=6120.27s (01.42.00)\n");
        while body.len() < 64 * 1024 - 1 {
            body.push('x');
        }
        body.push('é'); // 2-byte char straddling the 64 KB boundary
        body.push_str(&"y".repeat(4096));
        fs::write(&log_path, body).unwrap();
        let rf = dir.join(format!("{base}_video_rf_8-bit_20msps.flac"));
        fs::write(&rf, b"").unwrap();
        let d = companion_duration(&rf).unwrap();
        assert!((d - 6120.27).abs() < 1e-6);
    }
}
