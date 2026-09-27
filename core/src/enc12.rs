//! Minimal **true 12-bit FLAC** encoder (MISRC/HdSDAOH standard: signed
//! 12-bit samples [-2048, 2047], STREAMINFO `bits_per_sample` = 12, mono).
//!
//! Why this exists: no command-line encoder can write 12-bit FLAC. Verified on
//! the pinned toolchain (flac 1.5.0, SoX 14.4.2, ffmpeg 6.x):
//!  - `flac --force-raw-format … --bps 12` → `ERROR: invalid bits per sample
//!    '12' (must be 8/16/24/32)` (the CLI's raw/FLAC readers are byte-aligned
//!    only; `--bps` cannot force 12 anywhere),
//!  - `sox -b 12` → `WARN formats: flac can't encode to 12-bit`,
//!  - `ffmpeg -c:a flac -bits_per_raw_sample 12` → silently upgrades to 24.
//! The MISRC capture pipeline writes 12-bit FLAC by linking the libFLAC C API
//! directly (`FLAC__stream_encoder_set_bits_per_sample(12)`), which the FLAC
//! format supports (4–32 bits) but no CLI front-end exposes.
//!
//! Note SoX cannot READ true 12-bit FLAC either (`sox_precision` only accepts
//! byte-aligned bit depths — src/formats.c), and `flac -d` refuses raw output
//! at non-byte-aligned depths (WAV output works: 12-bit samples sit
//! left-justified ×16 in 16-bit containers). That is a SoX limitation, not a
//! property of these files: the reference `flac` CLI, metaflac, ffprobe and
//! claxon all accept them. The cutter therefore feeds 12-bit sources to SoX
//! as raw s16 on stdin (see `chop::chop_with_options`).
//!
//! So this module implements just enough of the FLAC bitstream to produce a
//! valid 12-bit stream without any external dependency: `fLaC` marker +
//! STREAMINFO (exact rate/bps/channel/total, zero MD5 — the spec's "unknown"
//! value, which decoders treat as "skip the check") followed by fixed-blocksize
//! frames carrying VERBATIM subframes (raw two's-complement samples,
//! big-endian bit-packed, zero-padded to the byte). Every frame ends with the
//! standard FLAC CRC-16 and every header with the CRC-8, so decoders accept
//! the stream as fully finalised and checksum-valid.
//!
//! VERBATIM means "stored, not compressed" — for RF content that is not a
//! regression (RF noise sits at ~97% of raw size through libFLAC, so a raw
//! 12-bit frame is actually *smaller* per sample than the compressed 16-bit
//! equivalent), and it is bit-exact by construction: the encoder's only
//! transform is the identity on the 12-bit values, which is what makes the
//! 12 → 16 → 12 round-trip sample-exact.
//!
//! Validation: the module's tests round-trip through claxon (decoder) and the
//! smoke test cross-checks against `flac -d` (reference decoder).

use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

/// Samples per FLAC frame (a common libFLAC default; any value 16..=65536
/// works with the 16-bit blocksize code this encoder writes).
const BLOCK_SIZE: u64 = 4096;

/// Encode mono 12-bit samples as a true 12-bit FLAC to `out_path`.
///
/// `samples` yields the 12-bit values (i16, must be in [-2048, 2047]) wrapped
/// in `Result` so an upstream decode error can abort the stream; `total` is
/// the exact expected count (written into STREAMINFO before the frames are
/// seen — an unfinalised STREAMINFO would defeat the point of the encoder).
/// `sample_rate` is the FLAC header rate (for RF captures the /1000
/// convention value, exactly what SoX wrote on the temp file).
pub fn encode_12bit_mono<I>(samples: I, sample_rate: u32, total: u64, out_path: &Path) -> Result<(), String>
where
    I: Iterator<Item = Result<i16, String>>,
{
    let f = File::create(out_path).map_err(|e| format!("12-bit encode: create output: {e}"))?;
    let mut w = BufWriter::new(f);

    // --- fLaC marker + STREAMINFO (single, last metadata block) ---
    w.write_all(b"fLaC").map_err(eio)?;
    let mut si: Vec<u8> = Vec::with_capacity(34);
    // min = max = BLOCK_SIZE: the canonical fixed-blocksize STREAMINFO (what
    // libFLAC itself writes). A min below max (e.g. the last partial frame's
    // size) makes the reference decoder warn "sample or frame number does not
    // increase correctly" even though the stream is legal.
    si.extend_from_slice(&(BLOCK_SIZE as u16).to_be_bytes());
    si.extend_from_slice(&(BLOCK_SIZE as u16).to_be_bytes());
    si.extend_from_slice(&[0, 0, 0]); // min frame size: unknown
    si.extend_from_slice(&[0, 0, 0]); // max frame size: unknown
    // 64 bits: sample_rate (20) | channels-1 (3) | bps-1 (5) | total (36).
    // 12-bit mono => channels-1 = 0, bps-1 = 11.
    let packed: u64 = ((sample_rate as u64) << 44)
        | (0u64 << 41)
        | (11u64 << 36)
        | (total & 0xF_FFFF_FFFF);
    si.extend_from_slice(&packed.to_be_bytes());
    si.extend_from_slice(&[0u8; 16]); // MD5: zero = "not computed" (spec-legal)
    w.write_all(&[0x80, 0x00, 0x00, 34]).map_err(eio)?; // last block, type 0
    w.write_all(&si).map_err(eio)?;

    // --- audio frames (verbatim subframes) ---
    let mut frame_no: u64 = 0;
    let mut written: u64 = 0;
    let mut block: Vec<i16> = Vec::with_capacity(BLOCK_SIZE as usize);
    for s in samples {
        let s = s?;
        if !(-2048..=2047).contains(&s) {
            return Err(format!("12-bit encode: sample {s} outside [-2048, 2047]"));
        }
        block.push(s);
        written += 1;
        if block.len() == BLOCK_SIZE as usize {
            write_frame(&mut w, frame_no, &block)?;
            frame_no += 1;
            block.clear();
        }
    }
    if !block.is_empty() {
        write_frame(&mut w, frame_no, &block)?;
    }
    if written != total {
        return Err(format!(
            "12-bit encode: STREAMINFO declared {total} samples but {written} were encoded"
        ));
    }
    w.flush().map_err(eio)?;
    Ok(())
}

fn eio(e: std::io::Error) -> String {
    format!("12-bit encode: {e}")
}

/// Write one FLAC frame: fixed-blocksize header (16-bit blocksize code,
/// rate-from-STREAMINFO code, mono 12-bit channel/bps byte, UTF-8 coded frame
/// number, CRC-8) + a verbatim subframe (header 0x04 + 12-bit big-endian
/// bit-packed samples, zero-padded to the byte) + CRC-16.
fn write_frame<W: Write>(w: &mut W, frame_no: u64, block: &[i16]) -> Result<(), String> {
    let mut hdr: Vec<u8> = Vec::with_capacity(12);
    hdr.push(0xFF); // sync high byte
    hdr.push(0xF8); // sync low byte + blocking strategy 0 (fixed)
    hdr.push(0x70); // blocksize code 0111 (16-bit bs-1 follows), rate code 0000 (STREAMINFO)
    // Channel assignment 0000 (mono — the low assignments are the stereo
    // side/mid modes; 1xxx means (n & 0b0111) + 2 channels), bps code 010
    // (12-bit), reserved 0. The frame-header bps field is an enumerated code
    // (000=STREAMINFO, 001=8, 010=12, 100=16, 101=20, 110=24, 111=32),
    // NOT bits-1.
    hdr.push(0x04);
    push_utf8_coded(&mut hdr, frame_no);
    hdr.extend_from_slice(&((block.len() as u16 - 1).to_be_bytes()));
    hdr.push(crc8(&hdr));

    // Subframe: verbatim (type 000001), no wasted bits => header byte 0x02
    // (1 zero pad bit | 6-bit type | wasted-bits flag).
    let mut body: Vec<u8> = Vec::with_capacity(1 + block.len() * 2 + 1);
    body.push(0x02);
    pack_samples_12bit(&mut body, block);

    // CRC-16 covers the whole frame except the CRC itself.
    let mut crc_input = hdr.clone();
    crc_input.extend_from_slice(&body);
    let crc = crc16(&crc_input);

    w.write_all(&hdr).map_err(eio)?;
    w.write_all(&body).map_err(eio)?;
    w.write_all(&crc.to_be_bytes()).map_err(eio)?;
    Ok(())
}

/// Pack 12-bit two's-complement samples MSB-first, zero-padding the final
/// byte (FLAC pads subframes to the byte boundary before the frame CRC).
fn pack_samples_12bit(out: &mut Vec<u8>, block: &[i16]) {
    let mut acc: u64 = 0;
    let mut nbits: u32 = 0;
    for &s in block {
        acc = (acc << 12) | (((s as u16) & 0x0FFF) as u64);
        nbits += 12;
        while nbits >= 8 {
            nbits -= 8;
            out.push(((acc >> nbits) & 0xFF) as u8);
        }
    }
    if nbits > 0 {
        out.push((((acc << (8 - nbits)) & 0xFF)) as u8);
    }
}

/// FLAC frame-number coding ("UTF-8"-style, 1–7 bytes).
fn push_utf8_coded(out: &mut Vec<u8>, n: u64) {
    if n < 0x80 {
        out.push(n as u8);
        return;
    }
    let bits = 64 - n.leading_zeros();
    let size: usize = if bits <= 11 {
        2
    } else if bits <= 16 {
        3
    } else if bits <= 21 {
        4
    } else if bits <= 26 {
        5
    } else if bits <= 31 {
        6
    } else {
        7
    };
    // Low `size-1` six-bit groups go into continuation bytes; the remainder
    // into the first byte behind the leading-ones mask.
    let mut groups: [u8; 6] = [0; 6];
    let mut v = n;
    for g in groups.iter_mut().take(size - 1) {
        *g = (v & 0x3F) as u8;
        v >>= 6;
    }
    // First byte (leading-ones mask + high data bits), then the continuation
    // bytes high group to low.
    let lead: u8 = match size {
        2 => 0xC0,
        3 => 0xE0,
        4 => 0xF0,
        5 => 0xF8,
        6 => 0xFC,
        _ => 0xFE,
    };
    out.push(lead | (v as u8));
    for k in (0..size - 1).rev() {
        out.push(0x80 | groups[k]);
    }
}

/// FLAC CRC-8 (poly x^8 + x^2 + x + 1 = 0x07, init 0) — frame headers.
fn crc8(bytes: &[u8]) -> u8 {
    let mut crc: u8 = 0;
    for &b in bytes {
        crc ^= b;
        for _ in 0..8 {
            crc = if crc & 0x80 != 0 { (crc << 1) ^ 0x07 } else { crc << 1 };
        }
    }
    crc
}

/// FLAC CRC-16 (poly 0x8005, init 0, MSB-first) — whole frames.
fn crc16(bytes: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &b in bytes {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x8005 } else { crc << 1 };
            crc &= 0xFFFF;
        }
    }
    crc
}

#[cfg(test)]
mod tests {
    use super::*;
    use claxon::FlacReader;

    fn ok_iter(v: &[i16]) -> impl Iterator<Item = Result<i16, String>> + '_ {
        v.iter().map(|&s| Ok(s))
    }

    #[test]
    fn roundtrips_through_claxon() {
        // A deterministic 12-bit-range pattern across two frames + a partial
        // third: encode, decode with claxon, compare sample-for-sample.
        let mut data: Vec<i16> = Vec::new();
        for i in 0..(BLOCK_SIZE * 2 + 137) {
            data.push(((((i as i64) * 37) % 4096) - 2048) as i16);
        }
        let out = std::env::temp_dir().join("fc_enc12_roundtrip.flac");
        encode_12bit_mono(ok_iter(&data), 2000, data.len() as u64, &out).unwrap();
        let mut reader = FlacReader::open(&out).unwrap();
        let si = reader.streaminfo();
        assert_eq!(si.bits_per_sample, 12);
        assert_eq!(si.channels, 1);
        assert_eq!(si.sample_rate, 2000);
        assert_eq!(si.samples, Some(data.len() as u64));
        let decoded: Vec<i32> = reader.samples().map(|s| s.unwrap()).collect();
        let decoded: Vec<i16> = decoded.iter().map(|&s| s as i16).collect();
        assert_eq!(decoded, data, "verbatim 12-bit round-trip must be exact");
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn rejects_out_of_range_samples() {
        let out = std::env::temp_dir().join("fc_enc12_range.flac");
        let r = encode_12bit_mono(ok_iter(&[100, 3000]), 2000, 2, &out);
        assert!(r.is_err());
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn utf8_coding_matches_flac_layout() {
        // Reference layouts from the FLAC spec (standard UTF-8 boundaries).
        let cases: &[(u64, &[u8])] = &[
            (0, &[0x00]),
            (127, &[0x7F]),
            (128, &[0xC2, 0x80]), // 2 bytes: 0b110_00010, 0b10_000000
            (0x7FF, &[0xDF, 0xBF]),
            (0x800, &[0xE0, 0xA0, 0x80]),
        ];
        for (n, want) in cases {
            let mut got = Vec::new();
            push_utf8_coded(&mut got, *n);
            assert_eq!(&got, want, "utf8 coding of {n}");
        }
        // A frame number at the top of the 36-bit range needs 7 bytes.
        let mut got = Vec::new();
        push_utf8_coded(&mut got, 0xF_FFFF_FFFF);
        assert_eq!(got.len(), 7);
    }
}
