//! Bit-perfect conversion verification (the pre-deletion safety net).
//!
//! Converting a tag-less capture (packed `.lds`, WAV, raw PCM) to FLAC is the
//! workflow that frees the source for deletion — the FLAC *replaces* the raw
//! capture. Before any source file is deleted, the output must be proven to
//! decode back to EXACTLY the source samples. This module provides that proof:
//! it decodes the output FLAC (claxon) and compares every sample of the cut
//! window against the source read through the same native input path the
//! conversion itself used (`.lds` group unpack with the `(value - 512) * 64`
//! scaling, the WAV data chunk at its fmt bit depth, raw u8/s8/u16/s16 with
//! SoX's width/sign semantics). Both whole-file conversions and partial cuts
//! verify (`skip(start)` + `take(length)` over the source stream).
//!
//! Only *plain* conversions can be bit-perfect: when output processing is
//! requested (a rate change or a bit-depth change), the samples are
//! intentionally transformed and there is nothing to compare against — those
//! conversions report an explicit skip instead, so the user knows the output
//! was NOT proven and the source must be kept. FLAC and Ogg FLAC sources are
//! lossless cuts by construction and are not verified at all.
//!
//! The verdict is surfaced through the chop result's stderr notes channel:
//! `verify: PASS (N samples, bit-perfect) — safe to delete the source`, or a
//! loud `warning: verify: FAILED ...` (the GUI pops the warnings). FLAC-Chop
//! itself never deletes sources; the PASS note is the green light for the
//! user's own deletion workflow.

use std::path::Path;

use crate::probe::InputFormat;

/// Outcome of a bit-perfect conversion verification.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VerifyOutcome {
    /// Every decoded output sample equals the corresponding source sample.
    /// `samples` is the compared sample count.
    Pass { samples: u64 },
    /// The decoded stream differs from the source: the first differing value
    /// pair, a decode error, or a length mismatch.
    Fail { samples: u64, reason: String },
    /// The source cannot be compared bit-perfectly (e.g. a non-integer-PCM
    /// WAV); the caller reports a skip so the user keeps the source.
    Unsupported(String),
}

/// The open source stream, or an explicit unsupported-skip.
enum Source {
    Samples(Box<dyn Iterator<Item = Result<i32, String>> + Send>),
    Unsupported(String),
}

/// WAV fmt/data facts from a compact RIFF walk (integer-PCM check included).
struct WavFacts {
    /// The fmt chunk's audio format (1 = integer PCM).
    audio_format: u16,
    channels: u16,
    bits: u16,
    data_offset: u64,
    data_len: u64,
}

fn wav_facts(path: &Path) -> Result<WavFacts, String> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).map_err(|e| format!("open failed: {e}"))?;
    let rd_u16 = |f: &mut std::fs::File| -> Result<u16, String> {
        let mut b = [0u8; 2];
        f.read_exact(&mut b).map_err(|e| format!("read failed: {e}"))?;
        Ok(u16::from_le_bytes(b))
    };
    let rd_u32 = |f: &mut std::fs::File| -> Result<u32, String> {
        let mut b = [0u8; 4];
        f.read_exact(&mut b).map_err(|e| format!("read failed: {e}"))?;
        Ok(u32::from_le_bytes(b))
    };
    let mut magic = [0u8; 4];
    f.read_exact(&mut magic).map_err(|e| format!("read failed: {e}"))?;
    if &magic != b"RIFF" {
        return Err("not a RIFF/WAV file (missing RIFF marker)".to_string());
    }
    let _riff_len = rd_u32(&mut f)?;
    f.read_exact(&mut magic).map_err(|e| format!("read failed: {e}"))?;
    if &magic != b"WAVE" {
        return Err("not a RIFF/WAV file (missing WAVE marker)".to_string());
    }
    let mut facts: Option<WavFacts> = None;
    let mut data: Option<(u64, u64)> = None; // (offset, len)
    loop {
        let mut chunk_id = [0u8; 4];
        if f.read_exact(&mut chunk_id).is_err() {
            break; // ran out of chunks
        }
        let chunk_len = u64::from(rd_u32(&mut f)?);
        let chunk_offset = f
            .stream_position()
            .map_err(|e| format!("seek failed: {e}"))?;
        if &chunk_id == b"fmt " {
            let audio_format = rd_u16(&mut f)?;
            let channels = rd_u16(&mut f)?;
            let _sample_rate = rd_u32(&mut f)?;
            let _byte_rate = rd_u32(&mut f)?;
            let _block_align = rd_u16(&mut f)?;
            let bits = rd_u16(&mut f)?;
            facts = Some(WavFacts {
                audio_format,
                channels,
                bits,
                data_offset: 0,
                data_len: 0,
            });
        } else if &chunk_id == b"data" {
            data = Some((chunk_offset, chunk_len));
            // Keep walking only while the fmt chunk has not been seen yet.
            if facts.is_some() {
                break;
            }
        }
        // Chunks are word-aligned (odd lengths carry a pad byte).
        let skip = chunk_len + (chunk_len & 1);
        if f
            .seek(SeekFrom::Current(skip as i64))
            .map_err(|e| format!("seek failed: {e}"))?
            >= std::fs::metadata(path)
                .map_err(|e| format!("stat failed: {e}"))?
                .len()
        {
            break;
        }
    }
    let mut facts = facts.ok_or_else(|| "WAV has no fmt chunk".to_string())?;
    let (data_offset, data_len) = data.ok_or_else(|| "WAV has no data chunk".to_string())?;
    facts.data_offset = data_offset;
    facts.data_len = data_len;
    Ok(facts)
}

/// A chunked byte-to-sample reader: reads from `start_offset`, converts every
/// `step` bytes with `convert` (SoX's width/sign semantics), and yields the
/// interleaved samples in order. `byte_budget` (the data chunk size for WAV)
/// bounds the usable bytes (a trailing partial frame is dropped, like SoX).
fn sample_iter(
    path: &Path,
    start_offset: u64,
    byte_budget: Option<u64>,
    step: usize,
    convert: fn(&[u8]) -> i32,
) -> Result<Box<dyn Iterator<Item = Result<i32, String>> + Send>, String> {
    use std::io::{BufReader, Read, Seek, SeekFrom};
    let mut src = std::fs::File::open(path).map_err(|e| format!("open failed: {e}"))?;
    src.seek(SeekFrom::Start(start_offset))
        .map_err(|e| format!("seek failed: {e}"))?;
    let mut src = BufReader::with_capacity(1 << 20, src);
    let mut remaining = byte_budget;
    let mut buf = vec![0u8; 1 << 16];
    let mut pending: std::collections::VecDeque<i32> = std::collections::VecDeque::new();
    Ok(Box::new(std::iter::from_fn(move || {
        loop {
            if let Some(v) = pending.pop_front() {
                return Some(Ok(v));
            }
            // How many whole frames fit in this read?
            let want = match remaining {
                Some(r) if r == 0 => return None,
                Some(r) => r.min(buf.len() as u64) as usize,
                None => buf.len(),
            };
            match src.read(&mut buf[..want]) {
                Ok(0) => return None, // EOF
                Ok(n) => {
                    if let Some(r) = &mut remaining {
                        *r -= n as u64;
                    }
                    let whole = n / step;
                    if whole == 0 {
                        continue; // trailing partial frame: dropped
                    }
                    for i in 0..whole {
                        pending.push_back(convert(&buf[i * step..i * step + step]));
                    }
                }
                Err(e) => return Some(Err(format!("read failed: {e}"))),
            }
        }
    })))
}

/// SoX's raw/WAV sample semantics at each integer width (what SoX reads a
/// byte sequence as, and therefore what a bit-perfect output decodes to).
fn conv_u8(b: &[u8]) -> i32 {
    i32::from(b[0]) - 128 // unsigned, centre 128
}
fn conv_s8(b: &[u8]) -> i32 {
    i32::from(b[0] as i8)
}
fn conv_u16(b: &[u8]) -> i32 {
    i32::from(u16::from_le_bytes([b[0], b[1]])) - 32768
}
fn conv_s16(b: &[u8]) -> i32 {
    i32::from(i16::from_le_bytes([b[0], b[1]]))
}
fn conv_s24(b: &[u8]) -> i32 {
    let v = u32::from(b[0]) | (u32::from(b[1]) << 8) | (u32::from(b[2]) << 16);
    ((v << 8) as i32) >> 8 // sign-extend 24 -> 32
}
fn conv_s32(b: &[u8]) -> i32 {
    i32::from_le_bytes([b[0], b[1], b[2], b[3]])
}
/// 8-bit sources feeding a 16-bit output (the GPU .lds-packed path — or any
/// 16-bit encode) compare at the standard exact ×256 scaling: (b-128)*256
/// / s*256 are exactly what the 16-bit container holds.
fn conv_u8_x256(b: &[u8]) -> i32 {
    (i32::from(b[0]) - 128) * 256
}
fn conv_s8_x256(b: &[u8]) -> i32 {
    i32::from(b[0] as i8) * 256
}

/// Open the source capture's sample stream in input order, as the conversion
/// reads it. `out_bits` is the OUTPUT FLAC's bits-per-sample (from its
/// STREAMINFO): an 8-bit source feeding an 8-bit output (the SoX path)
/// compares at its native depth; feeding a 16-bit output (the GPU
/// .lds-packed path, or any 16-bit encode) compares at the exact ×256
/// scaling the 16-bit container holds — both bit-perfect at their depth.
/// Errors on a malformed container; returns [`Source::Unsupported`] when the
/// format cannot be compared bit-perfectly.
fn open_source(path: &Path, fmt: InputFormat, out_bits: u16) -> Result<Source, String> {
    match fmt {
        InputFormat::Lds => {
            let file_size = std::fs::metadata(path)
                .map_err(|e| format!("stat failed: {e}"))?
                .len();
            // Whole packing groups only — the conversion path drops a
            // trailing partial group too (total from whole groups).
            let (total, _trailing) = crate::lds::total_samples(file_size);
            let mut src = std::fs::File::open(path).map_err(|e| format!("open failed: {e}"))?;
            use std::io::{BufReader, Seek, SeekFrom};
            src.seek(SeekFrom::Start(0))
                .map_err(|e| format!("seek failed: {e}"))?;
            let mut src = BufReader::with_capacity(8 * 1024 * 1024, src);
            let mut group = [0u8; 5];
            let mut remaining = total;
            let mut pending: std::collections::VecDeque<i32> = std::collections::VecDeque::new();
            Ok(Source::Samples(Box::new(std::iter::from_fn(move || {
                loop {
                    if let Some(v) = pending.pop_front() {
                        return Some(Ok(v));
                    }
                    if remaining == 0 {
                        return None;
                    }
                    match crate::lds::read_exact_group(&mut src, &mut group) {
                        Ok(()) => {}
                        Err(e) => return Some(Err(e)),
                    }
                    remaining -= 4.min(remaining);
                    for s in crate::lds::unpack_group(&group) {
                        pending.push_back(i32::from(s));
                    }
                }
            }))))
        }
        InputFormat::Wav => {
            let facts = wav_facts(path)?;
            if facts.audio_format != 1 {
                return Ok(Source::Unsupported(format!(
                    "WAV audio format {} is not integer PCM (bit-perfect comparison undefined)",
                    facts.audio_format
                )));
            }
            let (step, convert): (usize, fn(&[u8]) -> i32) = match facts.bits {
                8 => (1, conv_u8),
                16 => (2, conv_s16),
                24 => (3, conv_s24),
                32 => (4, conv_s32),
                other => {
                    return Ok(Source::Unsupported(format!(
                        "WAV bit depth {other} is not a SoX-verified integer depth"
                    )))
                }
            };
            let _ = facts.channels; // interleaved order either way
            Ok(Source::Samples(sample_iter(
                path,
                facts.data_offset,
                Some(facts.data_len),
                step,
                convert,
            )?))
        }
        InputFormat::U8 => {
            let convert = if out_bits >= 16 { conv_u8_x256 } else { conv_u8 };
            Ok(Source::Samples(sample_iter(path, 0, None, 1, convert)?))
        }
        InputFormat::S8 => {
            let convert = if out_bits >= 16 { conv_s8_x256 } else { conv_s8 };
            Ok(Source::Samples(sample_iter(path, 0, None, 1, convert)?))
        }
        InputFormat::U16 => Ok(Source::Samples(sample_iter(
            path, 0, None, 2, conv_u16,
        )?)),
        InputFormat::S16 => Ok(Source::Samples(sample_iter(
            path, 0, None, 2, conv_s16,
        )?)),
        // FLAC-family sources: lossless cuts by construction — the caller
        // never asks to verify those (an explicit internal guard).
        InputFormat::Flac | InputFormat::OggFlac => Err(
            "FLAC-family sources are not verified (lossless cuts by construction)".to_string(),
        ),
    }
}

/// Verify that the output FLAC at `out_path` decodes to EXACTLY the source
/// capture's cut window `[start, start + length)` (a length clamped at the
/// source end, like the cutter, verifies too: both streams simply end at
/// the same sample). `fmt` is the source's probed format.
pub fn verify_conversion(
    in_path: &Path,
    out_path: &Path,
    fmt: InputFormat,
    start: u64,
    length: u64,
) -> Result<VerifyOutcome, String> {
    let mut reader = claxon::FlacReader::open(out_path)
        .map_err(|e| format!("output FLAC unreadable: {e}"))?;
    let out_bits = reader.streaminfo().bits_per_sample as u16;
    let source = match open_source(in_path, fmt, out_bits)? {
        Source::Samples(it) => it,
        Source::Unsupported(reason) => return Ok(VerifyOutcome::Unsupported(reason)),
    };
    let mut decoded = reader.samples();
    let mut count: u64 = 0;
    let mut source = source.skip(usize::try_from(start).map_err(|_| "start out of range")?);
    let take = usize::try_from(length).map_err(|_| "length out of range")?;
    let mut left: u64 = 0;
    let mut iter = source.by_ref().take(take);
    loop {
        // The source window is `length` samples (or everything past `start`
        // when the source is shorter — the cutter clamps the same way).
        let s = match iter.next() {
            Some(v) => v?,
            None => break,
        };
        left += 1;
        match decoded.next() {
            Some(Ok(d)) => {
                if d != s {
                    return Ok(VerifyOutcome::Fail {
                        samples: count,
                        reason: format!("sample {count} differs: source {s}, output {d}"),
                    });
                }
                count += 1;
            }
            Some(Err(e)) => {
                return Ok(VerifyOutcome::Fail {
                    samples: count,
                    reason: format!("output decode failed at sample {count}: {e}"),
                });
            }
            None => {
                return Ok(VerifyOutcome::Fail {
                    samples: count,
                    reason: format!(
                        "output decoded fewer samples ({count}) than the cut window ({left} expected so far)"
                    ),
                });
            }
        }
    }
    if let Some(first_extra) = decoded.next() {
        return Ok(VerifyOutcome::Fail {
            samples: count,
            reason: match first_extra {
                Ok(d) => format!(
                    "output decoded MORE samples than the cut window (extra sample {count} = {d})"
                ),
                Err(e) => format!("output decode failed after sample {count}: {e}"),
            },
        });
    }
    Ok(VerifyOutcome::Pass { samples: count })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flac_family_sources_error_not_panic() {
        let r = verify_conversion(
            Path::new("/nonexistent.lds"),
            Path::new("/nonexistent.flac"),
            InputFormat::Flac,
            0,
            1,
        );
        assert!(r.is_err());
    }

    #[test]
    fn missing_source_files_error_cleanly() {
        for fmt in [
            InputFormat::Lds,
            InputFormat::Wav,
            InputFormat::U8,
            InputFormat::S16,
        ] {
            let r = verify_conversion(
                Path::new("/nonexistent-source-file"),
                Path::new("/nonexistent-output-file"),
                fmt,
                0,
                1,
            );
            assert!(r.is_err(), "{fmt:?}");
        }
    }
}
