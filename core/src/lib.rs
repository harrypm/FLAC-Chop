//! FLAC-Chop Rust core.
//!
//! Three concerns, kept separate:
//!  - [`probe`]: read a FLAC file's STREAMINFO via claxon (metadata-only, so the
//!    audio frames are never touched — important for 100+ GB RF captures).
//!  - [`msps`]: pull an `<n>msps` hint out of a filename so we can undo the
//!    RF-capture header convention where the real 20 MSPS rate is stored as
//!    20 kHz in the FLAC header.
//!  - [`chop`]: drive SoX to do the actual sample-exact `trim` cut.
//!
//! [`ffi`] re-exports those as a plain C ABI so the Qt6 C++ GUI can link the
//! staticlib directly.

pub mod chop;
pub mod companions;
pub mod ffi;
pub mod msps;
pub mod probe;
pub mod rate;
pub mod vorbis;
