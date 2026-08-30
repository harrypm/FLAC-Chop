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
//! Tag schema (from MISRC-GUI `gui_record.c` + the capture pipeline):
//!
//! ```text
//! RF_TOTAL_SAMPLES   = real total sample count at the real rate
//! RF_SAMPLE_RATE     = real rate in Hz   (header_khz * 1000 for RF)
//! RF_SAMPLE_RATE_KHZ = header kHz value   (RF only; = header sample_rate)
//! DURATION_SECONDS   = real duration (s)  (= total / header_rate)
//! LENGTH             = duration in ms
//! ```
//!
//! For RF files the FLAC header `sample_rate` holds the /1000 "kHz" value
//! (e.g. 20000 for 20 MSPS); `RF_TOTAL_SAMPLES` is the STREAMINFO
//! `total_samples` (count at the header rate) multiplied by 1000 to give the
//! real-rate count. For non-RF audio the header `sample_rate` is the real Hz
//! and `RF_SAMPLE_RATE_KHZ` is omitted.

use claxon::{FlacReader, FlacReaderOptions};
use lofty::{AudioFile, ItemKey, ItemValue, TaggedFileExt, TagItem};
use std::fs::File;
use std::path::Path;

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
/// match the actual cut. `is_rf` selects the RF /1000 convention (RF_SAMPLE_RATE
/// = header*1000, RF_TOTAL_SAMPLES = streaminfo_total*1000, + RF_SAMPLE_RATE_KHZ)
/// versus plain audio (RF_SAMPLE_RATE = header, no KHZ tag).
///
/// Returns `Ok(())` on success, or an error string describing why the rewrite
/// failed (the caller may surface it as a non-fatal warning — the cut itself
/// already succeeded).
pub fn rewrite_cut_tags(out_path: &Path, is_rf: bool) -> Result<(), String> {
    // Read the output's STREAMINFO for the authoritative post-cut numbers.
    let (header_rate, total_samples) = read_streaminfo(out_path)?;
    if header_rate == 0 {
        return Err("output STREAMINFO sample_rate is 0".into());
    }
    let duration_sec = total_samples as f64 / header_rate as f64;
    let length_ms = (duration_sec * 1000.0).round() as u64;

    // Open with lofty for the Vorbis-comment read/write.
    let mut tagged = lofty::read_from_path(out_path)
        .map_err(|e| format!("lofty read: {e}"))?;
    let tag = tagged
        .primary_tag_mut()
        .ok_or_else(|| "no primary Vorbis comment tag".to_string())?;

    // Drop the stale numeric RF tags SoX passed through.
    for k in OWNED_TAGS {
        tag.remove_key(&ItemKey::Unknown((*k).into()));
    }

    // Fresh values from the output's own STREAMINFO.
    let (rf_sample_rate, rf_total_samples) = if is_rf {
        let real_hz = (header_rate as u64) * 1000;
        let real_total = total_samples.saturating_mul(1000);
        // RF_SAMPLE_RATE_KHZ = header kHz value.
        tag.push_unchecked(TagItem::new(
            ItemKey::Unknown("RF_SAMPLE_RATE_KHZ".into()),
            ItemValue::Text(header_rate.to_string()),
        ));
        (real_hz, real_total)
    } else {
        (header_rate as u64, total_samples)
    };

    tag.push_unchecked(TagItem::new(
        ItemKey::Unknown("RF_TOTAL_SAMPLES".into()),
        ItemValue::Text(rf_total_samples.to_string()),
    ));
    tag.push_unchecked(TagItem::new(
        ItemKey::Unknown("RF_SAMPLE_RATE".into()),
        ItemValue::Text(rf_sample_rate.to_string()),
    ));
    tag.push_unchecked(TagItem::new(
        ItemKey::Unknown("DURATION_SECONDS".into()),
        ItemValue::Text(format!("{:.6}", duration_sec)),
    ));
    tag.push_unchecked(TagItem::new(
        ItemKey::Unknown("LENGTH".into()),
        ItemValue::Text(length_ms.to_string()),
    ));

    tagged
        .save_to_path(out_path)
        .map_err(|e| format!("lofty save: {e}"))
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
}
