//! Extract an `<n>msps` hint from a path string.
//!
//! RF captures from the DdD / cxadc pipeline name files like
//! `..._video_rf_8-bit_20msps.flac` but store the real 20 MSPS rate as 20 kHz
//! in the FLAC header (a /1000 convention). Pulling the MSPS out of the name
//! lets the plan compute sample counts at the *real* rate.

/// Scan `s` for the first `<number>msps` (case-insensitive), where `<number>`
/// is an integer or decimal like `20`, `40`, `44.1`. Returns the value in MSPS
/// or `None` if no such token is found.
pub fn extract_msps(s: &str) -> Option<f64> {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i].is_ascii_digit() {
            let start = i;
            // consume digits and at most one '.'; stop before "msps"
            let mut seen_dot = false;
            while i < b.len() {
                let c = b[i];
                if c.is_ascii_digit() {
                    i += 1;
                } else if c == b'.' && !seen_dot {
                    seen_dot = true;
                    i += 1;
                } else {
                    break;
                }
            }
            // need "msps" immediately after the number
            if i + 4 <= b.len() && b[i..i + 4].eq_ignore_ascii_case(b"msps") {
                let num = std::str::from_utf8(&b[start..i]).ok()?;
                let v: f64 = num.parse().ok()?;
                if v > 0.0 {
                    return Some(v);
                }
            }
        } else {
            i += 1;
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_integer_msps() {
        assert_eq!(extract_msps("foo_20msps.flac"), Some(20.0));
    }

    #[test]
    fn finds_decimal_msps() {
        assert_eq!(extract_msps("44.1msps_cap.flac"), Some(44.1));
    }

    #[test]
    fn case_insensitive() {
        assert_eq!(extract_msps("x_40MSPS_y"), Some(40.0));
    }

    #[test]
    fn none_when_absent() {
        assert_eq!(extract_msps("plain.flac"), None);
    }

    #[test]
    fn ignores_trailing_letter_without_msps() {
        assert_eq!(extract_msps("20m_other.flac"), None);
    }
}
