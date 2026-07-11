//! FLAC metadata probe.
//!
//! Uses claxon's `metadata_only` reader option so constructing the reader
//! stops immediately after the STREAMINFO block. For an RF capture this means
//! we learn sample rate / bits / total samples from the first metadata block
//! without scanning audio frames — a 115 GB file probes in milliseconds.
//!
//! ## The 36-bit `total_samples` wrap
//!
//! The FLAC STREAMINFO `total_samples` field is only 36 bits wide
//! (max 2^36 = 68,719,476,736 samples). RF captures longer than that wrap
//! modulo 2^36: a ~1h38 VHS capture at 20 MSPS is ~118 billion samples, which
//! overflows the field and is stored as `118,247,751,100 mod 2^36 =
//! 49,528,274,364`. Trusting the raw header count would report 41:16 instead of
//! the true 01:38:32.
//!
//! This module ports the wrap-detection logic from
//! `vhs-decode/vhsdecode/hifi/utils.py` (`check_flac_header_total_samples`):
//! compare the declared sample count against the actual audio payload bytes in
//! the file; if the file holds more audio than the declared count could account
//! for, the field wrapped, and we recover the true count by adding `k * 2^36`
//! for the unique `k` that fits the frame-size bounds. When frame-size stats
//! are unavailable we fall back to the smallest `k` consistent with "FLAC never
//! expands the data" (correct for RF noise, which compresses poorly).

use claxon::{FlacReader, FlacReaderOptions};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// 2^36 — the modulus of the FLAC STREAMINFO `total_samples` field.
pub const FLAC_TOTAL_SAMPLES_FIELD_MOD: u64 = 1 << 36; // 68_719_476_736

/// Result of probing a FLAC file's header. `ok` is false on any error; `error`
/// then carries a human-readable message.
#[derive(Debug, Clone)]
pub struct ProbeResult {
    pub ok: bool,
    pub error: String,
    /// Sample rate as stored in the FLAC STREAMINFO (e.g. 20000 for a 20 MSPS
    /// RF capture that divides the real rate by 1000).
    pub header_sample_rate: u64,
    /// Raw `total_samples` straight from STREAMINFO, BEFORE wrap correction.
    pub declared_total_samples: u64,
    /// Total inter-channel samples, AFTER 36-bit wrap correction. For mono RF
    /// this is the total sample count SoX's `trim` works in.
    pub total_samples: u64,
    pub total_samples_known: bool,
    /// Number of 2^36 blocks added to `declared_total_samples` to get
    /// `total_samples`. 0 means the header count was trusted as-is.
    pub total_samples_wraps: u32,
    /// True if the wrap count could not be pinned down from frame-size stats
    /// and was instead estimated from the uncompressed-byte lower bound
    /// (assumes light compression — typical for RF noise).
    pub total_samples_estimated: bool,
    pub bits_per_sample: u32,
    pub channels: u32,
    /// Total file size in bytes (from stat, no read).
    pub file_size: u64,
    /// Byte offset where the first audio frame starts (just past the last
    /// metadata block). `file_size - audio_offset` is the compressed audio
    /// payload size used by the wrap check.
    pub audio_offset: u64,
}

impl Default for ProbeResult {
    fn default() -> Self {
        Self {
            ok: false,
            error: String::new(),
            header_sample_rate: 0,
            declared_total_samples: 0,
            total_samples: 0,
            total_samples_known: false,
            total_samples_wraps: 0,
            total_samples_estimated: false,
            bits_per_sample: 0,
            channels: 0,
            file_size: 0,
            audio_offset: 0,
        }
    }
}

/// Walk the FLAC metadata block headers from the start of `file` and return the
/// byte offset just past the last metadata block (i.e. the first audio frame).
/// Validates the `fLaC` stream marker. Does not parse block bodies.
fn find_audio_offset(file: &mut File) -> Result<u64, String> {
    file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
    let mut magic = [0u8; 4];
    file.read_exact(&mut magic).map_err(|e| e.to_string())?;
    if &magic != b"fLaC" {
        return Err("not a FLAC stream (missing fLaC marker)".to_string());
    }
    loop {
        let mut hdr = [0u8; 4];
        file.read_exact(&mut hdr).map_err(|e| e.to_string())?;
        let is_last = (hdr[0] & 0x80) != 0;
        let length = u32::from_be_bytes([0, hdr[1], hdr[2], hdr[3]]);
        file.seek(SeekFrom::Current(length as i64))
            .map_err(|e| e.to_string())?;
        if is_last {
            return Ok(file.stream_position().map_err(|e| e.to_string())?);
        }
    }
}

/// Decide whether the declared `total_samples` can be trusted, and if not,
/// recover the true count by undoing 36-bit wraps. Mirrors
/// `check_flac_header_total_samples` from vhs-decode.
///
/// Returns `(trustworthy, corrected_or_none)`. `corrected` is `Some(true_count)`
/// only when a wrap was detected AND a unique recovery was possible via the
/// frame-size bounds. When `(false, None)` is returned the header is known to
/// be wrapped but the precise wrap count could not be determined — the caller
/// then applies the uncompressed lower-bound fallback.
fn check_total_samples(
    declared: u64,
    audio_bytes: u64,
    min_block: u16,
    max_block: u16,
    min_frame: Option<u32>,
    max_frame: Option<u32>,
    channels: u32,
    bps: u32,
) -> (bool, Option<u64>) {
    if audio_bytes == 0 {
        return (true, None);
    }
    if declared == 0 {
        // 0 means "unknown" in STREAMINFO — not a wrap, just absent.
        return (false, None);
    }

    // Upper bound on the bytes the declared sample count could occupy.
    let declared_max_bytes: u128 = if min_block > 0 && max_frame.map_or(false, |f| f > 0) {
        // Each block holds >= min_block samples and occupies <= max_frame bytes,
        // so declared samples occupy at most (declared/min_block + 1) frames.
        (declared as u128 / min_block as u128 + 1) * max_frame.unwrap() as u128
    } else {
        // Frame-size stats unknown: fall back to verbatim worst case + margin.
        (declared as u128 * channels as u128 * (bps as u128 / 8) * 105 / 100) + 65536
    };

    if (audio_bytes as u128) <= declared_max_bytes {
        return (true, None);
    }

    // The file holds more audio than the declared count could account for: the
    // 36-bit field wrapped. Try to recover the true count from the frame-size
    // bounds, accepting it only if exactly one wrap count fits.
    if min_block > 0
        && max_block > 0
        && min_frame.map_or(false, |f| f > 0)
        && max_frame.map_or(false, |f| f > 0)
    {
        let min_b = min_block as u128;
        let max_b = max_block as u128;
        let min_f = min_frame.unwrap() as u128;
        let max_f = max_frame.unwrap() as u128;
        let lower = audio_bytes as u128 / max_f * min_b;
        let upper = audio_bytes as u128 / min_f * max_b;
        let mut candidates: Vec<u64> = Vec::new();
        let mut k = 1u64;
        loop {
            let candidate =
                declared as u128 + (k as u128) * (FLAC_TOTAL_SAMPLES_FIELD_MOD as u128);
            if candidate > upper {
                break;
            }
            if candidate >= lower {
                candidates.push(candidate as u64);
            }
            k += 1;
        }
        if candidates.len() == 1 {
            return (false, Some(candidates[0]));
        }
    }

    (false, None)
}

/// Open `path` and read its FLAC STREAMINFO, then correct the 36-bit
/// `total_samples` wrap if present. Never reads audio frames.
pub fn probe(path: &Path) -> ProbeResult {
    let mut r = ProbeResult::default();

    let file = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            r.error = format!("open failed: {e}");
            return r;
        }
    };

    // File size from stat (no read) — needed for the wrap check.
    let file_size = match file.metadata() {
        Ok(m) => m.len(),
        Err(e) => {
            r.error = format!("stat failed: {e}");
            return r;
        }
    };
    r.file_size = file_size;

    // Walk metadata block headers on a separate handle to find the audio
    // offset (claxon doesn't expose where its metadata-only reader stopped).
    let mut off_file = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            r.error = format!("open failed: {e}");
            return r;
        }
    };
    let audio_offset = match find_audio_offset(&mut off_file) {
        Ok(o) => o,
        Err(e) => {
            r.error = e;
            return r;
        }
    };
    r.audio_offset = audio_offset;

    // metadata_only + skip vorbis comment => claxon returns as soon as the
    // mandatory STREAMINFO block has been parsed.
    let opts = FlacReaderOptions {
        metadata_only: true,
        read_vorbis_comment: false,
    };
    let reader = match FlacReader::new_ext(file, opts) {
        Ok(rd) => rd,
        Err(e) => {
            r.error = format!("not a valid FLAC stream: {e}");
            return r;
        }
    };

    let si = reader.streaminfo();
    r.ok = true;
    r.header_sample_rate = u64::from(si.sample_rate);
    r.bits_per_sample = si.bits_per_sample;
    r.channels = si.channels;

    let declared = si.samples.unwrap_or(0);
    let known = si.samples.is_some();
    r.declared_total_samples = declared;
    r.total_samples = declared;
    r.total_samples_known = known;

    if known && audio_offset > 0 && file_size > audio_offset {
        let audio_bytes = file_size - audio_offset;
        let (trustworthy, corrected) = check_total_samples(
            declared,
            audio_bytes,
            si.min_block_size,
            si.max_block_size,
            si.min_frame_size,
            si.max_frame_size,
            si.channels,
            si.bits_per_sample,
        );
        if !trustworthy {
            if let Some(c) = corrected {
                // Precise recovery from frame-size bounds.
                r.total_samples_wraps = ((c - declared) / FLAC_TOTAL_SAMPLES_FIELD_MOD) as u32;
                r.total_samples = c;
            } else {
                // Frame-size stats missing or ambiguous: fall back to the
                // smallest wrap count k>=1 for which the true sample count is
                // at least the uncompressed audio payload (FLAC never expands
                // the data; RF noise compresses poorly, so the smallest k is
                // the right estimate).
                let bytes_per_sample = u64::from(si.channels) * (u64::from(si.bits_per_sample) / 8);
                if bytes_per_sample > 0 {
                    let lower = (audio_bytes + bytes_per_sample - 1) / bytes_per_sample; // ceil
                    let mut k = 1u64;
                    while k <= 4096 {
                        let cand = declared + k * FLAC_TOTAL_SAMPLES_FIELD_MOD;
                        if cand >= lower {
                            r.total_samples = cand;
                            r.total_samples_wraps = k as u32;
                            r.total_samples_estimated = true;
                            break;
                        }
                        k += 1;
                    }
                }
            }
        }
    }

    r
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_file_is_reported_not_panicked() {
        let r = probe(Path::new("/nonexistent/nope.flac"));
        assert!(!r.ok);
        assert!(r.error.contains("open failed"));
    }

    #[test]
    fn check_trusts_short_file_no_wrap() {
        // 1 min @ 20 MSPS, 8-bit mono: 1.2e9 samples, ~1.2 GB uncompressed.
        // Compressed payload (1.0 GB) fits the declared count → trustworthy.
        let (trust, corrected) = check_total_samples(
            1_200_000_000,
            1_000_000_000,
            65535,
            65535,
            Some(64_000),
            Some(70_000),
            1,
            8,
        );
        assert!(trust);
        assert_eq!(corrected, None);
    }

    #[test]
    fn check_detects_wrap_and_recovers_precisely() {
        // Tape_15-like: declared = 49.5e9 (wrapped), true = 118.2e9 (one wrap),
        // audio payload ~115 GB. Frame sizes chosen so the [lower, upper] range
        // contains exactly the k=1 candidate.
        let declared: u64 = 49_528_274_364;
        let audio_bytes: u64 = 115_000_000_000;
        // blocksize 65535; min_frame 60000, max_frame 74000:
        //   lower = 115e9 / 74000 * 65535 ≈ 101.8e9
        //   upper = 115e9 / 60000 * 65535 ≈ 125.6e9
        //   k=1 cand = 118.247e9  -> inside  ✓
        //   k=2 cand = 186.967e9  -> > upper ✗
        let (trust, corrected) = check_total_samples(
            declared,
            audio_bytes,
            65535,
            65535,
            Some(60_000),
            Some(74_000),
            1,
            8,
        );
        assert!(!trust);
        let c = corrected.expect("expected a unique recovery");
        assert_eq!(c - declared, FLAC_TOTAL_SAMPLES_FIELD_MOD);
        assert_eq!(c, 118_247_751_100);
    }

    #[test]
    fn check_wrap_without_frame_sizes_returns_none_for_fallback() {
        // Frame-size stats unknown: precise recovery impossible → (false, None),
        // and the caller's fallback must supply the smallest plausible k.
        let declared: u64 = 49_528_274_364;
        let audio_bytes: u64 = 115_000_000_000;
        let (trust, corrected) =
            check_total_samples(declared, audio_bytes, 65535, 65535, None, None, 1, 8);
        assert!(!trust);
        assert_eq!(corrected, None);
    }
}
