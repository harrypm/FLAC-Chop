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
use crate::rate::resolve_real_rate;
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
    /// True if the total sample count was obtained by scanning FLAC frame
    /// headers (the STREAMINFO total was 0/unknown — an unfinalized, typically
    /// piped/streamed capture). The count is exact, summed from every frame's
    /// block size; false means the count came from the STREAMINFO header.
    pub total_samples_scanned: bool,
    /// True if the total was inferred from a companion file (a sibling .log
    /// with a `duration=Ns` line, or a sibling .wav header) sharing the capture
    /// base name. Exact (the companion is finalized even when this file isn't).
    pub total_samples_from_companion: bool,
    /// True if the total was read from a Vorbis comment `RF_TOTAL_SAMPLES` tag
    /// inside the FLAC itself (the MISRC/DdD pipeline's authoritative in-file
    /// record). Highest-priority source — beats the header, companions, and
    /// frame scan.
    pub total_samples_from_vorbis: bool,
    /// True if `RF_SAMPLE_RATE` was present in the Vorbis comment and used to
    /// confirm the RF /1000 rate resolution.
    pub rate_from_vorbis: bool,
    pub bits_per_sample: u32,
    pub channels: u32,
    /// Total file size in bytes (from stat, no read).
    pub file_size: u64,
    /// Byte offset where the first audio frame starts (just past the last
    /// metadata block). `file_size - audio_offset` is the compressed audio
    /// payload size used by the wrap check.
    pub audio_offset: u64,
    /// Real sample rate in Hz, resolved from the header rate via the RF /1000
    /// rule (see [`crate::rate`]). For a 20 MSPS RF capture this is
    /// 20,000,000 even though the header says 20000.
    pub real_rate_hz: f64,
    /// True if the file is treated as RF (header rate was multiplied by 1000
    /// or an msps hint was used). False for real audio files.
    pub is_rf: bool,
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
            total_samples_scanned: false,
            total_samples_from_companion: false,
            total_samples_from_vorbis: false,
            rate_from_vorbis: false,
            bits_per_sample: 0,
            channels: 0,
            file_size: 0,
            audio_offset: 0,
            real_rate_hz: 0.0,
            is_rf: false,
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

// --- FLAC frame-header scanning (for files with unknown STREAMINFO total) ---
//
// When the STREAMINFO `total_samples` is 0/unknown (an unfinalized, typically
// piped/streamed capture), the only way to get the true sample count is to
// walk every FLAC frame header and sum each frame's block size. This does NOT
// decode audio — it reads only the small fixed header at the start of each
// frame. The bit layout mirrors claxon's `frame::read_frame_header_or_eof`
// (claxon-0.4.3/src/frame.rs) so we don't re-derive the format.
//
// Robustness: a stray 0xFF 0xF8/0xF9 inside compressed audio could look like a
// frame sync. We reject such false positives three ways: (1) every header field
// must have a valid (non-reserved) encoding, (2) the header CRC-8 must match,
// and (3) the frame/sample number must equal the expected sequential value
// (frame index for fixed-blocksize, running sample count for variable). A
// random byte sequence satisfying all three is astronomically unlikely, so the
// summed count is exact.

/// CRC-8 table (polynomial x^8 + x^2 + x + 1, init 0) — same as claxon crc.rs.
const CRC8_TABLE: [u8; 256] = [
    0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
    0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
    0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
    0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
    0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
    0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
    0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
    0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
    0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
    0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
    0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
    0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
    0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
    0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
    0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
    0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3,
];

/// Read the FLAC "UTF-8"-coded variable-length integer from the start of `w`.
/// Returns `(value, bytes_consumed)` or `None` if invalid / not enough bytes.
/// Mirrors claxon `frame::read_var_length_int`.
fn read_utf8_coded(w: &[u8]) -> Option<(u64, usize)> {
    if w.is_empty() {
        return None;
    }
    let first = w[0];
    let mut read_additional = 0u8;
    let mut mask_data = 0b0111_1111u8;
    let mut mask_mark = 0b1000_0000u8;
    while first & mask_mark != 0 {
        read_additional += 1;
        mask_data >>= 1;
        mask_mark >>= 1;
    }
    if read_additional > 0 {
        if read_additional == 1 {
            return None; // a lone continuation byte as the first byte is invalid
        }
        read_additional -= 1;
    }
    let total = 1 + read_additional as usize;
    if total > w.len() {
        return None; // not enough bytes yet
    }
    let mut result = ((first & mask_data) as u64) << (6 * read_additional as usize);
    for k in 0..read_additional as usize {
        let byte = w[1 + k];
        if byte & 0b1100_0000 != 0b1000_0000 {
            return None; // continuation byte must start with 10xxxxxx
        }
        let shift = 6 * (read_additional as usize - 1 - k);
        result |= ((byte & 0b0011_1111) as u64) << shift;
    }
    Some((result, total))
}

/// Try to parse a FLAC frame header at the start of `w`. Returns
/// `(block_size, header_len, number, variable)` on a header that is
/// structurally valid and whose CRC-8 checks out; `None` otherwise. The
/// caller applies the sequential-number cross-check (so this does not need the
/// expected value). `w` must be at least `MAX_HEADER_LEN` bytes when not at EOF.
pub const MAX_HEADER_LEN: usize = 17;
fn parse_frame_header(w: &[u8]) -> Option<(u16, usize, u64, bool)> {
    if w.len() < 2 {
        return None;
    }
    let sync = u16::from_be_bytes([w[0], w[1]]);
    // 14-bit sync 11111111111110, then reserved bit (must be 0), then blocking.
    if sync & 0b1111_1111_1111_1110 != 0b1111_1111_1111_1000 {
        return None;
    }
    if sync & 0b0000_0000_0000_0010 != 0 {
        return None; // reserved bit set
    }
    let variable = sync & 0b0000_0000_0000_0001 != 0;

    if w.len() < 4 {
        return None;
    }
    let bs_sr = w[2];
    let mut block_size: u16 = 0;
    let mut read_8bit_bs = false;
    let mut read_16bit_bs = false;
    match bs_sr >> 4 {
        0b0000 => return None, // reserved
        0b0001 => block_size = 192,
        n if (0b0010..=0b0101).contains(&n) => block_size = 576 * (1u16 << (n - 2)),
        0b0110 => read_8bit_bs = true,
        0b0111 => read_16bit_bs = true,
        n => block_size = 256 * (1u16 << (n - 8)),
    }
    let mut read_8bit_sr = false;
    let mut read_16bit_sr = false;
    let mut read_16bit_sr_ten = false;
    match bs_sr & 0b0000_1111 {
        0b0000 => {}                  // get sample rate from STREAMINFO
        0b0001..=0b1011 => {}          // predefined rate; value not needed here
        0b1100 => read_8bit_sr = true,
        0b1101 => read_16bit_sr = true,
        0b1110 => read_16bit_sr_ten = true,
        0b1111 => return None,         // invalid (would fool sync detection)
        _ => return None,
    }

    let chan_bps_res = w[3];
    if chan_bps_res >> 4 >= 0b1011 {
        return None; // channel assignment 1011..1111 reserved
    }
    match (chan_bps_res & 0b0000_1110) >> 1 {
        0b000 | 0b001 | 0b010 | 0b100 | 0b101 | 0b110 => {}
        _ => return None, // 011 and 111 reserved
    }
    if chan_bps_res & 0b0000_0001 != 0 {
        return None; // reserved bit
    }

    // UTF-8 coded frame/sample number starts at index 4.
    let (number, utf8_len) = read_utf8_coded(&w[4..])?;
    let mut pos = 4 + utf8_len;

    if read_8bit_bs {
        if pos + 1 > w.len() {
            return None;
        }
        block_size = w[pos] as u16 + 1;
        pos += 1;
    }
    if read_16bit_bs {
        if pos + 2 > w.len() {
            return None;
        }
        let bs = u16::from_be_bytes([w[pos], w[pos + 1]]);
        if bs == 0xffff {
            return None; // exceeds max block size
        }
        block_size = bs + 1;
        pos += 2;
    }
    if read_8bit_sr {
        if pos + 1 > w.len() {
            return None;
        }
        pos += 1;
    }
    if read_16bit_sr {
        if pos + 2 > w.len() {
            return None;
        }
        pos += 2;
    }
    if read_16bit_sr_ten {
        if pos + 2 > w.len() {
            return None;
        }
        pos += 2;
    }

    // Trailing CRC-8 over all header bytes so far.
    if pos + 1 > w.len() {
        return None;
    }
    let mut crc = 0u8;
    for &b in &w[..pos] {
        crc = CRC8_TABLE[(crc ^ b) as usize];
    }
    if crc != w[pos] {
        return None;
    }

    Some((block_size, pos + 1, number, variable))
}

/// Walk FLAC frame headers from `audio_offset` to EOF, summing each frame's
/// block size. Exact (no audio decoding). Used only when STREAMINFO
/// `total_samples` is 0/unknown.
///
/// `min_frame_size` (from STREAMINFO, if known) is used to skip forward after
/// each accepted frame: the next frame sync cannot be closer than one full
/// min-size frame after the current frame start, so we jump past the current
/// frame's body instead of byte-scanning it. This both speeds up the scan
/// (skipping the bulk of each frame's compressed bytes) and avoids chasing
/// false sync patterns inside frame data.
fn count_samples_by_scanning(
    file: &mut File,
    audio_offset: u64,
    min_frame_size: Option<u32>,
) -> Result<u64, String> {
    file.seek(SeekFrom::Start(audio_offset))
        .map_err(|e| e.to_string())?;

    const CHUNK: usize = 8 << 20; // 8 MiB — fewer syscalls on slow disks
    const SOFT_EOF_AFTER: u64 = 4 * 1024 * 1024; // 4 MiB seed-fallback window
    let min_frame_skip = min_frame_size.map(|f| f as usize).unwrap_or(0);
    let mut buf = vec![0u8; CHUNK];
    let mut carry: Vec<u8> = Vec::new();
    let mut running: u64 = 0;       // sum of block sizes (also = expected sample no. for variable)
    let mut frame_index: u64 = 0;   // expected frame number for fixed-blocksize
    let mut seeded = false;         // have we accepted the first frame yet?
    let mut strict = true;          // require sequential frame/sample numbers
    let mut scanned_so_far: u64 = 0; // bytes scanned since audio_offset (for seed fallback)
    let mut frames_found: u64 = 0;

    loop {
        let n = file.read(&mut buf).map_err(|e| e.to_string())?;
        let eof = n == 0;
        let mut window: Vec<u8> = Vec::with_capacity(carry.len() + n);
        window.extend_from_slice(&carry);
        window.extend_from_slice(&buf[..n]);

        let mut i = 0usize;
        while i + 1 < window.len() {
            let b0 = window[i];
            let b1 = window[i + 1];
            if b0 == 0xFF && (b1 == 0xF8 || b1 == 0xF9) {
                // Need a full header's worth of bytes to parse definitively,
                // unless we're at EOF (no more bytes are coming).
                if !eof && window.len() - i < MAX_HEADER_LEN {
                    break; // NeedMore — keep this candidate for the next chunk
                }
                match parse_frame_header(&window[i..]) {
                    Some((block_size, hlen, number, variable)) => {
                        let expected = if variable { running } else { frame_index };
                        if !strict && !seeded {
                            // Seed the counters from this frame's number so a
                            // mid-stream cut (first frame # != 0) still works.
                            if variable {
                                running = number;
                            } else {
                                frame_index = number;
                            }
                        } else if number != expected {
                            // Sequential cross-check failed — not our frame.
                            i += 1;
                            continue;
                        }
                        running = running.saturating_add(block_size as u64);
                        frame_index = frame_index.saturating_add(1);
                        seeded = true;
                        strict = true; // subsequent frames must be sequential
                        frames_found += 1;
                        i += hlen;
                        // Speedup: the next frame sync is at least one
                        // min-size frame away from this frame's start, so skip
                        // past the body. Safe because a real sync can't appear
                        // inside a frame, and it dodges false syncs in the data.
                        if min_frame_skip > hlen {
                            i += min_frame_skip - hlen;
                        }
                        continue;
                    }
                    None => {
                        i += 1;
                    }
                }
            } else {
                i += 1;
            }
        }

        if eof {
            break;
        }

        scanned_so_far += n as u64;
        // If we scanned 4 MiB without accepting a single frame, the stream
        // probably doesn't start at frame #0 (mid-stream cut). Relax to seed
        // from the first structurally-valid, CRC-valid header we find.
        if !seeded && strict && scanned_so_far >= SOFT_EOF_AFTER {
            strict = false;
        }

        // Keep an unprocessed tail, capped at 64 bytes (a header is <= 17), so
        // candidates split across chunk boundaries survive. Bytes before `i`
        // are fully processed (any accepted frame is before `i`, so no double-
        // count); the tail is unprocessed. Clamp `i` to the window length first:
        // the min_frame_size skip can jump `i` past the end of the window.
        let keep_from = i.min(window.len()).max(window.len().saturating_sub(64));
        carry = window[keep_from..].to_vec();
    }

    eprintln!(
        "[probe] scan done: frames_found={}, samples={}, min_frame_size={:?}",
        frames_found, running, min_frame_size
    );

    Ok(running)
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

    // Read the custom RF Vorbis comment tags (RF_TOTAL_SAMPLES / RF_SAMPLE_RATE
    // / DURATION_SECONDS) that the MISRC/DdD pipeline writes inside the FLAC.
    // These are the capture tool's authoritative in-file record and beat every
    // other source. The tags are self-consistent by construction:
    //   RF_TOTAL_SAMPLES / RF_SAMPLE_RATE = DURATION_SECONDS
    // The pipeline has used two tag schemas: an early one where RF_SAMPLE_RATE
    // held the /1000 kHz value (20000) and a later one where it holds the real
    // Hz value (20000000, with RF_SAMPLE_RATE_KHZ for the /1000 value). Either
    // way, dividing RF_TOTAL_SAMPLES by RF_SAMPLE_RATE gives the correct real
    // duration, so we use RF_SAMPLE_RATE directly as the real rate — no ×1000
    // assumption. The ×1000 RF convention only applies to files WITHOUT these
    // tags (handled by the rate module).
    let rf_tags = crate::vorbis::read_rf_tags(path);

    let msps_hint = crate::msps::extract_msps(&path.to_string_lossy());
    let (mut real_rate, mut is_rf) = resolve_real_rate(r.header_sample_rate, msps_hint);
    if let Some(tag_sr) = rf_tags.sample_rate {
        if tag_sr > 0 {
            // The tag's RF_SAMPLE_RATE is the rate the capture tool counts
            // samples at; use it directly as the real rate.
            real_rate = tag_sr as f64;
            is_rf = true;
            r.rate_from_vorbis = true;
        }
    }
    r.real_rate_hz = real_rate;
    r.is_rf = is_rf;

    let declared = si.samples.unwrap_or(0);
    let known = si.samples.is_some();
    r.declared_total_samples = declared;
    r.total_samples = declared;
    r.total_samples_known = known;

    // --- Total sample count resolution (priority order) ---
    // 1. Vorbis `RF_TOTAL_SAMPLES` tag — authoritative, in-file, exact.
    // 2. STREAMINFO header (+ 36-bit wrap correction) — finalized files.
    // 3. Companion .log/.wav — unfinalized files with a sibling.
    // 4. Frame-header scan — unfinalized files with no sibling (slow).
    let mut vorbis_total: Option<u64> = None;
    if let Some(ts) = rf_tags.total_samples {
        if ts > 0 {
            vorbis_total = Some(ts);
        }
    }
    // Sanity: the tags' own math is RF_TOTAL_SAMPLES / RF_SAMPLE_RATE =
    // DURATION_SECONDS, all at the /1000 (kHz) rate. Verify they agree; warn
    // if not, but still trust the integer total. (The real cut rate used
    // downstream is RF_SAMPLE_RATE * 1000.)
    if let (Some(ts), Some(tag_sr), Some(dur)) =
        (vorbis_total, rf_tags.sample_rate, rf_tags.duration_seconds)
    {
        if tag_sr > 0 {
            let implied = ts as f64 / tag_sr as f64;
            if (implied - dur).abs() > 1.0 {
                eprintln!(
                    "[probe] WARNING: vorbis RF_TOTAL_SAMPLES={} / RF_SAMPLE_RATE={} = {:.3}s but DURATION_SECONDS={}; mismatch",
                    ts, tag_sr, implied, dur
                );
            }
        }
    }

    if let Some(ts) = vorbis_total {
        // RF_TOTAL_SAMPLES is the actual on-disk sample count, at the
        // RF_SAMPLE_RATE (which equals the header rate). Use it as-is — no
        // scaling. (Duration = ts / RF_SAMPLE_RATE, matching DURATION_SECONDS.)
        r.total_samples = ts;
        r.total_samples_known = true;
        r.total_samples_from_vorbis = true;
    } else if known && audio_offset > 0 && file_size > audio_offset {
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
    } else if !known {
        // SANITY CHECK: STREAMINFO total_samples is 0/unknown ONLY when the
        // encoder could not seek back to finalize the header (a piped/streamed
        // or interrupted capture). We must NOT guess the count from the file
        // size (FLAC compression varies too much — a 70 GB file can be 58 min
        // or 1h42 depending on content).
        //
        // First try a companion file that shares the capture base name — a
        // sibling .log with an explicit `duration=Ns` line, or a sibling .wav
        // header. These are finalized even when this FLAC isn't, and are exact
        // + cheap (a few KB / a header read). Verified on Tape_12: the misrc
        // log says duration=6120.27s and the baseband WAV agrees at 6120.82s.
        if let Some(secs) = crate::companions::companion_duration(path) {
            if secs > 0.0 {
                r.total_samples = (secs * r.real_rate_hz).round() as u64;
                r.total_samples_known = true;
                r.total_samples_from_companion = true;
            }
        }

        // Fallback: scan the FLAC frame headers and sum every frame's block
        // size for an exact count (slower — reads the whole file). Only when
        // no companion was found. We only do this when the header total is
        // genuinely absent; a file that HAS a header total is finalized and is
        // handled by the wrap branch above.
        if !r.total_samples_from_companion
            && audio_offset > 0
            && file_size > audio_offset
        {
            let audio_bytes = file_size - audio_offset;
            match count_samples_by_scanning(&mut off_file, audio_offset, si.min_frame_size) {
                Ok(scanned) if scanned > 0 => {
                    // FLAC never expands the data, so the uncompressed size of
                    // the scanned samples must be >= the compressed payload.
                    // If it isn't, the scan is suspect (corrupt stream / mis-
                    // alignment) — flag it but still report the scanned count,
                    // which comes from validated frame headers.
                    let bytes_per_sample =
                        u64::from(si.channels) * (u64::from(si.bits_per_sample) / 8);
                    let uncompressed = scanned.saturating_mul(bytes_per_sample);
                    if bytes_per_sample > 0 && uncompressed < audio_bytes {
                        eprintln!(
                            "[probe] WARNING: scanned {} samples * {}/ch-byte = {} bytes < audio payload {} bytes; scan may be misaligned",
                            scanned, bytes_per_sample, uncompressed, audio_bytes
                        );
                    }
                    r.total_samples = scanned;
                    r.total_samples_known = true;
                    r.total_samples_scanned = true;
                }
                Ok(_) => {
                    // No frames found — leave known=false; the GUI shows unknown.
                }
                Err(e) => {
                    eprintln!("[probe] frame scan failed: {e}");
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
