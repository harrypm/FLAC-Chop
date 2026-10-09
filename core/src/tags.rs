//! Rewrite the RF metadata Vorbis comment tags on a *cut* output FLAC so they
//! reflect the new altered metadata, matching the embedding model MISRC-GUI uses
//! (libFLAC Vorbis comments).
//!
//! SoX `trim` preserves the input's Vorbis comments verbatim, so a 100k-sample
//! cut of a 10425 s capture still shows `RF_TOTAL_SAMPLES=208511433850` — the
//! stale full-capture values. This module rewrites the numeric RF tags on the
//! output file from the output's own STREAMINFO (the source of truth after the
//! cut), while leaving ingest metadata (`project`, `tape_id`, `operator`,
//! `location`, `notes`, …) untouched (SoX already passed them through).
//!
//! It also exposes a general Vorbis-comment editor API
//! ([`read_all_comments`] / [`replace_all_comments`]) used by the GUI's
//! Metadata Editor tab to view and rewrite every comment on the *source* file
//! in place — the same in-place / splice write path the cut-tag rewrite uses.
//!
//! Tag schema (from MISRC-GUI `gui_record.c` + the capture pipeline):
//!
//! ```text
//! RF_TOTAL_SAMPLES   = true on-disk sample count (1:1 real RF samples)
//! RF_SAMPLE_RATE     = real rate in Hz   (header_khz * 1000 for RF)
//! RF_SAMPLE_RATE_KHZ = header kHz value   (RF only; = header sample_rate)
//! DURATION_SECONDS   = real duration (s)  (= on-disk count / real rate)
//! LENGTH             = duration in ms
//! ```
//!
//! For RF files the FLAC header `sample_rate` holds the /1000 "kHz" value
//! (e.g. 20000 for 20 MSPS); `RF_TOTAL_SAMPLES` is the STREAMINFO
//! `total_samples` taken as the true on-disk count — every stored sample IS
//! one real RF sample (only the header RATE is /1000), matching the
//! MISRC-GUI writer and the probe's later-schema reader. For non-RF audio
//! the header `sample_rate` is the real Hz and `RF_SAMPLE_RATE_KHZ` is
//! omitted.
//!
//! The write path mirrors the MISRC-GUI recorder method (`misrc_tools/common/
//! flac_writer.c` + `gui_record_finalize_flac_metadata`): metadata updates are
//! done **in place** — the VC block is rewritten in the same byte region,
//! growing into the adjacent PADDING block when needed — so there is no temp
//! file and no full-file rewrite. When the update cannot fit (no adjacent
//! padding and the vendor string cannot absorb the delta), the fallback is a
//! full verified splice (temp file + rename) that leaves a MISRC-style
//! 4096-byte PADDING after the VC so future updates fit in place. The old
//! lofty-based writer was dropped entirely: lofty 0.18's FLAC writer corrupts
//! real cut outputs (frames displaced one byte past the declared metadata end
//! → `LOST_SYNC` on decode) and panics on padding-less files.

use claxon::{FlacReader, FlacReaderOptions};
use std::fs::File;
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

/// The numeric RF tags we own and rewrite. Anything else (ingest metadata,
/// `ENCODER`, pictures, …) is left untouched.
const OWNED_TAGS: &[&str] = &[
    "RF_TOTAL_SAMPLES",
    "RF_SAMPLE_RATE",
    "RF_SAMPLE_RATE_KHZ",
    "DURATION_SECONDS",
    "LENGTH",
];

/// Read the output FLAC's STREAMINFO and rewrite the numeric RF Vorbis tags to
/// match the actual cut. `is_rf` selects the RF rate convention (RF_SAMPLE_RATE
/// = header*1000 real Hz, + RF_SAMPLE_RATE_KHZ) versus plain audio
/// (RF_SAMPLE_RATE = header, no KHZ tag); in both cases RF_TOTAL_SAMPLES is
/// the output's true on-disk sample count (1:1 real RF samples — the
/// MISRC-GUI writer convention, and what the probe's later-schema reader
/// expects). Duration tags derive from the real rate, never the kHz header.
///
/// `extra` carries caller-authored "KEY=value" comments for the output (the
/// GUI's pre-conversion metadata editor rows for raw/packed sources, which
/// cannot carry tags themselves): each extra key replaces the same-key
/// comment in the output, applied BEFORE the owned numeric RF tags — which
/// always win (their values come from the output's own STREAMINFO).
///
/// Returns `Ok(())` on success, or an error string describing why the rewrite
/// failed (the caller may surface it as a non-fatal warning — the cut itself
/// already succeeded).
pub fn rewrite_cut_tags(out_path: &Path, is_rf: bool, extra: &[String]) -> Result<(), String> {
    // Read the output's STREAMINFO for the authoritative post-cut numbers.
    let (header_rate, total_samples) = read_streaminfo(out_path)?;
    if header_rate == 0 {
        return Err("output STREAMINFO sample_rate is 0".into());
    }
    // The on-disk count is 1:1 real RF samples; the RF header rate is the
    // /1000 kHz value, so the real duration derives from header*1000 (for
    // non-RF audio the header rate is already the real rate).
    let real_rate_hz: u64 = if is_rf {
        (header_rate as u64) * 1000
    } else {
        header_rate as u64
    };
    let duration_sec = total_samples as f64 / real_rate_hz as f64;
    let length_ms = (duration_sec * 1000.0).round() as u64;

    // Fresh values from the output's own STREAMINFO (1:1 on-disk counts).
    let (rf_sample_rate, rf_total_samples) = if is_rf {
        (real_rate_hz, total_samples)
    } else {
        (header_rate as u64, total_samples)
    };

    // The updated tag set, in the MISRC-GUI push order. Replaces the stale
    // numeric RF tags SoX passed through; every other comment is untouched.
    let mut updates: Vec<(&str, String)> = vec![
        ("RF_TOTAL_SAMPLES", rf_total_samples.to_string()),
        ("RF_SAMPLE_RATE", rf_sample_rate.to_string()),
        ("DURATION_SECONDS", format!("{duration_sec:.6}")),
        ("LENGTH", length_ms.to_string()),
    ];
    if is_rf {
        // RF_SAMPLE_RATE_KHZ = header kHz value.
        updates.insert(0, ("RF_SAMPLE_RATE_KHZ", header_rate.to_string()));
    }
    debug_assert!(
        updates.iter().all(|(k, _)| OWNED_TAGS.contains(k)),
        "rewrite must only touch owned numeric RF tags"
    );

    // MISRC-GUI method: update the VC block in place (growing into adjacent
    // padding / absorbing slack into padding), with no temp file and no
    // full-file rewrite. Only fall back to the full splice when the update
    // cannot fit in the existing metadata region.
    match update_vorbis_comment_in_place(out_path, &updates, extra) {
        Ok(true) => Ok(()),
        Ok(false) => splice_vorbis_comment(out_path, &updates, extra),
        Err(e) => Err(e),
    }
}

/// Try to update the VORBIS_COMMENT block entirely in place, the way the
/// MISRC-GUI recorder finalizer does (`gui_record_finalize_flac_metadata`):
/// write the new VC payload over the old VC block region, sized so the region
/// covers exactly the same bytes — every other byte of the file, including
/// all audio frames, stays untouched and no temp file is needed. The size is
/// equalized by:
///
/// 1. the PADDING block adjacent after the VC, when one is present (the MISRC
///    writer deliberately places a 4096-byte one there): it shrinks or grows
///    to absorb the delta — the `set_block(use_padding=true)` case;
/// 2. otherwise (the SoX cut layout: STREAMINFO → VC → frames) trimming the
///    vendor string by exactly the growth, or inserting a PADDING block for
///    the slack on shrink. The vendor is an informative string only — a few
///    bytes off it is invisible next to the full-file rewrite the alternative
///    would need.
///
/// Returns `Ok(true)` when the in-place update was applied, `Ok(false)` when
/// it cannot fit (caller falls back to the full splice, which leaves
/// MISRC-style padding behind so the case does not recur), `Err` on I/O or
/// malformed-container errors. The file is only written after every fallible
/// step has passed, and the self-check refuses to leave a broken file behind
/// silently.
fn update_vorbis_comment_in_place(
    path: &Path,
    updates: &[(&str, String)],
    extra: &[String],
) -> Result<bool, String> {
    let (blocks, frames_start) = parse_metadata_chain(path)?;
    let src_len = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    let orig_frames_len = src_len
        .checked_sub(frames_start)
        .ok_or_else(|| format!("frame start {frames_start} past end of file ({src_len})"))?;
    if orig_frames_len == 0 {
        return Err("no audio frames after metadata".to_string());
    }

    let vc_idx = match blocks.iter().position(|b| b.kind == 4) {
        Some(i) => i,
        None => return Ok(false), // no VC: full splice inserts one
    };
    let (vendor, mut comments) = parse_vorbis_comment(&blocks[vc_idx].payload)?;
    merge_extra_comments(&mut comments, extra);
    for (key, _) in updates {
        let prefix = format!("{key}=");
        comments.retain(|c| !c.starts_with(&prefix));
    }
    for (key, val) in updates {
        comments.push(format!("{key}={val}"));
    }

    write_vc_in_place_core(path, &blocks, frames_start, src_len, vc_idx, &vendor, &comments)
}

/// Core in-place VC writer: write `comments` (a full "KEY=value" list) over the
/// existing VORBIS_COMMENT block at `vc_idx`, equalizing the covered byte
/// region against the adjacent PADDING block / the vendor string so every other
/// byte of the file (including all audio frames) stays untouched and no temp
/// file is needed. `src_len` is the file size before the write (reused from the
/// caller). Returns `Ok(true)` when applied, `Ok(false)` when it cannot fit
/// (the caller falls back to the full splice), `Err` on I/O or
/// malformed-container errors. The file is only written after every fallible
/// step has passed, and the self-check refuses to leave a broken file behind
/// silently.
fn write_vc_in_place_core(
    path: &Path,
    blocks: &[MetaBlock],
    frames_start: u64,
    src_len: u64,
    vc_idx: usize,
    vendor: &str,
    comments: &[String],
) -> Result<bool, String> {
    // Byte offset of the VC block, the block layout around it, and the total
    // byte size of the region the rewrite must cover exactly.
    let vc_off: u64 = 4 + blocks[..vc_idx].iter().map(|b| 4 + b.payload.len() as u64).sum::<u64>();
    let old_len = blocks[vc_idx].payload.len();
    let pad_follows = blocks.get(vc_idx + 1).filter(|b| b.kind == 1).map(|b| b.payload.len());
    let old_region_len = 4 + old_len as u64 + pad_follows.map_or(0, |p| 4 + p as u64);
    // The trailing PADDING keeps the chain's last-flag when it is the final
    // block; the VC only carries the flag when no padding follows it.
    let pad_is_last = pad_follows.is_some() && vc_idx + 2 == blocks.len();

    // Build the equal-size replacement region for a given vendor string.
    // `None` = the update cannot be represented with this vendor.
    let build_region = |vendor: &str| -> Option<Vec<u8>> {
        let payload = build_vorbis_comment(vendor, comments);
        if payload.len() > 0xff_ffff {
            return None;
        }
        let mut region = Vec::new();
        match pad_follows {
            Some(pad) => {
                // Absorb the delta into the adjacent PADDING block (a
                // zero-length PADDING block is valid). Growth past the
                // padding is handled by the caller trimming the vendor.
                let new_pad = pad as i64 - (payload.len() as i64 - old_len as i64);
                if new_pad < 0 {
                    return None;
                }
                region.push(0x04); // VC: not last, padding follows
                region.extend_from_slice(&(payload.len() as u32).to_be_bytes()[1..]);
                region.extend_from_slice(&payload);
                // PADDING block header: type 1, last-flag by chain position.
                region.push(if pad_is_last { 0x81 } else { 0x01 });
                region.extend_from_slice(&(new_pad as u32).to_be_bytes()[1..]);
                region.extend(std::iter::repeat(0u8).take(new_pad as usize));
            }
            None => {
                // VC is the last metadata block (SoX cut layout).
                let slack = old_len as i64 - payload.len() as i64;
                if payload.len() == old_len {
                    // Exact fit: rewrite the block in place, VC stays last.
                    region.push(0x04 | 0x80);
                    region.extend_from_slice(&(payload.len() as u32).to_be_bytes()[1..]);
                    region.extend_from_slice(&payload);
                } else if slack >= 4 {
                    // Shrink: insert a PADDING block for the slack.
                    let pad_len = slack - 4;
                    region.push(0x04); // VC no longer last
                    region.extend_from_slice(&(payload.len() as u32).to_be_bytes()[1..]);
                    region.extend_from_slice(&payload);
                    region.push(0x81); // inserted PADDING (type 1) is last
                    region.extend_from_slice(&(pad_len as u32).to_be_bytes()[1..]);
                    region.extend(std::iter::repeat(0u8).take(pad_len as usize));
                } else {
                    // Shrink by 1..=3 bytes: no room for a PADDING header and
                    // the vendor cannot grow — full splice handles it.
                    return None;
                }
            }
        }
        if region.len() as u64 != old_region_len {
            return None; // sizing bug — never write a mismatched region
        }
        Some(region)
    };

    // Try the original vendor first (covers delta == 0, shrink-with-slack, and
    // any growth the adjacent padding absorbs); otherwise trim the vendor by
    // exactly the shortfall.
    let new_len = build_vorbis_comment(vendor, comments).len();
    let delta = new_len as i64 - old_len as i64;
    let shortfall = match pad_follows {
        Some(pad) => (delta - pad as i64).max(0), // growth past the padding
        None => delta.max(0),                     // no padding: growth needs the trim
    };
    let region = if shortfall == 0 {
        match build_region(vendor) {
            Some(r) => r,
            None => return Ok(false),
        }
    } else if shortfall as usize <= vendor.len() {
        let v = &vendor[..vendor.len() - shortfall as usize];
        match build_region(v) {
            Some(r) => r,
            None => return Ok(false),
        }
    } else {
        return Ok(false); // cannot fit → full splice
    };

    // Every fallible step has passed — write the equal-size region over the
    // old one. Frames are never touched.
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .open(path)
        .map_err(|e| format!("open: {e}"))?;
    f.seek(SeekFrom::Start(vc_off)).map_err(|e| format!("seek: {e}"))?;
    f.write_all(&region).map_err(|e| format!("write region: {e}"))?;

    // Self-verify: the chain must re-parse to the same frame start (file size
    // unchanged) with a frame sync there.
    let (_, vframes) = parse_metadata_chain(path)?;
    let new_file_len = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    if vframes != frames_start || new_file_len != src_len {
        return Err(format!(
            "in-place update verification failed: frames at {vframes} (was {frames_start}), file {new_file_len} B (was {src_len})"
        ));
    }
    let mut vf = File::open(path).map_err(|e| format!("reopen: {e}"))?;
    vf.seek(SeekFrom::Start(vframes)).map_err(|e| format!("seek: {e}"))?;
    let mut sync = [0u8; 2];
    vf.read_exact(&mut sync).map_err(|e| format!("read sync: {e}"))?;
    if sync[0] != 0xff || (sync[1] & 0xfc) != 0xf8 {
        return Err(format!(
            "in-place update verification failed: no frame sync at offset {vframes} (bytes {:02x} {:02x})",
            sync[0], sync[1]
        ));
    }
    Ok(true)
}

/// One FLAC metadata block: type + payload (the 4-byte block header is rebuilt
/// on write, so stored payloads never carry stale length bytes).
struct MetaBlock {
    /// Block type (0 = STREAMINFO, 4 = VORBIS_COMMENT, 1 = PADDING, …).
    kind: u8,
    /// Block payload exactly as it sits on disk (header excluded).
    payload: Vec<u8>,
}

/// Parse the metadata chain of the FLAC at `path`.
///
/// Returns the blocks (in order, payloads only, last-flags stripped — flags
/// are rebuilt from position on write) and the byte offset where the audio
/// frames begin. Errors if the container does not look like well-formed FLAC
/// front matter — never patch a file we don't fully understand.
fn parse_metadata_chain(path: &Path) -> Result<(Vec<MetaBlock>, u64), String> {
    let mut f = File::open(path).map_err(|e| format!("open: {e}"))?;
    let mut magic = [0u8; 4];
    f.read_exact(&mut magic).map_err(|e| format!("read magic: {e}"))?;
    if &magic == b"OggS" {
        // Ogg FLAC (.ldf): Vorbis comments live in Ogg header packets, which
        // the in-place / splice writers cannot rewrite safely. Say so plainly
        // instead of the misleading "not a FLAC file".
        return Err(
            "this is an Ogg FLAC file (.ldf): its metadata cannot be edited in place — FLAC-Chop cuts from it fine, and the cut output is a native FLAC whose tags are editable"
                .to_string(),
        );
    }
    if &magic != b"fLaC" {
        return Err("not a FLAC file".to_string());
    }
    let mut blocks = Vec::new();
    let mut pos: u64 = 4;
    loop {
        let mut hdr = [0u8; 4];
        f.read_exact(&mut hdr).map_err(|e| format!("read block hdr: {e}"))?;
        let kind = hdr[0] & 0x7f;
        let is_last = hdr[0] & 0x80 != 0;
        let len = u32::from(hdr[1]) << 16 | u32::from(hdr[2]) << 8 | u32::from(hdr[3]);
        let mut payload = vec![0u8; len as usize];
        f.read_exact(&mut payload).map_err(|e| format!("read block: {e}"))?;
        pos += 4 + len as u64;
        blocks.push(MetaBlock { kind, payload });
        if is_last {
            break;
        }
    }
    Ok((blocks, pos))
}

/// Parse a VORBIS_COMMENT payload: `vendor_len` (u32 LE) + vendor + `count`
/// (u32 LE) + (`len` (u32 LE) + "KEY=value") × count.
fn parse_vorbis_comment(payload: &[u8]) -> Result<(String, Vec<String>), String> {
    let u32le = |p: &[u8]| -> Result<u32, String> {
        let mut b = [0u8; 4];
        b.copy_from_slice(p.get(0..4).ok_or("vorbis comment truncated")?);
        Ok(u32::from_le_bytes(b))
    };
    let vlen = u32le(payload)? as usize;
    let mut pos = 4;
    let vendor = String::from_utf8(
        payload
            .get(pos..pos + vlen)
            .ok_or("vorbis vendor truncated")?
            .to_vec(),
    )
    .map_err(|_| "vorbis vendor not UTF-8".to_string())?;
    pos += vlen;
    let count = u32le(payload.get(pos..).ok_or("vorbis count truncated")?)? as usize;
    pos += 4;
    let mut comments = Vec::with_capacity(count.min(1024));
    for _ in 0..count {
        let clen = u32le(payload.get(pos..).ok_or("comment len truncated")?)? as usize;
        pos += 4;
        let c = payload.get(pos..pos + clen).ok_or("comment truncated")?;
        pos += clen;
        comments.push(String::from_utf8(c.to_vec()).map_err(|_| "comment not UTF-8".to_string())?);
    }
    Ok((vendor, comments))
}

/// Merge caller-authored `extra` "KEY=value" comments into `comments` (used
/// by both splice paths): each extra key — split at the FIRST '=', matched
/// case-insensitively — replaces any same-key comment already in the list,
/// then the extra row is appended (the key is trimmed + upper-cased, the
/// value kept verbatim). Rows without '=' or with an empty key are skipped.
/// Applied BEFORE the owned numeric RF updates, so those always win.
fn merge_extra_comments(comments: &mut Vec<String>, extra: &[String]) {
    for e in extra {
        let e = e.trim();
        if e.is_empty() {
            continue;
        }
        let Some(eq) = e.find('=') else { continue };
        let key = e[..eq].trim().to_ascii_uppercase();
        if key.is_empty() {
            continue;
        }
        comments.retain(|c| match c.find('=') {
            Some(i) => !c[..i].eq_ignore_ascii_case(&key),
            None => true,
        });
        comments.push(format!("{key}={}", &e[eq + 1..]));
    }
}

/// Build a VORBIS_COMMENT payload from `vendor` + comment list.
fn build_vorbis_comment(vendor: &str, comments: &[String]) -> Vec<u8> {
    let mut out = Vec::new();
    let v = vendor.as_bytes();
    out.extend_from_slice(&(v.len() as u32).to_le_bytes());
    out.extend_from_slice(v);
    out.extend_from_slice(&(comments.len() as u32).to_le_bytes());
    for c in comments {
        let b = c.as_bytes();
        out.extend_from_slice(&(b.len() as u32).to_le_bytes());
        out.extend_from_slice(b);
    }
    out
}

/// Apply a set of key-targeted `updates`: rebuild the VC block with the
/// updates applied on top of the existing comments plus a MISRC-style
/// 4096-byte PADDING, and write it in. Used by the cut rewrite path when the
/// in-place region update cannot fit. The GUI editor uses
/// [`replace_all_comments`] (full list).
///
/// Primary: the in-place shift splice ([`shift_splice_in_place`]) — no temp
/// file, no rename, and the only new disk allocation is the metadata growth
/// itself, so embedding the tags works on a nearly-full disk. Fallback: the
/// verified temp-file splice ([`splice_vc_core`]) — atomic, but it needs
/// output-sized free space beside the file.
fn splice_vorbis_comment(
    path: &Path,
    updates: &[(&str, String)],
    extra: &[String],
) -> Result<(), String> {
    let (blocks, frames_start) = parse_metadata_chain(path)?;

    // Existing Vorbis comments (vendor + comment list), or empty defaults.
    let (vendor, mut comments) = match blocks.iter().find(|b| b.kind == 4) {
        Some(b) => parse_vorbis_comment(&b.payload)?,
        None => ("FLAC-Chop".to_string(), Vec::new()),
    };

    // Caller-authored extras first (each replaces the same-key comment);
    // the owned updates below then override the numeric RF keys.
    merge_extra_comments(&mut comments, extra);
    // Drop every existing occurrence of an updated key, then append fresh
    // values (dup keys are legal in Vorbis comments — never leave stale ones).
    for (key, _) in updates {
        let prefix = format!("{key}=");
        comments.retain(|c| !c.starts_with(&prefix));
    }
    for (key, val) in updates {
        comments.push(format!("{key}={val}"));
    }

    let new_payload = build_vorbis_comment(&vendor, &comments);
    if new_payload.len() > 0xff_ffff {
        return Err(format!("vorbis comment too large ({} bytes)", new_payload.len()));
    }

    // Primary: the in-place shift splice — no temp file (a dead
    // `<output>.tmp-tagsplice` can never be left behind), no rename-over on
    // Windows (which loses to antivirus/indexer locks), and no
    // output-sized free space (the temp splice writes a FULL second copy of
    // the file, which is exactly what dies on a nearly-full disk).
    let rebuilt = chain_with_vc_and_padding(blocks, new_payload);
    match shift_splice_in_place(path, &rebuilt, frames_start) {
        ShiftResult::Done => Ok(()),
        ShiftResult::DamagedErr(e) => Err(e),
        ShiftResult::CleanErr(shift_err) => {
            // The file is untouched — fall back to the verified temp-file
            // splice (atomic; needs output-sized free space beside the file).
            // If that also fails, the combined error explains BOTH paths.
            let (blocks2, frames_start2) = parse_metadata_chain(path)?;
            splice_vc_core(path, blocks2, frames_start2, &vendor, &comments).map_err(|splice_err| {
                format!("{splice_err} (the space-free in-place shift had also declined: {shift_err})")
            })
        }
    }
}

/// Core full-splice VC writer: rebuild the metadata chain with a VC block
/// carrying exactly `comments` (full "KEY=value" list) and `vendor`, swapping
/// the existing VC in place or inserting one after the STREAMINFO when none
/// exists, leaving a MISRC-style 4096-byte PADDING after the VC, and copying the
/// original audio frames byte-for-byte. Self-verifies (chain lengths add up,
/// first frame sync present at the declared offset, file is exactly metadata
/// plus the original frames), then renames a sibling temp file over `path`.
fn splice_vc_core(
    path: &Path,
    blocks: Vec<MetaBlock>,
    frames_start: u64,
    vendor: &str,
    comments: &[String],
) -> Result<(), String> {
    let new_payload = build_vorbis_comment(vendor, comments);
    if new_payload.len() > 0xff_ffff {
        return Err(format!("vorbis comment too large ({} bytes)", new_payload.len()));
    }

    // Rebuild the block list: the new VC + the MISRC-style padding after it
    // (shared with the in-place shift splice).
    let blocks = chain_with_vc_and_padding(blocks, new_payload);

    // Copy the audio frames verbatim from the original frame start. Record
    // the exact frame byte count first — the splice must move them 1:1 with
    // nothing inserted or dropped (the corruption class that killed lofty).
    let src_len = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    let orig_frames_len = src_len
        .checked_sub(frames_start)
        .ok_or_else(|| format!("frame start {frames_start} past end of file ({src_len})"))?;
    let mut src = File::open(path).map_err(|e| format!("open: {e}"))?;
    src.seek(SeekFrom::Start(frames_start)).map_err(|e| format!("seek: {e}"))?;

    // Sibling temp file → rename is atomic on the same filesystem.
    let tmp_path: PathBuf = {
        let mut p = path.to_path_buf();
        let mut name = p.file_name().unwrap_or_default().to_os_string();
        name.push(".tmp-tagsplice");
        p.set_file_name(name);
        p
    };
    // Every failure past this point must remove the temp — a dead
    // `<output>.tmp-tagsplice` beside the cut was the reported bug (a full
    // disk dies mid-copy here; a locked destination kills the rename below).
    let write_result: Result<(), String> = (|| {
        let mut out =
            BufWriter::new(File::create(&tmp_path).map_err(|e| format!("create tmp: {e}"))?);
        out.write_all(b"fLaC").map_err(|e| format!("write magic: {e}"))?;
        let last_idx = blocks.len() - 1;
        for (i, b) in blocks.iter().enumerate() {
            let len = b.payload.len();
            if len > 0xff_ffff {
                return Err(format!("metadata block type {} too large ({len})", b.kind));
            }
            let len24 = [(len >> 16) as u8, (len >> 8) as u8, len as u8];
            let flag = if i == last_idx { 0x80u8 } else { 0x00u8 };
            out.write_all(&[b.kind | flag, len24[0], len24[1], len24[2]])
                .map_err(|e| format!("write hdr: {e}"))?;
            out.write_all(&b.payload).map_err(|e| format!("write block: {e}"))?;
        }
        std::io::copy(&mut src, &mut out).map_err(|e| format!("copy frames: {e}"))?;
        out.flush().map_err(|e| format!("flush: {e}"))?;
        Ok(())
    })();
    if let Err(e) = write_result {
        let _ = std::fs::remove_file(&tmp_path); // never leave the dead temp
        return Err(e);
    }

    // Self-verify before replacing: the new chain must parse back to its own
    // exact metadata end, the file must be metadata + the original frames and
    // NOTHING else (lofty's writer was off by one byte here), and a frame sync
    // must sit at the new frame start.
    let expect_meta: u64 =
        4 + blocks.iter().map(|b| 4 + b.payload.len() as u64).sum::<u64>();
    let expected_len = expect_meta + orig_frames_len;
    let (_, vframes) = parse_metadata_chain(&tmp_path)?;
    let new_len = std::fs::metadata(&tmp_path).map(|m| m.len()).unwrap_or(0);
    if vframes != expect_meta {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(format!(
            "splice verification failed: frames at {vframes}, expected {expect_meta}"
        ));
    }
    if new_len != expected_len {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(format!(
            "splice verification failed: new file is {new_len} bytes, expected exactly {expected_len} (metadata {expect_meta} + original frames {orig_frames_len})"
        ));
    }
    if orig_frames_len == 0 {
        let _ = std::fs::remove_file(&tmp_path);
        return Err("splice verification failed: no audio frames after metadata".to_string());
    }
    {
        let mut vf = File::open(&tmp_path).map_err(|e| format!("reopen: {e}"))?;
        vf.seek(SeekFrom::Start(vframes)).map_err(|e| format!("seek: {e}"))?;
        let mut sync = [0u8; 2];
        vf.read_exact(&mut sync).map_err(|e| format!("read sync: {e}"))?;
        // Frame sync is 0b11111111_111110xx (0xFFF8..0xFFFB).
        if sync[0] != 0xff || (sync[1] & 0xfc) != 0xf8 {
            let _ = std::fs::remove_file(&tmp_path);
            return Err(format!(
                "splice verification failed: no frame sync at offset {vframes} (bytes {:02x} {:02x})",
                sync[0], sync[1]
            ));
        }
    }

    // Rename over the original, retrying transient Windows lock errors
    // (antivirus / the search indexer can hold the just-written destination
    // for a moment — the rename-over-existing fails with os error 5/32 until
    // they let go). On final failure the temp is removed — the untouched
    // original (with its stale tags) stays, never a dead temp file.
    if let Err(e) = rename_over_with_retry(&tmp_path, path) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(e);
    }
    Ok(())
}

/// Rebuild the metadata chain for a splice: the VORBIS_COMMENT block replaced
/// (or inserted after STREAMINFO when absent) with `new_payload`, plus a
/// MISRC-style 4096-byte PADDING right after the VC (an existing smaller
/// padding is grown) so any future tag update fits in place. Shared by the
/// in-place shift splice and the temp-file splice.
fn chain_with_vc_and_padding(mut blocks: Vec<MetaBlock>, new_payload: Vec<u8>) -> Vec<MetaBlock> {
    let vc_pos = match blocks.iter().position(|b| b.kind == 4) {
        Some(i) => {
            blocks[i].payload = new_payload;
            i
        }
        None => {
            blocks.insert(1, MetaBlock { kind: 4, payload: new_payload });
            1
        }
    };
    match blocks.get_mut(vc_pos + 1) {
        Some(b) if b.kind == 1 => {
            // Grow an existing too-small PADDING up to the MISRC size.
            if b.payload.len() < 4096 {
                b.payload.resize(4096, 0);
            }
        }
        _ => {
            blocks.insert(vc_pos + 1, MetaBlock { kind: 1, payload: vec![0u8; 4096] });
        }
    }
    blocks
}

/// Outcome of an in-place shift splice ([`shift_splice_in_place`]).
enum ShiftResult {
    /// Applied and verified.
    Done,
    /// Failed with the file untouched — the caller may fall back to the
    /// temp-file splice.
    CleanErr(String),
    /// Failed after the frame move had begun — the frames are partially
    /// displaced and only re-cutting the output recovers. NEVER fall back.
    DamagedErr(String),
}

/// Read exactly `buf.len()` bytes at `off` (each helper seeks first, short
/// reads refilled; the explicit seek also satisfies Windows' alternating
/// read/write rule on a shared handle).
fn read_exact_at(f: &mut File, off: u64, buf: &mut [u8]) -> Result<(), String> {
    f.seek(SeekFrom::Start(off)).map_err(|e| format!("seek: {e}"))?;
    let mut got = 0usize;
    while got < buf.len() {
        let n = f.read(&mut buf[got..]).map_err(|e| format!("read: {e}"))?;
        if n == 0 {
            return Err("unexpected end of file".to_string());
        }
        got += n;
    }
    Ok(())
}

/// Write exactly `buf` at `off` (each helper seeks first, short writes
/// refilled).
fn write_exact_at(f: &mut File, mut buf: &[u8], off: u64) -> Result<(), String> {
    f.seek(SeekFrom::Start(off)).map_err(|e| format!("seek: {e}"))?;
    while !buf.is_empty() {
        let n = f.write(buf).map_err(|e| format!("write: {e}"))?;
        if n == 0 {
            return Err("write wrote 0 bytes".to_string());
        }
        buf = &buf[n..];
    }
    Ok(())
}

/// Insert/replace the metadata head by shifting the audio frames WITHIN the
/// same file — no temp file, no rename, and none of the temp splice's
/// output-sized free-space requirement: the only new allocation is the
/// metadata growth itself (≈4.3 KB for the standard VC + PADDING rebuild), so
/// embedding the RF tags works on a nearly-full disk (the temp splice needs a
/// full second copy of the output next to a cut that already filled the
/// disk) and no dead `.tmp-tagsplice` can ever be left behind.
///
/// Ordering: growth extends the file FIRST (the only fallible allocation,
/// before anything moves), then moves the frames backward from the end in
/// chunks (dest = src + delta, so a moved chunk never overwrites an unread
/// one), then writes the fully-prebuilt metadata head into the vacated
/// front; shrink moves the frames forward from the start (dest < src),
/// writes the head, then truncates the vacated tail. Every size is computed
/// and the head bytes are fully built BEFORE any mutation, so the only
/// failures after the first move are raw I/O errors — reported as
/// [`ShiftResult::DamagedErr`] (this only ever runs on cut OUTPUTS; the
/// metadata editor's Save keeps the atomic temp-file splice for user files).
fn shift_splice_in_place(path: &Path, blocks: &[MetaBlock], old_frames_start: u64) -> ShiftResult {
    let damaged = |e: String| {
        ShiftResult::DamagedErr(format!(
            "in-place metadata shift failed after the frame move began — the output file may be damaged, re-cut it: {e}"
        ))
    };

    // --- Phase 1: everything fallible, without touching the file. ---
    let mut head: Vec<u8> = Vec::new();
    head.extend_from_slice(b"fLaC");
    let last_idx = blocks.len().saturating_sub(1);
    for (i, b) in blocks.iter().enumerate() {
        let len = b.payload.len();
        if len > 0xff_ffff {
            return ShiftResult::CleanErr(format!(
                "metadata block type {} too large ({len})",
                b.kind
            ));
        }
        let flag = if i == last_idx { 0x80u8 } else { 0x00u8 };
        head.extend_from_slice(&[b.kind | flag, (len >> 16) as u8, (len >> 8) as u8, len as u8]);
        head.extend_from_slice(&b.payload);
    }
    let expect_meta = head.len() as u64; // == the new frames start
    let src_len = match std::fs::metadata(path) {
        Ok(m) => m.len(),
        Err(e) => return ShiftResult::CleanErr(format!("stat: {e}")),
    };
    let orig_frames_len = src_len.saturating_sub(old_frames_start);
    if orig_frames_len == 0 {
        return ShiftResult::CleanErr("no audio frames after metadata".to_string());
    }
    let new_size = expect_meta + orig_frames_len;
    let delta_i = expect_meta as i64 - old_frames_start as i64;

    // Frame-edge snapshots (up to 16 bytes each) for a cheap strong post-move
    // check: the moved stream must still start and end with these bytes.
    const EDGE: u64 = 16;
    let n_first = EDGE.min(orig_frames_len);
    let n_last = EDGE.min(orig_frames_len);
    let (first_edge, last_edge) = {
        let mut f = match File::open(path) {
            Ok(f) => f,
            Err(e) => return ShiftResult::CleanErr(format!("open: {e}")),
        };
        let mut first_edge = vec![0u8; n_first as usize];
        let mut last_edge = vec![0u8; n_last as usize];
        if let Err(e) = read_exact_at(&mut f, old_frames_start, &mut first_edge) {
            return ShiftResult::CleanErr(e);
        }
        if let Err(e) = read_exact_at(&mut f, src_len - n_last, &mut last_edge) {
            return ShiftResult::CleanErr(e);
        }
        (first_edge, last_edge)
    };

    // --- Phase 2: the mutation. ---
    let mut f = match std::fs::OpenOptions::new().read(true).write(true).open(path) {
        Ok(f) => f,
        Err(e) => return ShiftResult::CleanErr(format!("open read+write: {e}")),
    };
    // Growth extends FIRST: one allocation up front (the only free space the
    // shift ever needs), so the chunk moves below cannot hit a space error
    // mid-move.
    if delta_i > 0 {
        if let Err(e) = f.set_len(new_size) {
            return ShiftResult::CleanErr(format!(
                "extend by {} bytes (the only free space the in-place shift needs): {e}",
                delta_i
            ));
        }
    }
    let mut buf = vec![0u8; 8 * 1024 * 1024];
    if delta_i > 0 {
        // Grow: dest = src + delta (higher) — move BACKWARD from the end.
        let delta = delta_i as u64;
        let mut pos = src_len;
        while pos > old_frames_start {
            let n = ((pos - old_frames_start).min(buf.len() as u64)) as usize;
            let src_off = pos - n as u64;
            if let Err(e) = read_exact_at(&mut f, src_off, &mut buf[..n]) {
                return damaged(format!("read frames at {src_off}: {e}"));
            }
            if let Err(e) = write_exact_at(&mut f, &buf[..n], src_off + delta) {
                return damaged(format!("write frames at {}: {e}", src_off + delta));
            }
            pos = src_off;
        }
    } else if delta_i < 0 {
        // Shrink: dest = src - |delta| (lower) — move FORWARD from the start.
        let delta = (-delta_i) as u64;
        let mut pos = old_frames_start;
        while pos < src_len {
            let n = ((src_len - pos).min(buf.len() as u64)) as usize;
            if let Err(e) = read_exact_at(&mut f, pos, &mut buf[..n]) {
                return damaged(format!("read frames at {pos}: {e}"));
            }
            if let Err(e) = write_exact_at(&mut f, &buf[..n], pos - delta) {
                return damaged(format!("write frames at {}: {e}", pos - delta));
            }
            pos += n as u64;
        }
    }
    // The prebuilt metadata head into the (vacated) front.
    if let Err(e) = write_exact_at(&mut f, &head, 0) {
        return damaged(format!("write metadata head: {e}"));
    }
    if delta_i < 0 {
        // Shrink: drop the vacated tail slack.
        if let Err(e) = f.set_len(new_size) {
            return damaged(format!("truncate: {e}"));
        }
    }
    drop(f); // release the handle before the verification re-open

    // --- Phase 3: verify (the same rigour as the temp-file splice). ---
    let (_, vframes) = match parse_metadata_chain(path) {
        Ok(v) => v,
        Err(e) => return damaged(format!("verify parse: {e}")),
    };
    if vframes != expect_meta {
        return damaged(format!("verify: frames at {vframes}, expected {expect_meta}"));
    }
    let new_len = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    if new_len != new_size {
        return damaged(format!("verify: file is {new_len} bytes, expected {new_size}"));
    }
    {
        let mut vf = match File::open(path) {
            Ok(f) => f,
            Err(e) => return damaged(format!("verify open: {e}")),
        };
        if n_first >= 2 {
            let mut sync = [0u8; 2];
            if let Err(e) = read_exact_at(&mut vf, expect_meta, &mut sync) {
                return damaged(format!("verify sync read: {e}"));
            }
            // Frame sync is 0b11111111_111110xx (0xFFF8..0xFFFB).
            if sync[0] != 0xff || (sync[1] & 0xfc) != 0xf8 {
                return damaged(format!(
                    "verify: no frame sync at offset {expect_meta} (bytes {:02x} {:02x})",
                    sync[0], sync[1]
                ));
            }
        }
        let mut got_first = vec![0u8; n_first as usize];
        if let Err(e) = read_exact_at(&mut vf, expect_meta, &mut got_first) {
            return damaged(format!("verify edge read: {e}"));
        }
        if got_first != first_edge {
            return damaged(
                "verify: the moved frame stream does not start with the original bytes"
                    .to_string(),
            );
        }
        let mut got_last = vec![0u8; n_last as usize];
        if let Err(e) = read_exact_at(&mut vf, new_size - n_last, &mut got_last) {
            return damaged(format!("verify edge read: {e}"));
        }
        if got_last != last_edge {
            return damaged(
                "verify: the moved frame stream does not end with the original bytes".to_string(),
            );
        }
    }
    ShiftResult::Done
}

/// Rename `from` over `to`, retrying transient Windows lock failures.
/// `std::fs::rename` on Windows is MoveFileExW(MOVEFILE_REPLACE_EXISTING),
/// which fails with "Access is denied" (os error 5) / "Sharing violation"
/// (os error 32) while antivirus or the search indexer still holds the
/// just-written destination — locks that clear on their own within
/// milliseconds-to-seconds. Retry those two for ~10 s; every other error
/// (bad path, permissions, ...) fails immediately — waiting cannot help.
fn rename_over_with_retry(from: &Path, to: &Path) -> Result<(), String> {
    let mut last = String::new();
    for _ in 0..40u32 {
        match std::fs::rename(from, to) {
            Ok(()) => return Ok(()),
            Err(e) => {
                let transient = matches!(e.raw_os_error(), Some(5) | Some(32));
                last = e.to_string();
                if !transient {
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(250));
            }
        }
    }
    Err(format!("rename {} -> {}: {last}", from.display(), to.display()))
}

/// Read every Vorbis comment in the FLAC at `path` as `(key, value)` pairs, in
/// file order. The vendor string is returned too (it is preserved on write by
/// [`replace_all_comments`]). Returns an empty list (and empty vendor) when the
/// file has no VORBIS_COMMENT block. Errors on a malformed container or I/O
/// failure — never on a simply tag-less file.
pub fn read_all_comments(path: &Path) -> Result<(String, Vec<(String, String)>), String> {
    let (blocks, _frames) = parse_metadata_chain(path)?;
    let (vendor, comments) = match blocks.iter().find(|b| b.kind == 4) {
        Some(b) => parse_vorbis_comment(&b.payload)?,
        None => return Ok((String::new(), Vec::new())),
    };
    // Split each "KEY=value" on the FIRST '=' only — values may contain '='.
    let pairs = comments
        .iter()
        .map(|c| match c.find('=') {
            Some(i) => (c[..i].to_string(), c[i + 1..].to_string()),
            None => (c.clone(), String::new()),
        })
        .collect();
    Ok((vendor, pairs))
}

/// Replace the entire VORBIS_COMMENT block on the FLAC at `path` with exactly
/// `comments` (a full `(key, value)` list, in order). The existing vendor
/// string is preserved (or a `"FLAC-Chop"` default is used when the file has no
/// VC block). Keys must be non-empty and match `[A-Za-z0-9_]+`; they are
/// uppercased on write (Vorbis field names are case-insensitive). Values are
/// arbitrary UTF-8 (may be empty, may contain `=`).
///
/// Writes in place (adjacent PADDING / vendor trim) with a verified temp-file
/// splice fallback that leaves MISRC-style 4096-byte padding behind, exactly
/// like the cut-tag rewrite path.
pub fn replace_all_comments(path: &Path, comments: &[(String, String)]) -> Result<(), String> {
    // Validate + build the "KEY=value" comment strings. Keys uppercased per
    // Vorbis convention; values left untouched (may hold '=' / be empty).
    let mut comment_strs: Vec<String> = Vec::with_capacity(comments.len());
    for (i, (k, v)) in comments.iter().enumerate() {
        if k.is_empty() {
            return Err(format!("comment {i}: field name is empty"));
        }
        if !k.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
            return Err(format!(
                "comment {i}: field name {:?} has invalid characters (allowed: A-Z a-z 0-9 _)",
                k
            ));
        }
        comment_strs.push(format!("{}={}", k.to_ascii_uppercase(), v));
    }

    let (blocks, frames_start) = parse_metadata_chain(path)?;
    // Preserve the existing vendor string (default when no VC block exists).
    let vendor = match blocks.iter().find(|b| b.kind == 4) {
        Some(b) => parse_vorbis_comment(&b.payload).map(|(v, _)| v)?,
        None => "FLAC-Chop".to_string(),
    };

    let src_len = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    let orig_frames_len = src_len
        .checked_sub(frames_start)
        .ok_or_else(|| format!("frame start {frames_start} past end of file ({src_len})"))?;
    if orig_frames_len == 0 {
        return Err("no audio frames after metadata".to_string());
    }

    // Try the in-place update first (only when a VC block already exists — the
    // in-place path cannot insert a new block). On Ok(false) (cannot fit) or
    // when there is no VC, fall back to the full splice, which inserts a VC
    // and leaves MISRC-style padding so the next edit fits in place.
    let applied = match blocks.iter().position(|b| b.kind == 4) {
        Some(vc_idx) => write_vc_in_place_core(
            path, &blocks, frames_start, src_len, vc_idx, &vendor, &comment_strs,
        )?,
        None => false,
    };
    if applied {
        Ok(())
    } else {
        splice_vc_core(path, blocks, frames_start, &vendor, &comment_strs)
    }
}

/// Read `(header_sample_rate, total_samples)` from a FLAC's STREAMINFO via
/// claxon. `total_samples` is the STREAMINFO count (at the header rate); 0
/// means unknown (unfinalized/piped captures), in which case the caller should
/// treat the tags as best-effort.
fn read_streaminfo(path: &Path) -> Result<(u32, u64), String> {
    let file = File::open(path).map_err(|e| format!("open: {e}"))?;
    let opts = FlacReaderOptions {
        metadata_only: true,
        read_vorbis_comment: false,
    };
    let reader = FlacReader::new_ext(file, opts).map_err(|e| format!("claxon: {e}"))?;
    let si = reader.streaminfo();
    let header_rate = si.sample_rate;
    // claxon's StreamInfo exposes `samples: Option<u64>` (None/0 = unknown for
    // unfinalized/piped captures).
    let total = si.samples.unwrap_or(0);
    Ok((header_rate, total))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// Minimal FLAC: 10000 Hz, 1 ch, 8-bit STREAMINFO, the given Vorbis
    /// comments, a PADDING block, and one fake frame (valid sync prefix — the
    /// splice checks the sync, not full decodability).
    fn write_minimal_flac(name: &str, vc_comments: &[&str]) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(name);
        let mut f = File::create(&p).unwrap();
        f.write_all(b"fLaC").unwrap();

        // STREAMINFO: 10000 Hz, 1 ch, 8 bps, total 4096 (one block).
        let packed: u64 = (10000u64 << 44) | (0u64 << 41) | (7u64 << 36) | 4096;
        let mut si = Vec::new();
        si.extend_from_slice(&4096u16.to_be_bytes());
        si.extend_from_slice(&4096u16.to_be_bytes());
        si.extend_from_slice(&[0xFF, 0xFF, 0xFF]);
        si.extend_from_slice(&[0xFF, 0xFF, 0xFF]);
        si.extend_from_slice(&packed.to_be_bytes());
        si.extend_from_slice(&[0u8; 16]); // md5 placeholder
        f.write_all(&[0x00, 0x00, 0x00, 34]).unwrap(); // not last, len 34
        f.write_all(&si).unwrap();

        // VORBIS_COMMENT with the requested comments.
        let vc = build_vorbis_comment(
            "test",
            &vc_comments.iter().map(|s| s.to_string()).collect::<Vec<_>>(),
        );
        f.write_all(&[0x04, (vc.len() >> 16) as u8, (vc.len() >> 8) as u8, vc.len() as u8])
            .unwrap();
        f.write_all(&vc).unwrap();

        // PADDING, last block.
        f.write_all(&[0x81, 0x00, 0x00, 0x04]).unwrap();
        f.write_all(&[0u8; 4]).unwrap();

        // One constant subframe-style frame: 0xFFF8 sync + junk payload.
        f.write_all(&[0xFF, 0xF8, 0xCC, 0x02, 0x00, 0x0A, 0x1D, 0x4A]).unwrap();
        p
    }

    fn read_comments(p: &Path) -> Vec<String> {
        let (blocks, _) = parse_metadata_chain(p).unwrap();
        let vc = blocks.iter().find(|b| b.kind == 4).unwrap();
        parse_vorbis_comment(&vc.payload).unwrap().1
    }

    #[test]
    fn splice_replaces_owned_tags_and_keeps_others() {
        let p = write_minimal_flac(
            "fc_test_tag_basic.flac",
            &["PROJECT=demo", "RF_TOTAL_SAMPLES=999", "OPERATOR=harry"],
        );
        splice_vorbis_comment(
            &p,
            &[
                ("RF_TOTAL_SAMPLES", "12345".to_string()),
                ("RF_SAMPLE_RATE", "10000000".to_string()),
            ],
            &[],
        )
        .unwrap();
        let c = read_comments(&p);
        assert!(c.contains(&"PROJECT=demo".to_string()));
        assert!(c.contains(&"OPERATOR=harry".to_string()));
        assert!(c.contains(&"RF_TOTAL_SAMPLES=12345".to_string()));
        assert!(c.contains(&"RF_SAMPLE_RATE=10000000".to_string()));
        assert!(!c.contains(&"RF_TOTAL_SAMPLES=999".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn splice_preserves_frames_byte_for_byte() {
        let p = write_minimal_flac("fc_test_tag_frames.flac", &["A=1"]);
        let mut frames_before = Vec::new();
        {
            let (_, fs) = parse_metadata_chain(&p).unwrap();
            let mut f = File::open(&p).unwrap();
            f.seek(SeekFrom::Start(fs)).unwrap();
            f.read_to_end(&mut frames_before).unwrap();
        }
        splice_vorbis_comment(&p, &[("LENGTH", "5000".to_string())], &[]).unwrap();
        let (_, fs) = parse_metadata_chain(&p).unwrap();
        let mut f = File::open(&p).unwrap();
        f.seek(SeekFrom::Start(fs)).unwrap();
        let mut frames_after = Vec::new();
        f.read_to_end(&mut frames_after).unwrap();
        assert_eq!(frames_before, frames_after);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn splice_inserts_vc_when_missing() {
        let p = std::env::temp_dir().join("fc_test_tag_novc.flac");
        {
            let mut f = File::create(&p).unwrap();
            f.write_all(b"fLaC").unwrap();
            let mut si = [0u8; 34];
            si[10..18].copy_from_slice(&((10000u64 << 44) | 4096).to_be_bytes());
            f.write_all(&[0x80, 0x00, 0x00, 34]).unwrap(); // last block
            f.write_all(&si).unwrap();
            f.write_all(&[0xFF, 0xF8, 0x00, 0x01]).unwrap();
        }
        splice_vorbis_comment(&p, &[("RF_TOTAL_SAMPLES", "42".to_string())], &[]).unwrap();
        let (blocks, _) = parse_metadata_chain(&p).unwrap();
        // STREAMINFO + inserted VC + the MISRC-style padding the splice leaves
        // behind so future updates fit in place.
        assert_eq!(blocks.len(), 3);
        assert_eq!(blocks[0].kind, 0);
        assert_eq!(blocks[1].kind, 4);
        assert_eq!(blocks[2].kind, 1);
        assert!(read_comments(&p).contains(&"RF_TOTAL_SAMPLES=42".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    /// SoX cut layout: [STREAMINFO][VORBIS_COMMENT (last)][frames] — no
    /// PADDING block (SoX does not write one).
    fn write_sox_style_flac(name: &str, vc_comments: &[&str]) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(name);
        let mut f = File::create(&p).unwrap();
        f.write_all(b"fLaC").unwrap();
        let packed: u64 = (10000u64 << 44) | (0u64 << 41) | (7u64 << 36) | 4096;
        let mut si = Vec::new();
        si.extend_from_slice(&4096u16.to_be_bytes());
        si.extend_from_slice(&4096u16.to_be_bytes());
        si.extend_from_slice(&[0xFF, 0xFF, 0xFF]);
        si.extend_from_slice(&[0xFF, 0xFF, 0xFF]);
        si.extend_from_slice(&packed.to_be_bytes());
        si.extend_from_slice(&[0u8; 16]);
        f.write_all(&[0x00, 0x00, 0x00, 34]).unwrap();
        f.write_all(&si).unwrap();
        let vc = build_vorbis_comment(
            "reference libFLAC 1.3.3 20190804",
            &vc_comments.iter().map(|s| s.to_string()).collect::<Vec<_>>(),
        );
        f.write_all(&[0x84, (vc.len() >> 16) as u8, (vc.len() >> 8) as u8, vc.len() as u8])
            .unwrap(); // last block
        f.write_all(&vc).unwrap();
        f.write_all(&[0xFF, 0xF8, 0xCC, 0x02, 0x00, 0x0A, 0x1D, 0x4A]).unwrap();
        p
    }

    fn read_frames(p: &Path) -> Vec<u8> {
        let (_, fs) = parse_metadata_chain(p).unwrap();
        let mut f = File::open(p).unwrap();
        f.seek(SeekFrom::Start(fs)).unwrap();
        let mut v = Vec::new();
        f.read_to_end(&mut v).unwrap();
        v
    }

    #[test]
    fn inplace_updates_sox_style_cut_without_temp_file() {
        // The real-cut shape: 5 stale tags, new values a few bytes larger.
        // Growth is absorbed by trimming the vendor string — file size and
        // frame bytes must be untouched, and no temp file may appear.
        let p = write_sox_style_flac(
            "fc_test_tag_inplace_sox.flac",
            &[
                "DURATION_SECONDS=216.557080",
                "LENGTH=216557",
                "RF_TOTAL_SAMPLES=2165570800",
                "RF_SAMPLE_RATE=10000000",
                "RF_SAMPLE_RATE_KHZ=10000",
            ],
        );
        let size_before = std::fs::metadata(&p).unwrap().len();
        let frames_before = read_frames(&p);

        let applied = update_vorbis_comment_in_place(
            &p,
            &[
                ("RF_SAMPLE_RATE_KHZ", "10000".to_string()),
                ("RF_TOTAL_SAMPLES", "50000000000".to_string()),
                ("RF_SAMPLE_RATE", "10000000".to_string()),
                ("DURATION_SECONDS", "5000.000000".to_string()),
                ("LENGTH", "5000000".to_string()),
            ],
            &[],
        )
        .unwrap();
        assert!(applied, "sox-style cut must update in place");
        assert_eq!(
            std::fs::metadata(&p).unwrap().len(),
            size_before,
            "in-place update must not change the file size"
        );
        assert_eq!(read_frames(&p), frames_before, "frames must be untouched");
        let c = read_comments(&p);
        assert!(c.contains(&"RF_TOTAL_SAMPLES=50000000000".to_string()));
        assert!(c.contains(&"LENGTH=5000000".to_string()));
        assert!(!c.contains(&"RF_TOTAL_SAMPLES=2165570800".to_string()));
        // No temp file left behind.
        let tmp = std::env::temp_dir().join("fc_test_tag_inplace_sox.flac.tmp-tagsplice");
        assert!(!tmp.exists(), "in-place update must not create a temp file");
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn inplace_grows_into_adjacent_padding() {
        // MISRC writer layout: [SI][VC][PADDING][frames] — growth consumes the
        // padding; file size and frames stay untouched.
        let p = write_minimal_flac(
            "fc_test_tag_inplace_pad.flac",
            &["RF_TOTAL_SAMPLES=999", "PROJECT=demo"],
        );
        let size_before = std::fs::metadata(&p).unwrap().len();
        let frames_before = read_frames(&p);
        let applied = update_vorbis_comment_in_place(
            &p,
            &[("RF_TOTAL_SAMPLES", "123456789".to_string())],
            &[],
        )
        .unwrap();
        assert!(applied, "growth within padding must update in place");
        assert_eq!(std::fs::metadata(&p).unwrap().len(), size_before);
        assert_eq!(read_frames(&p), frames_before);
        let c = read_comments(&p);
        assert!(c.contains(&"RF_TOTAL_SAMPLES=123456789".to_string()));
        assert!(c.contains(&"PROJECT=demo".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn inplace_shrink_inserts_padding_block() {
        // New values much shorter than the old ones → the VC shrinks; the
        // slack is soaked up by a freshly inserted PADDING block so the file
        // size and frames stay identical.
        let p = write_sox_style_flac(
            "fc_test_tag_inplace_shrink.flac",
            &[
                "DURATION_SECONDS=216.557080",
                "LENGTH=216557",
                "RF_TOTAL_SAMPLES=2165570800",
                "RF_SAMPLE_RATE=10000000",
                "RF_SAMPLE_RATE_KHZ=10000",
            ],
        );
        let size_before = std::fs::metadata(&p).unwrap().len();
        let frames_before = read_frames(&p);
        let applied = update_vorbis_comment_in_place(
            &p,
            &[
                ("RF_SAMPLE_RATE_KHZ", "1".to_string()),
                ("RF_TOTAL_SAMPLES", "1".to_string()),
                ("RF_SAMPLE_RATE", "1".to_string()),
                ("DURATION_SECONDS", "0.000000".to_string()),
                ("LENGTH", "1".to_string()),
            ],
            &[],
        )
        .unwrap();
        assert!(applied, "shrink with ≥4 slack must update in place");
        assert_eq!(std::fs::metadata(&p).unwrap().len(), size_before);
        assert_eq!(read_frames(&p), frames_before);
        let (blocks, _) = parse_metadata_chain(&p).unwrap();
        assert_eq!(blocks.len(), 3); // SI + VC + inserted PADDING
        assert_eq!(blocks[2].kind, 1);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn splice_fallback_leaves_misrc_padding() {
        // Growth far past vendor + padding: the in-place update must decline
        // (Ok(false)) and the full splice must take over — leaving a
        // MISRC-style 4096-byte PADDING after the VC so the next update fits
        // in place.
        let p = write_sox_style_flac(
            "fc_test_tag_fallback.flac",
            &[
                "DURATION_SECONDS=216.557080",
                "LENGTH=216557",
                "RF_TOTAL_SAMPLES=2165570800",
                "RF_SAMPLE_RATE=10000000",
                "RF_SAMPLE_RATE_KHZ=10000",
            ],
        );
        let huge = "9".repeat(600);
        let applied =
            update_vorbis_comment_in_place(&p, &[("RF_TOTAL_SAMPLES", huge.clone())], &[]).unwrap();
        assert!(!applied, "growth past vendor must fall back to the splice");
        splice_vorbis_comment(&p, &[("RF_TOTAL_SAMPLES", huge.clone())], &[]).unwrap();
        let (blocks, _) = parse_metadata_chain(&p).unwrap();
        assert_eq!(blocks[2].kind, 1, "MISRC-style padding must follow the VC");
        assert_eq!(blocks[2].payload.len(), 4096);
        assert!(read_comments(&p).contains(&format!("RF_TOTAL_SAMPLES={huge}")));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn splice_refuses_non_flac() {
        let p = std::env::temp_dir().join("fc_test_tag_notflac.bin");
        std::fs::write(&p, b"not a flac file at all").unwrap();
        assert!(splice_vorbis_comment(&p, &[("A", "1".to_string())], &[]).is_err());
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn owned_tags_are_unique_and_sorted() {
        // Sanity: the list we rewrite must be exactly the numeric RF set, no
        // ingest-metadata keys (those are passed through, never owned).
        let mut s = OWNED_TAGS.to_vec();
        s.sort();
        let mut d: Vec<&str> = s.clone();
        d.dedup();
        assert_eq!(s.len(), d.len(), "OWNED_TAGS has duplicates");
        assert!(s.contains(&"RF_TOTAL_SAMPLES"));
        assert!(s.contains(&"RF_SAMPLE_RATE"));
        assert!(s.contains(&"RF_SAMPLE_RATE_KHZ"));
        assert!(s.contains(&"DURATION_SECONDS"));
        assert!(s.contains(&"LENGTH"));
    }

    #[test]
    fn read_streaminfo_non_flac_errors() {
        // /dev/null is not FLAC — must return an error, not panic.
        let r = read_streaminfo(Path::new("/dev/null"));
        assert!(r.is_err());
    }

    fn read_vendor(p: &Path) -> String {
        let (blocks, _) = parse_metadata_chain(p).unwrap();
        let vc = blocks.iter().find(|b| b.kind == 4).unwrap();
        parse_vorbis_comment(&vc.payload).unwrap().0
    }

    // --- read_all_comments / replace_all_comments (GUI Metadata Editor path) --

    #[test]
    fn read_all_comments_returns_pairs() {
        let p = write_minimal_flac(
            "fc_test_read_all.flac",
            &["PROJECT=demo", "OPERATOR=harry", "RF_TOTAL_SAMPLES=42"],
        );
        let (vendor, pairs) = read_all_comments(&p).unwrap();
        assert_eq!(vendor, "test");
        assert_eq!(pairs.len(), 3);
        assert_eq!(pairs[0], ("PROJECT".to_string(), "demo".to_string()));
        assert_eq!(pairs[1], ("OPERATOR".to_string(), "harry".to_string()));
        assert_eq!(pairs[2], ("RF_TOTAL_SAMPLES".to_string(), "42".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn read_all_comments_value_with_equals_is_kept_intact() {
        // Values may contain '=' — only the FIRST '=' splits key from value.
        let p = write_minimal_flac("fc_test_read_eq.flac", &["NOTES=a=b=c"]);
        let (_, pairs) = read_all_comments(&p).unwrap();
        assert_eq!(pairs, vec![("NOTES".to_string(), "a=b=c".to_string())]);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn read_all_comments_no_vc_is_empty_not_error() {
        // STREAMINFO-only FLAC (no VC block) → empty list, not an error.
        let p = std::env::temp_dir().join("fc_test_read_novc.flac");
        {
            let mut f = File::create(&p).unwrap();
            f.write_all(b"fLaC").unwrap();
            let mut si = [0u8; 34];
            si[10..18].copy_from_slice(&((10000u64 << 44) | 4096).to_be_bytes());
            f.write_all(&[0x80, 0x00, 0x00, 34]).unwrap();
            f.write_all(&si).unwrap();
            f.write_all(&[0xFF, 0xF8, 0x00, 0x01]).unwrap();
        }
        let (vendor, pairs) = read_all_comments(&p).unwrap();
        assert!(vendor.is_empty());
        assert!(pairs.is_empty());
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn read_all_comments_refuses_non_flac() {
        let p = std::env::temp_dir().join("fc_test_read_notflac.bin");
        std::fs::write(&p, b"not flac").unwrap();
        assert!(read_all_comments(&p).is_err());
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn replace_all_comments_rewrites_full_set_inplace() {
        // Adjacent 4-byte PADDING absorbs the small shrink; file size and
        // frames stay identical. Covers removal (OPERATOR dropped), addition
        // (NOTES added), and lower-case key uppercasing ("project"→PROJECT).
        let p = write_minimal_flac(
            "fc_test_replace_inplace.flac",
            &["PROJECT=demo", "RF_TOTAL_SAMPLES=999", "OPERATOR=harry"],
        );
        let size_before = std::fs::metadata(&p).unwrap().len();
        let frames_before = read_frames(&p);
        replace_all_comments(
            &p,
            &[
                ("project".to_string(), "demo".to_string()),
                ("RF_TOTAL_SAMPLES".to_string(), "999".to_string()),
                ("NOTES".to_string(), "hi".to_string()),
            ],
        )
        .unwrap();
        assert_eq!(
            std::fs::metadata(&p).unwrap().len(),
            size_before,
            "in-place replace must keep the file size"
        );
        assert_eq!(read_frames(&p), frames_before, "frames must be untouched");
        let c = read_comments(&p);
        assert!(
            c.contains(&"PROJECT=demo".to_string()),
            "lower-case key must be upper-cased: {c:?}"
        );
        assert!(c.contains(&"RF_TOTAL_SAMPLES=999".to_string()));
        assert!(c.contains(&"NOTES=hi".to_string()));
        assert!(!c.contains(&"OPERATOR=harry".to_string()), "removed key must be gone");
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn replace_all_comments_validates_keys() {
        let p = write_minimal_flac("fc_test_replace_badkey.flac", &["A=1"]);
        let err = replace_all_comments(&p, &[("".to_string(), "x".to_string())]).unwrap_err();
        assert!(err.contains("empty"), "{err}");
        let err2 =
            replace_all_comments(&p, &[("BAD KEY".to_string(), "x".to_string())]).unwrap_err();
        assert!(err2.contains("invalid"), "{err2}");
        // File untouched on validation failure (the write never starts).
        assert!(read_comments(&p).contains(&"A=1".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn replace_all_comments_preserves_vendor() {
        let p = write_sox_style_flac("fc_test_replace_vendor.flac", &["A=1"]);
        let vendor_before = read_vendor(&p);
        replace_all_comments(&p, &[("B".to_string(), "2".to_string())]).unwrap();
        assert_eq!(read_vendor(&p), vendor_before, "vendor string must be preserved");
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn replace_all_comments_inserts_vc_when_missing() {
        // No VC block → the in-place path declines and the splice inserts one,
        // leaving MISRC-style padding behind.
        let p = std::env::temp_dir().join("fc_test_replace_insert.flac");
        {
            let mut f = File::create(&p).unwrap();
            f.write_all(b"fLaC").unwrap();
            let mut si = [0u8; 34];
            si[10..18].copy_from_slice(&((10000u64 << 44) | 4096).to_be_bytes());
            f.write_all(&[0x80, 0x00, 0x00, 34]).unwrap();
            f.write_all(&si).unwrap();
            f.write_all(&[0xFF, 0xF8, 0x00, 0x01]).unwrap();
        }
        replace_all_comments(&p, &[("RF_TOTAL_SAMPLES".to_string(), "42".to_string())]).unwrap();
        let (blocks, _) = parse_metadata_chain(&p).unwrap();
        assert_eq!(blocks[0].kind, 0); // SI
        assert_eq!(blocks[1].kind, 4); // inserted VC
        assert_eq!(blocks[2].kind, 1); // MISRC padding
        assert!(read_comments(&p).contains(&"RF_TOTAL_SAMPLES=42".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn replace_all_comments_splices_when_growth_too_large() {
        // Growth far past the vendor: in-place declines, the full splice takes
        // over (file size grows by the new padding), frames copied 1:1.
        let p = write_sox_style_flac("fc_test_replace_splice.flac", &["A=1"]);
        let frames_before = read_frames(&p);
        let big = "x".repeat(500);
        replace_all_comments(&p, &[("NOTES".to_string(), big.clone())]).unwrap();
        let (blocks, _) = parse_metadata_chain(&p).unwrap();
        assert_eq!(blocks[blocks.len() - 2].kind, 4, "VC must sit before the padding");
        assert_eq!(blocks.last().unwrap().kind, 1, "splice must leave padding last");
        assert!(read_comments(&p).contains(&format!("NOTES={big}")));
        assert_eq!(read_frames(&p), frames_before, "frames copied byte-for-byte");
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn shift_splice_grows_in_place_without_temp() {
        // The SoX cut layout (no padding): the in-place shift must insert the
        // grown VC + 4096 padding by moving the frames within the SAME file —
        // no temp file, the file grows by exactly the metadata delta, the
        // frames stay byte-identical, and the chain re-parses cleanly.
        let p = write_sox_style_flac(
            "fc_test_tag_shift_grow.flac",
            &["DURATION_SECONDS=216.557080", "RF_TOTAL_SAMPLES=2165570800"],
        );
        let frames_before = read_frames(&p);
        let size_before = std::fs::metadata(&p).unwrap().len();
        let (blocks, frames_start) = parse_metadata_chain(&p).unwrap();
        let (vendor, mut comments) = match blocks.iter().find(|b| b.kind == 4) {
            Some(b) => parse_vorbis_comment(&b.payload).unwrap(),
            None => ("FLAC-Chop".to_string(), Vec::new()),
        };
        comments.push("RF_SAMPLE_RATE=40000000".to_string());
        let payload = build_vorbis_comment(&vendor, &comments);
        let rebuilt = chain_with_vc_and_padding(blocks, payload);
        let expect_meta: u64 = 4 + rebuilt.iter().map(|b| 4 + b.payload.len() as u64).sum::<u64>();
        assert!(expect_meta > frames_start, "this fixture must grow the head");
        match shift_splice_in_place(&p, &rebuilt, frames_start) {
            ShiftResult::Done => {}
            ShiftResult::CleanErr(e) => panic!("unexpected CleanErr: {e}"),
            ShiftResult::DamagedErr(e) => panic!("unexpected DamagedErr: {e}"),
        }
        let (blocks2, frames_at) = parse_metadata_chain(&p).unwrap();
        assert_eq!(frames_at, expect_meta);
        assert_eq!(
            std::fs::metadata(&p).unwrap().len(),
            size_before + (expect_meta - frames_start),
            "growth must extend the file by exactly the metadata delta"
        );
        assert_eq!(blocks2[blocks2.len() - 2].kind, 4, "VC must sit before the padding");
        assert_eq!(blocks2.last().unwrap().kind, 1);
        assert_eq!(blocks2.last().unwrap().payload.len(), 4096);
        assert!(read_comments(&p).contains(&"RF_SAMPLE_RATE=40000000".to_string()));
        assert_eq!(read_frames(&p), frames_before, "frames must be moved 1:1");
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn shift_splice_shrinks_in_place() {
        // Old head BIG (a fat VC + a fat PADDING), new head small (a tiny VC,
        // padding kept at its >4096 size): negative delta — the frames move
        // FORWARD, the head shrinks, the file is truncated to the exact new
        // size, no temp file.
        let dir = std::env::temp_dir();
        let p = dir.join("fc_test_tag_shift_shrink.flac");
        {
            let mut f = File::create(&p).unwrap();
            f.write_all(b"fLaC").unwrap();
            let mut si = [0u8; 34];
            si[10..18].copy_from_slice(&((10000u64 << 44) | 4096).to_be_bytes());
            f.write_all(&[0x00, 0x00, 0x00, 34]).unwrap();
            f.write_all(&si).unwrap();
            // A fat VC (one 600-byte comment).
            let vc = build_vorbis_comment("test", &[format!("NOTES={}", "x".repeat(600))]);
            f.write_all(&[0x04, (vc.len() >> 16) as u8, (vc.len() >> 8) as u8, vc.len() as u8])
                .unwrap();
            f.write_all(&vc).unwrap();
            // A fat 4596-byte PADDING, last block (0x11F4).
            f.write_all(&[0x81, 0x00, 0x11, 0xF4]).unwrap(); // 4596
            f.write_all(&vec![0u8; 4596]).unwrap();
            // One constant subframe-style frame.
            f.write_all(&[0xFF, 0xF8, 0xCC, 0x02, 0x00, 0x0A, 0x1D, 0x4A]).unwrap();
        }
        let frames_before = read_frames(&p);
        let size_before = std::fs::metadata(&p).unwrap().len();
        let (blocks, frames_start) = parse_metadata_chain(&p).unwrap();
        let payload = build_vorbis_comment("test", &["A=1".to_string()]);
        let rebuilt = chain_with_vc_and_padding(blocks, payload);
        let expect_meta: u64 = 4 + rebuilt.iter().map(|b| 4 + b.payload.len() as u64).sum::<u64>();
        assert!(expect_meta < frames_start, "this fixture must shrink the head");
        match shift_splice_in_place(&p, &rebuilt, frames_start) {
            ShiftResult::Done => {}
            ShiftResult::CleanErr(e) => panic!("unexpected CleanErr: {e}"),
            ShiftResult::DamagedErr(e) => panic!("unexpected DamagedErr: {e}"),
        }
        let (_, frames_at) = parse_metadata_chain(&p).unwrap();
        assert_eq!(frames_at, expect_meta);
        assert_eq!(
            std::fs::metadata(&p).unwrap().len(),
            size_before - (frames_start - expect_meta),
            "shrink must truncate the file to the exact new size"
        );
        assert_eq!(read_frames(&p), frames_before, "frames must be moved 1:1");
        assert!(read_comments(&p).contains(&"A=1".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn rewrite_cut_tags_merges_extras_and_owned_wins() {
        // Caller-authored extras (the pre-conversion editor rows): each
        // replaces the same-key comment and survives the rewrite; the owned
        // numeric RF tags are applied AFTER and win over an extra that tries
        // to set them (their values come from the output's STREAMINFO — the
        // fixture: 10000 Hz header, 4096 samples).
        let p = write_sox_style_flac(
            "fc_test_tag_extras.flac",
            &["DURATION_SECONDS=216.557080", "RF_TOTAL_SAMPLES=2165570800"],
        );
        let extras = vec![
            "DATE_RECORDED=2022-12-11 00:00:34".to_string(),
            "PROJECT=demo".to_string(),
            "RF_TOTAL_SAMPLES=999999".to_string(), // owned: must be overridden
        ];
        rewrite_cut_tags(&p, true, &extras).unwrap();
        let c = read_comments(&p);
        assert!(c.contains(&"DATE_RECORDED=2022-12-11 00:00:34".to_string()));
        assert!(c.contains(&"PROJECT=demo".to_string()));
        assert!(c.contains(&"RF_TOTAL_SAMPLES=4096".to_string()));
        assert!(!c.contains(&"RF_TOTAL_SAMPLES=999999".to_string()));
        assert!(c.contains(&"RF_SAMPLE_RATE=10000000".to_string()));
        assert!(c.contains(&"RF_SAMPLE_RATE_KHZ=10000".to_string()));
        // 4096 samples at the 10 MSPS real rate = 0.4096 ms.
        assert!(c.contains(&"DURATION_SECONDS=0.000410".to_string()));
        assert!(!c.contains(&"DURATION_SECONDS=216.557080".to_string()));
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn replace_all_comments_round_trips_with_read() {
        // read → replace(read-back) → read again must be stable.
        let p = write_minimal_flac(
            "fc_test_replace_roundtrip.flac",
            &["PROJECT=demo", "OPERATOR=harry"],
        );
        let (_, pairs) = read_all_comments(&p).unwrap();
        replace_all_comments(&p, &pairs).unwrap();
        let (_, pairs2) = read_all_comments(&p).unwrap();
        assert_eq!(pairs, pairs2);
        let _ = std::fs::remove_file(&p);
    }
}
