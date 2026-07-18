//! Read the custom RF metadata Vorbis comment tags written by the MISRC / DdD
//! capture pipeline.
//!
//! These tags live inside the FLAC file itself (no sibling dependency) and are
//! the authoritative source the capture tool wrote, e.g. for a 20 MSPS RF
//! capture whose real duration is 4403.98 s:
//!
//! ```text
//! DURATION_SECONDS = 4403.980000   real duration in seconds
//! LENGTH           = 4403980       length in milliseconds
//! RF_TOTAL_SAMPLES = 88079600      true sample count at the real rate
//! RF_SAMPLE_RATE   = 20000         the /1000 "kHz" header value
//! ```
//!
//! `RF_TOTAL_SAMPLES / RF_SAMPLE_RATE` reproduces `DURATION_SECONDS`, so the
//! three are self-consistent and cross-checkable. We prefer `RF_TOTAL_SAMPLES`
//! (exact integer count) over `DURATION_SECONDS` (float, rounding) for the
//! sample count, and use `RF_SAMPLE_RATE` to confirm the RF /1000 intent.
//!
//! ## Units caveat (important)
//!
//! In the early tag schema shown above, `RF_SAMPLE_RATE` is the /1000 "kHz"
//! header value (20000), and therefore `RF_TOTAL_SAMPLES` is a count *at that
//! rate* — i.e. the on-disk sample count divided by 1000 (a 4403.98 s capture
//! at 20 MSPS holds ~88 *billion* samples on disk, not 88 million). In the
//! later schema `RF_SAMPLE_RATE` holds the real Hz value (20000000) and
//! `RF_TOTAL_SAMPLES` is the true on-disk count. This module returns the raw
//! tag values; [`crate::probe`] detects which schema is in play (rate < 1 MHz
//! ⇒ /1000 schema) and rescales both to on-disk units, cross-checking the
//! result against the compressed payload size.

use claxon::{FlacReader, FlacReaderOptions};
use std::fs::File;
use std::path::Path;

/// RF metadata pulled from a FLAC's Vorbis comment block. Any field may be
/// `None` if the tag was absent or unparseable.
#[derive(Debug, Clone, Default)]
pub struct RfTags {
    /// `RF_TOTAL_SAMPLES` — true total inter-channel sample count at the real
    /// rate (what SoX `trim` works in for mono RF).
    pub total_samples: Option<u64>,
    /// `RF_SAMPLE_RATE` — the value stored in the FLAC header (the real rate
    /// divided by 1000). Used to confirm the RF /1000 assumption.
    pub sample_rate: Option<u64>,
    /// `DURATION_SECONDS` — real duration in seconds (float).
    pub duration_seconds: Option<f64>,
}

/// Parse the first value of a Vorbis comment tag as `u64` (case-insensitive
/// name lookup). Returns `None` if absent or not a clean integer.
fn tag_u64(reader: &FlacReader<File>, name: &str) -> Option<u64> {
    reader.get_tag(name).next().and_then(|v| v.parse::<u64>().ok())
}

/// Parse the first value of a Vorbis comment tag as `f64`.
fn tag_f64(reader: &FlacReader<File>, name: &str) -> Option<f64> {
    reader.get_tag(name).next().and_then(|v| v.parse::<f64>().ok())
}

/// Read the RF metadata tags out of an already-open claxon reader (which must
/// have been created with `read_vorbis_comment: true`). Lets the probe reuse
/// its own STREAMINFO reader instead of opening and re-parsing the file.
pub fn rf_tags_from_reader(reader: &FlacReader<File>) -> RfTags {
    RfTags {
        total_samples: tag_u64(reader, "RF_TOTAL_SAMPLES"),
        sample_rate: tag_u64(reader, "RF_SAMPLE_RATE"),
        duration_seconds: tag_f64(reader, "DURATION_SECONDS"),
    }
}

/// Open `path` and read the RF metadata Vorbis comment tags. Returns an empty
/// `RfTags` (all `None`) if the file can't be opened, isn't FLAC, or has no
/// Vorbis comment — the caller falls back to other sources. Reads only the
/// metadata blocks (STREAMINFO + Vorbis comment); never reads audio frames, so
/// even a 100 GB file probes in milliseconds.
pub fn read_rf_tags(path: &Path) -> RfTags {
    let file = match File::open(path) {
        Ok(f) => f,
        Err(_) => return RfTags::default(),
    };
    let opts = FlacReaderOptions {
        metadata_only: true,
        read_vorbis_comment: true,
    };
    let reader = match FlacReader::new_ext(file, opts) {
        Ok(r) => r,
        Err(_) => return RfTags::default(),
    };
    rf_tags_from_reader(&reader)
}

#[cfg(test)]
mod tests {
    use super::*;

    // We can't easily synthesize a FLAC in a unit test, so these cover the
    // parsing helpers indirectly via the public API on a non-FLAC file (which
    // must return all-None, never panic).
    #[test]
    fn missing_file_returns_empty_tags() {
        let t = read_rf_tags(Path::new("/nonexistent/nope.flac"));
        assert!(t.total_samples.is_none());
        assert!(t.sample_rate.is_none());
        assert!(t.duration_seconds.is_none());
    }

    #[test]
    fn non_flac_file_returns_empty_tags() {
        // /dev/null is not a FLAC stream — must return empty, not panic.
        let t = read_rf_tags(Path::new("/dev/null"));
        assert!(t.total_samples.is_none());
    }
}
