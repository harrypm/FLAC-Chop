//! DdD/ld-decode packed 10-bit `.lds` RF data — unpack support.
//!
//! `.lds` is the Domesday86 / ld-decode raw LaserDisc RF capture format: mono
//! 10-bit unsigned samples (centre 512) with **4 samples packed into 5 bytes**:
//!
//! ```text
//!  Unpacked (10-bit values):      Packed (5 bytes):
//!  0: xxxx xx00 0000 0000         0: 0000 0000 0011 1111
//!  1: xxxx xx11 1111 1111         2: 1111 2222 2222 2233
//!  2: xxxx xx22 2222 2222         4: 3333 3333
//!  3: xxxx xx33 3333 3333
//! ```
//!
//! Unpacking to the 16-bit samples the rest of the pipeline (SoX / the 12-bit
//! encoder) consumes applies the reference scaling
//! `s16 = (value - 512) * 64` — the exact stream tbc-tools' `ld-lds-converter`
//! (`src/ld-lds-converter/dataconverter.cpp`, `unpackFile`/`packFile`) writes
//! for its FLAC/s16 outputs: the 10-bit value left-justified into the 16-bit
//! container, same as the headerless u16 raw handling. This module re-derives
//! that packing in Rust from the reference implementation's bit layout (the
//! file-format facts; no code is copied).
//!
//! Sample-exact cuts of a huge `.lds` never unpack the whole file: a cut of
//! `[start, start+len)` needs only the byte range of the packing groups that
//! cover it ([`unpack_window`]), which is what the cutter feeds to SoX (a
//! temp s16 next to the output, exactly like the Ogg-FLAC window remux).

use std::fs::File;
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

/// Byte size of one packing group (4 samples).
pub const GROUP_BYTES: u64 = 5;
/// Samples in one packing group.
pub const GROUP_SAMPLES: u64 = 4;
/// Reference default rate for a hint-less `.lds`: 40 MSPS (the ld-decode
/// LaserDisc RF convention; ld-lds-converter's default `--sample-rate 40000`
/// is the same /1000-convention value).
pub const DEFAULT_MSPS: f64 = 40.0;

/// Unpack one 5-byte group into 4 s16 samples (the reference scaling
/// `(value - 512) * 64`: the 10-bit value left-justified into s16).
#[inline]
pub fn unpack_group(b: &[u8; 5]) -> [i16; 4] {
    let word0 = (u32::from(b[0]) * 4) + (u32::from(b[1] & 0xC0) >> 6);
    let word1 = (u32::from(b[1] & 0x3F) * 16) + (u32::from(b[2] & 0xF0) >> 4);
    let word2 = (u32::from(b[2] & 0x0F) * 64) + (u32::from(b[3] & 0xFC) >> 2);
    let word3 = (u32::from(b[3] & 0x03) * 256) + u32::from(b[4]);
    [
        ((word0 as i32 - 512) * 64) as i16,
        ((word1 as i32 - 512) * 64) as i16,
        ((word2 as i32 - 512) * 64) as i16,
        ((word3 as i32 - 512) * 64) as i16,
    ]
}

/// Pack 4 s16 samples into one 5-byte group — the exact inverse of
/// [`unpack_group`] up to the 10-bit quantization the format mandates
/// (`value = (s16 / 64) + 512`, truncating toward zero exactly like the
/// reference C++ integer division).
#[inline]
pub fn pack_group(s: &[i16; 4]) -> [u8; 5] {
    let w = [
        (i32::from(s[0]) / 64) + 512,
        (i32::from(s[1]) / 64) + 512,
        (i32::from(s[2]) / 64) + 512,
        (i32::from(s[3]) / 64) + 512,
    ];
    let mut b = [0u8; 5];
    b[0] = ((w[0] & 0x03FC) >> 2) as u8;
    b[1] = (((w[0] & 0x0003) << 6) + ((w[1] & 0x03F0) >> 4)) as u8;
    b[2] = (((w[1] & 0x000F) << 4) + ((w[2] & 0x03C0) >> 6)) as u8;
    b[3] = (((w[2] & 0x003F) << 2) + ((w[3] & 0x0300) >> 8)) as u8;
    b[4] = (w[3] & 0x00FF) as u8;
    b
}

/// Total unpackable samples in a `.lds` of `file_size` bytes, plus the
/// trailing bytes that don't form a whole group (dropped, like the reference
/// converter's "dropping trailing N bytes" alignment handling).
pub fn total_samples(file_size: u64) -> (u64, u64) {
    (
        file_size / GROUP_BYTES * GROUP_SAMPLES,
        file_size % GROUP_BYTES,
    )
}

/// Where an [`unpack_window`] starts/ends, in on-disk sample units.
#[derive(Debug, Clone, Copy)]
pub struct Window {
    /// On-disk sample index of the FIRST sample written to the temp s16
    /// (the group-aligned floor of the requested start).
    pub first_sample: u64,
    /// Samples actually written (<= requested length; clamped at EOF).
    pub samples: u64,
}

/// Read exactly `buf.len()` bytes, refilling short reads (Windows `Read`
/// returns partial buffers; a true EOF mid-group is a hard error).
fn read_exact_group(src: &mut File, buf: &mut [u8; 5]) -> Result<(), String> {
    let mut got = 0;
    while got < buf.len() {
        let n = src
            .read(&mut buf[got..])
            .map_err(|e| format!("read failed: {e}"))?;
        if n == 0 {
            return Err(format!(
                "unexpected end of .lds data mid-group ({got}/{} bytes)",
                buf.len()
            ));
        }
        got += n;
    }
    Ok(())
}

/// Unpack exactly the samples needed for the cut `[start, start+len)` into a
/// raw s16 little-endian temp file (mono). Only the covering groups are read
/// — a 10 s cut of a 352 GB `.lds` touches ~800 MB of source, not the whole
/// file. `cancel` is polled between batches (a cancel removes the partial
/// temp and returns Err("cancelled")).
///
/// The temp covers `[first_sample, first_sample + skip + len)` — the leading
/// `skip` (0..=3) group-mates of the requested start are INCLUDED (the inner
/// SoX `trim <skip>s <len>s` then drops them exactly); leaving them out made
/// the trim window extend one sample past the temp end (a 1-sample-short cut
/// + sox "Last 1 position(s) not reached" — verified on real data).
///
/// Returns the [`Window`] actually written: `first_sample` is the group-aligned
/// start, and `samples` is the TOTAL temp length (skip + len, clamped at EOF —
/// a short temp means sox clamps the cut to the file end, like the Ogg path).
pub fn unpack_window(
    in_path: &Path,
    out_s16: &Path,
    start_samples: u64,
    length_samples: u64,
    cancel: &dyn Fn() -> bool,
) -> Result<Window, String> {
    let file_size = std::fs::metadata(in_path)
        .map_err(|e| format!("stat failed: {e}"))?
        .len();
    let (total, _trailing) = total_samples(file_size);
    if start_samples >= total {
        return Err(format!(
            "start sample {start_samples} is at or past the end of the .lds data ({total} samples)"
        ));
    }
    if length_samples == 0 {
        return Err("length must be > 0".to_string());
    }

    let first_group = start_samples / GROUP_SAMPLES;
    let skip = start_samples % GROUP_SAMPLES; // 0..=3 leading group-mates
    let first_sample = first_group * GROUP_SAMPLES;
    // Groups covering [start, start+len): the (skip + len) samples from the
    // window start need ceil((skip + len) / 4) groups, clamped to the groups
    // physically present after first_group (the EOF clamp).
    let want = skip + length_samples;
    let groups_needed = want.div_ceil(GROUP_SAMPLES);
    let groups_present = file_size / GROUP_BYTES;
    let groups = groups_needed.min(groups_present.saturating_sub(first_group));

    let mut src = File::open(in_path).map_err(|e| format!("open failed: {e}"))?;
    src.seek(SeekFrom::Start(first_group * GROUP_BYTES))
        .map_err(|e| format!("seek failed: {e}"))?;

    let out = File::create(out_s16)
        .map_err(|e| format!("cannot create {}: {e}", out_s16.display()))?;
    let mut out = BufWriter::with_capacity(8 * 1024 * 1024, out);

    let need = skip + length_samples; // total temp samples covering the cut
    let mut written: u64 = 0; // total temp samples written (incl. the skip)
    let mut group = [0u8; 5];
    let mut batch: Vec<u8> = Vec::with_capacity(8 * 1024 * 1024);
    'outer: for group_index in 0..groups {
        // Poll every 4096 groups (~32 KiB of s16 output), matching the
        // reference converter's cancellation cadence.
        if (group_index & 0xFFF) == 0 && cancel() {
            let _ = std::fs::remove_file(out_s16);
            return Err("cancelled".to_string());
        }
        read_exact_group(&mut src, &mut group)?;
        let samples = unpack_group(&group);
        for s in samples {
            if written >= need {
                break 'outer; // the full [skip, skip+len) window is staged
            }
            batch.extend_from_slice(&s.to_le_bytes());
            written += 1;
        }
        if batch.len() >= 8 * 1024 * 1024 {
            out.write_all(&batch)
                .map_err(|e| format!("write failed: {e}"))?;
            batch.clear();
        }
    }
    if !batch.is_empty() {
        out.write_all(&batch)
            .map_err(|e| format!("write failed: {e}"))?;
    }
    out.flush().map_err(|e| format!("flush failed: {e}"))?;
    Ok(Window {
        first_sample,
        samples: written,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_lds(path: &Path, samples: &[i16]) {
        let mut bytes = Vec::new();
        for g in samples.chunks(4) {
            bytes.extend_from_slice(&pack_group(&[g[0], g[1], g[2], g[3]]));
        }
        std::fs::write(path, &bytes).unwrap();
    }

    fn read_s16(path: &Path) -> Vec<i16> {
        let raw = std::fs::read(path).unwrap();
        raw.chunks_exact(2)
            .map(|c| i16::from_le_bytes([c[0], c[1]]))
            .collect()
    }

    #[test]
    fn golden_group_matches_reference_layout() {
        // Hand-derived from the reference layout: words [0, 1023, 1023, 1023]
        // pack to 00 3F FF FF FF (b0 = w0>>2 = 0; b1 = (w0&3)<<6 | w1>>4 = 63
        // = 0x3F; b2..b4 = 0xFF), and unpack to s16 (-512)*64, (511)*64 x3.
        let b = [0x00, 0x3F, 0xFF, 0xFF, 0xFF];
        let s = unpack_group(&b);
        assert_eq!(s, [-512 * 64, 511 * 64, 511 * 64, 511 * 64]);
        assert_eq!(pack_group(&s), b); // pack round-trips the same values
    }

    #[test]
    fn golden_group_zero() {
        let b = [0x00, 0x00, 0x00, 0x00, 0x00];
        assert_eq!(unpack_group(&b), [-512 * 64; 4]);
        assert_eq!(pack_group(&[-512 * 64; 4]), b);
    }

    #[test]
    fn golden_mixed_words_hand_computed() {
        // Hand-computed from the reference layout, NOT built with the code
        // under test: words [0, 1023, 512, 511] pack to 00 3F F8 01 FF.
        //   b0 =  w0>>2                     = 0x00
        //   b1 = (w0&3)<<6 | (w1&0x3F0)>>4  = 0x00 | 63      = 0x3F
        //   b2 = (w1&0xF)<<4 | (w2&0x3C0)>>6 = 240 | 8      = 0xF8
        //   b3 = (w2&0x3F)<<2 | (w3&0x300)>>8 = 0 | 1       = 0x01
        //   b4 =  w3&0xFF                    = 0xFF
        // and unpack to the s16 values (-512)*64, (511)*64, 0, (-1)*64.
        let b = [0x00, 0x3F, 0xF8, 0x01, 0xFF];
        let s = unpack_group(&b);
        assert_eq!(s, [-512 * 64, 511 * 64, 0, -64]);
        assert_eq!(pack_group(&s), b);
    }

    #[test]
    fn pack_unpack_roundtrip_on_the_grid() {
        // Any s16 that is an exact multiple of 64 (a value already on the
        // 10-bit grid) round-trips byte-exactly.
        for v in [-512i16 * 64, -64, 0, 64, 511 * 64, 3200, -1920] {
            let s = [v, v.wrapping_add(64), 0, -64];
            assert_eq!(unpack_group(&pack_group(&s)), s, "value {v}");
        }
    }

    #[test]
    fn pack_quantizes_like_reference_truncation() {
        // (s16 / 64) truncates toward zero, exactly like the reference C++
        // integer division: -65/64 = -1 (word 511), -1/64 = 0 (word 512).
        let s = [-65i16, 65, -1, 1];
        let b = pack_group(&s);
        let u = unpack_group(&b);
        assert_eq!(u[0], (511 - 512) * 64); // -65 -> word 511
        assert_eq!(u[1], (513 - 512) * 64); // +65 -> word 513
        assert_eq!(u[2], (512 - 512) * 64); // -1 -> word 512 (toward zero)
        assert_eq!(u[3], (512 - 512) * 64); // +1 -> word 512 (toward zero)
    }

    #[test]
    fn extreme_values_never_panic() {
        let s = [i16::MIN, i16::MAX, 0, -1];
        let _ = unpack_group(&pack_group(&s)); // must not panic
    }

    #[test]
    fn total_samples_math() {
        assert_eq!(total_samples(0), (0, 0));
        assert_eq!(total_samples(5), (4, 0));
        assert_eq!(total_samples(10), (8, 0));
        assert_eq!(total_samples(11), (8, 1));
        // The real 352 GB Video8 Tape_019 capture size from the tbc-tools
        // test log: exactly group-aligned.
        assert_eq!(total_samples(351_870_648_320), (281_496_518_656, 0));
    }

    #[test]
    fn window_unpack_is_sample_exact() {
        // 3 groups: 12 ramp samples.
        let dir = std::env::temp_dir();
        let src = dir.join("fc_test_lds_window.lds");
        let samples: Vec<i16> = (0..12).map(|i| (i - 6) * 64).collect();
        write_lds(&src, &samples);

        // Cut [5, 5+6): first_group=1, skip=1 -> first_sample=4; the temp
        // covers s4..s10 INCLUSIVE of the skip mate s4 (7 samples) so the
        // caller's `trim 1s 6s` has the full window.
        let out = dir.join("fc_test_lds_window.s16");
        let w = unpack_window(&src, &out, 5, 6, &|| false).unwrap();
        assert_eq!(w.first_sample, 4);
        assert_eq!(w.samples, 7);
        assert_eq!(
            read_s16(&out),
            vec![(4 - 6) * 64, (5 - 6) * 64, 0, 64, 128, 192, 256]
        );
    }

    #[test]
    fn window_clamps_at_eof_and_rejects_bad_bounds() {
        let dir = std::env::temp_dir();
        let src = dir.join("fc_test_lds_eof.lds");
        write_lds(&src, &(0..8).map(|i| i * 64).collect::<Vec<_>>());

        // Request past the end: writes what exists (from group 0).
        let out = dir.join("fc_test_lds_eof.s16");
        let w = unpack_window(&src, &out, 0, 100, &|| false).unwrap();
        assert_eq!(w.first_sample, 0);
        assert_eq!(w.samples, 8);

        // Start past the end / zero length: hard errors.
        let e = unpack_window(&src, &out, 8, 1, &|| false).unwrap_err();
        assert!(e.contains("past the end"), "{e}");
        let e = unpack_window(&src, &out, 0, 0, &|| false).unwrap_err();
        assert!(e.contains("length"), "{e}");
    }

    #[test]
    fn window_last_group_eof_clamp() {
        // 5 samples of groups: start inside the LAST group, length past EOF.
        let dir = std::env::temp_dir();
        let src = dir.join("fc_test_lds_last.lds");
        let samples: Vec<i16> = (0..8).map(|i| (i - 4) * 64).collect();
        write_lds(&src, &samples);
        let out = dir.join("fc_test_lds_last.s16");
        // start=5 (group 1, skip 1), len=100 -> only the last group remains:
        // the temp is its 4 samples (s4..s7, including the skip mate s4) and
        // the caller's `trim 1s 100s` clamps to s5..s7 (3 samples).
        let w = unpack_window(&src, &out, 5, 100, &|| false).unwrap();
        assert_eq!(w.first_sample, 4);
        assert_eq!(w.samples, 4);
        assert_eq!(read_s16(&out), vec![(4 - 4) * 64, (5 - 4) * 64, (6 - 4) * 64, (7 - 4) * 64]);
    }

    #[test]
    fn window_cancel_is_honoured_and_temp_removed() {
        let dir = std::env::temp_dir();
        let src = dir.join("fc_test_lds_cancel.lds");
        let samples: Vec<i16> = (0..16_384).map(|i| (i % 1024 - 512) * 64).collect();
        write_lds(&src, &samples);
        let out = dir.join("fc_test_lds_cancel.s16");
        let _ = std::fs::remove_file(&out);
        let e = unpack_window(&src, &out, 0, 16_384, &|| true).unwrap_err();
        assert!(e.contains("cancelled"), "{e}");
        assert!(!out.exists(), "partial temp must be removed on cancel");
    }
}
