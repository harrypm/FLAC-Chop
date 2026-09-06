//! In-place FLAC STREAMINFO `total_samples` repair.
//!
//! Some capture writers never seek back to finalize the STREAMINFO header, so
//! the declared total is wrong even though every audio frame is intact:
//!  - the MISRC HiFi writer stores the real count / 1000 (a 2,165,570,800-
//!    sample capture declares 2,165,571), so SoX believes the stream ends at
//!    0.2 s and refuses every cut past that ("Start position is after
//!    expected end of audio");
//!  - 36-bit wraps (declared + k·2³⁶) are handled by the probe, but do not
//!    fit back into the field, so they are NOT repairable here.
//!
//! When the probe has an authoritative true count (the MISRC/DdD
//! `RF_TOTAL_SAMPLES` Vorbis tag) that differs from the declared value and
//! still fits the 36-bit field, patching the field in place makes every
//! downstream tool (SoX, ffprobe, vhs-decode) read the file correctly. The
//! patch touches nothing else: the MD5 in STREAMINFO covers the *uncompressed*
//! audio, which is unchanged.

use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

/// Bit layout of the last 8 bytes of the STREAMINFO body (big-endian):
/// sample_rate(20) | channels(3) | bits_per_sample(5) | total_samples(36).
const PACKED_MASK_TOTAL: u64 = (1u64 << 36) - 1;
/// Byte offset of that word: "fLaC"(4) + block header(4) + min/max block(4)
/// + min/max frame(6) = 18.
const PACKED_OFFSET: u64 = 18;

/// Try to repair a wrong STREAMINFO `total_samples` in place.
///
/// * `path` — the FLAC file to patch.
/// * `declared` — the total the probe read from STREAMINFO (used as a safety
///   cross-check: the on-disk bytes must still decode to this exact value).
/// * `real` — the authoritative true count (probe: vorbis tag / wrap-
///   corrected value). Must fit the 36-bit field.
///
/// Returns `Ok(true)` when a patch was applied, `Ok(false)` when nothing
/// needed changing (already correct, or the caller's precondition failed),
/// and `Err` when the file looks repairable but the patch could not be
/// verified — the caller surfaces that as a warning and cuts anyway.
pub fn repair_declared_total(path: &Path, declared: u64, real: u64) -> Result<bool, String> {
    if real == 0 || real == declared {
        return Ok(false);
    }
    // The STREAMINFO field physically holds 36 bits (vhs-decode
    // FLAC_TOTAL_SAMPLES_FIELD_MOD). Anything larger cannot be stored.
    if real >= (1u64 << 36) {
        return Ok(false);
    }

    let mut f = File::open(path).map_err(|e| format!("open failed: {e}"))?;

    // Verify the container layout before touching anything: "fLaC" magic,
    // then a STREAMINFO block (type 0) as the first metadata block.
    let mut magic = [0u8; 4];
    f.read_exact(&mut magic).map_err(|e| e.to_string())?;
    if &magic != b"fLaC" {
        return Err("not a FLAC file".to_string());
    }
    let mut hdr = [0u8; 4];
    f.read_exact(&mut hdr).map_err(|e| e.to_string())?;
    if hdr[0] & 0x7f != 0 {
        return Err("first metadata block is not STREAMINFO".to_string());
    }
    let block_len = u32::from(hdr[1]) << 16 | u32::from(hdr[2]) << 8 | u32::from(hdr[3]);
    if block_len != 34 {
        return Err(format!("unexpected STREAMINFO length {block_len}"));
    }

    // STREAMINFO body: min_block(2) max_block(2) min_frame(3) max_frame(3)
    // then 8 bytes packing rate(20) | channels(3) | bps(5) | total(36).
    let mut packed_bytes = [0u8; 8];
    f.seek(SeekFrom::Start(PACKED_OFFSET)).map_err(|e| e.to_string())?;
    f.read_exact(&mut packed_bytes).map_err(|e| e.to_string())?;
    let packed = u64::from_be_bytes(packed_bytes);
    let on_disk_total = packed & PACKED_MASK_TOTAL;
    if on_disk_total != declared {
        return Err(format!(
            "STREAMINFO on disk declares {on_disk_total} but the probe read {declared} — refusing to patch an unexpected layout"
        ));
    }

    // Preserve rate/ch/bps (the high 28 bits), replace the low 36 with the
    // authoritative count.
    let new_packed = (packed & !PACKED_MASK_TOTAL) | real;
    let new_bytes = new_packed.to_be_bytes();

    let mut w = OpenOptions::new()
        .write(true)
        .open(path)
        .map_err(|e| format!("open for write failed: {e}"))?;
    w.seek(SeekFrom::Start(PACKED_OFFSET)).map_err(|e| e.to_string())?;
    w.write_all(&new_bytes).map_err(|e| e.to_string())?;
    w.flush().map_err(|e| e.to_string())?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// Minimal FLAC front matter: magic + STREAMINFO block header + body with
    /// the packed rate/ch/bps/total word. 10000 Hz, 1 ch, 8-bit, total 42.
    fn write_minimal_flac(name: &str, total: u64) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(name);
        let mut f = File::create(&p).unwrap();
        f.write_all(b"fLaC").unwrap();
        // block header: type 0 (not last), length 34
        f.write_all(&[0x00, 0x00, 0x00, 0x22]).unwrap();
        let mut body = [0u8; 34];
        // min/max block 4096, min/max frame unset
        body[0..2].copy_from_slice(&4096u16.to_be_bytes());
        body[2..4].copy_from_slice(&4096u16.to_be_bytes());
        body[4..7].copy_from_slice(&[0xFF, 0xFF, 0xFF]);
        body[7..10].copy_from_slice(&[0xFF, 0xFF, 0xFF]);
        // packed word: rate(20) << 44 | (ch-1)(3) << 41 | (bps-1)(5) << 36
        //              | total(36) — 10000 Hz, 1 ch, 8 bps.
        let packed: u64 = (10000u64 << 44) | (0u64 << 41) | (7u64 << 36) | total;
        body[10..18].copy_from_slice(&packed.to_be_bytes());
        f.write_all(&body).unwrap();
        f.write_all(&[0u8; 16]).unwrap(); // md5 placeholder
        p
    }

    fn read_total(p: &Path) -> u64 {
        let mut f = File::open(p).unwrap();
        f.seek(SeekFrom::Start(PACKED_OFFSET)).unwrap();
        let mut b = [0u8; 8];
        f.read_exact(&mut b).unwrap();
        let packed = u64::from_be_bytes(b);
        packed & PACKED_MASK_TOTAL
    }

    #[test]
    fn repair_patches_declared_total() {
        let p = write_minimal_flac("fc_test_si_before.flac", 2_165_571);
        assert_eq!(read_total(&p), 2_165_571);
        let patched = repair_declared_total(&p, 2_165_571, 2_165_570_800).unwrap();
        assert!(patched);
        assert_eq!(read_total(&p), 2_165_570_800);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn repair_is_noop_when_already_correct() {
        let p = write_minimal_flac("fc_test_si_ok.flac", 2_165_570_800);
        assert!(!repair_declared_total(&p, 2_165_570_800, 2_165_570_800).unwrap());
        assert_eq!(read_total(&p), 2_165_570_800);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn repair_refuses_when_on_disk_layout_differs() {
        let p = write_minimal_flac("fc_test_si_mismatch.flac", 2_165_571);
        // Caller says declared=999 but the file holds 2_165_571 → refuse.
        let err = repair_declared_total(&p, 999, 2_165_570_800).unwrap_err();
        assert!(err.contains("refusing"), "got: {err}");
        assert_eq!(read_total(&p), 2_165_571); // untouched
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn repair_skips_counts_that_do_not_fit_36_bits() {
        let p = write_minimal_flac("fc_test_si_36.flac", 2_165_571);
        let huge: u64 = 1 << 36; // cannot be stored in the field
        assert!(!repair_declared_total(&p, 2_165_571, huge).unwrap());
        assert_eq!(read_total(&p), 2_165_571); // untouched
        let _ = std::fs::remove_file(&p);
    }
}
