//! Resolve the real sample rate of a FLAC file from its header rate.
//!
//! RF captures from the DdD / cxadc pipeline store the real MSPS rate divided
//! by 1000 in the FLAC STREAMINFO `sample_rate` field — e.g. a 20 MSPS capture
//! is stored as 20000 Hz (20 kHz). This is the "fake kHz" metadata convention.
//!
//! Real audio files, by contrast, store their true sample rate (48000,
//! 96000, 192000, 44100, …) and must NOT be multiplied by 1000.
//!
//! ## Rule (per user spec)
//! Default assumption: the file is RF and the header rate is the real rate
//! divided by 1000 → `real_rate = header_rate * 1000`.
//! Exception: if the header rate matches a standard *audio* sample rate
//! (48000, 96000, 192000, 44100, …, within tolerance), treat it as real audio
//! and use the header rate as-is.
//!
//! A filename `<n>msps` hint, when present, confirms the MSPS value and is
//! used in preference to `header * 1000` (they normally agree for RF files).

/// Standard audio sample rates (Hz). A header rate within `TOL` of one of
/// these is treated as real audio, not RF /1000.
///
/// RF /1000 rates in the DdD/cxadc pipeline are 10000, 20000, 40000 (and
/// 8000 for 8 MSPS hifi) — none of which are in this set, so they fall through
/// to the RF assumption.
const AUDIO_RATES: &[u64] = &[
    22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 352800, 384000,
];

/// Tolerance (fraction of the target rate) for matching an audio rate.
/// 5% keeps the standard RF rates (10000, 20000, 40000) safely away from the
/// nearest audio rate (e.g. 20000 is ~9% below 22050, 40000 is ~9% below
/// 44100) while accepting slightly off-spec audio rates.
const TOL: f64 = 0.05;

/// True if `header_rate` is within `TOL` of a standard audio sample rate.
fn is_audio_rate(header_rate: u64) -> bool {
    let h = header_rate as f64;
    AUDIO_RATES.iter().any(|&a| {
        let a = a as f64;
        (h - a).abs() <= a * TOL
    })
}

/// Resolve the real sample rate in Hz.
///
/// Returns `(real_rate_hz, is_rf)`:
///  - `(header_rate, false)` when the header is a standard audio rate.
///  - `(msps * 1e6, true)` when an `<n>msps` filename hint is present (RF).
///  - `(header_rate * 1000, true)` otherwise (RF /1000 assumption).
pub fn resolve_real_rate(header_rate: u64, msps_hint: Option<f64>) -> (f64, bool) {
    if is_audio_rate(header_rate) {
        return (header_rate as f64, false);
    }
    if let Some(m) = msps_hint {
        if m > 0.0 {
            return (m * 1_000_000.0, true);
        }
    }
    (header_rate as f64 * 1000.0, true)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn audio_rate_48k_used_as_is() {
        assert_eq!(resolve_real_rate(48_000, None), (48_000.0, false));
    }

    #[test]
    fn audio_rate_96k_used_as_is() {
        assert_eq!(resolve_real_rate(96_000, None), (96_000.0, false));
    }

    #[test]
    fn audio_rate_192k_used_as_is() {
        assert_eq!(resolve_real_rate(192_000, None), (192_000.0, false));
    }

    #[test]
    fn audio_rate_441k_used_as_is() {
        assert_eq!(resolve_real_rate(44_100, None), (44_100.0, false));
    }

    #[test]
    fn rf_20khz_header_assumes_20msps() {
        // 20000 Hz header → 20 MSPS RF (the /1000 convention).
        assert_eq!(resolve_real_rate(20_000, None), (20_000_000.0, true));
    }

    #[test]
    fn rf_40khz_header_assumes_40msps() {
        assert_eq!(resolve_real_rate(40_000, None), (40_000_000.0, true));
    }

    #[test]
    fn rf_10khz_header_assumes_10msps() {
        assert_eq!(resolve_real_rate(10_000, None), (10_000_000.0, true));
    }

    #[test]
    fn msps_hint_confirms_rf_rate() {
        // filename says 20msps, header 20000 → 20e6 (matches header*1000).
        assert_eq!(resolve_real_rate(20_000, Some(20.0)), (20_000_000.0, true));
    }

    #[test]
    fn audio_rate_wins_over_msps_hint() {
        // A 48000 Hz audio file that happens to have "20msps" in the name
        // is still audio — the header rate is authoritative.
        assert_eq!(resolve_real_rate(48_000, Some(20.0)), (48_000.0, false));
    }

    #[test]
    fn slightly_off_audio_rate_still_audio() {
        // 47900 is within 5% of 48000 → audio.
        let (r, rf) = resolve_real_rate(47_900, None);
        assert!(!rf);
        assert!((r - 47_900.0).abs() < 1e-6);
    }

    #[test]
    fn rf_rate_not_falsely_matched_as_audio() {
        // 20000 must NOT match 22050 (it's ~9.3% off, > 5% tol) → RF.
        let (_, rf) = resolve_real_rate(20_000, None);
        assert!(rf);
    }
}
