//! Minimal FFI bindings to libSoX's public effects-chain API.
//!
//! Only the symbols and structs needed by `chop` are declared here. Struct
//! layouts are transcribed verbatim from `sox.h` (chirlu/sox) so that
//! `(*in).signal` / `(*in).encoding` are accessible by field — `sox_format_t`
//! is fully public in the header (not opaque), which is what makes the
//! example0.c-style flow possible from Rust.
//!
//! These are only linked when the `static-sox` cargo feature is enabled; the
//! build script (`core/build.rs`) supplies the static `libsox` + deps via the
//! `SOX_STATIC_PREFIX` environment variable set by the CI build script.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code)]

use std::os::raw::{c_char, c_int, c_uint, c_void};

/// `sox_sample_t` = `sox_int32_t` = `int32_t` (native SoX sample type).
pub type sox_sample_t = i32;
/// `sox_rate_t` = `double`.
pub type sox_rate_t = f64;
/// `sox_uint64_t` = `uint64_t`.
pub type sox_uint64_t = u64;

/// `sox_bool` enum: false = 0, true = 1.
pub type sox_bool = c_int;
pub const sox_false: sox_bool = 0;
pub const sox_true: sox_bool = 1;

/// `sox_option_t` enum: no = 0, yes = 1, default = 2.
pub type sox_option_t = c_int;
pub const sox_option_no: sox_option_t = 0;
pub const sox_option_yes: sox_option_t = 1;
pub const sox_option_default: sox_option_t = 2;

/// `sox_encoding_t` enum. We only need to carry it through opaquely in most
/// places; `SOX_ENCODING_SIGN2` etc. are not hardcoded here because we always
/// copy the encoding struct from the opened input file.
pub type sox_encoding_t = c_int;

/// `sox_signalinfo_t` — transcribed from sox.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sox_signalinfo_t {
    pub rate: sox_rate_t,
    pub channels: c_uint,
    pub precision: c_uint,
    pub length: sox_uint64_t,
    pub mult: *mut f64,
}

impl Default for sox_signalinfo_t {
    fn default() -> Self {
        Self {
            rate: 0.0,
            channels: 0,
            precision: 0,
            length: 0,
            mult: std::ptr::null_mut(),
        }
    }
}

/// `sox_encodinginfo_t` — transcribed from sox.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sox_encodinginfo_t {
    pub encoding: sox_encoding_t,
    pub bits_per_sample: c_uint,
    pub compression: f64,
    pub reverse_bytes: sox_option_t,
    pub reverse_nibbles: sox_option_t,
    pub reverse_bits: sox_option_t,
    pub opposite_endian: sox_bool,
}

impl Default for sox_encodinginfo_t {
    fn default() -> Self {
        Self {
            encoding: 0,
            bits_per_sample: 0,
            compression: 0.0,
            reverse_bytes: sox_option_default,
            reverse_nibbles: sox_option_default,
            reverse_bits: sox_option_default,
            opposite_endian: sox_false,
        }
    }
}

/// `sox_oob_t` — out-of-band data. We never construct this; passed as null.
#[repr(C)]
pub struct sox_oob_t {
    _opaque: [u8; 0],
}

/// `sox_format_t` — transcribed from sox.h (public). We only need the leading
/// fields up to `encoding`; the rest is declared opaque so we don't have to
/// replicate the full (large, handler-bearing) struct. Access is by pointer
/// only; we read `(*ft).signal` and `(*ft).encoding`.
#[repr(C)]
pub struct sox_format_t {
    pub filename: *mut c_char,
    pub signal: sox_signalinfo_t,
    pub encoding: sox_encodinginfo_t,
    // Trailing fields (filetype, oob, seekable, ...) are not needed from Rust;
    // do not access them. Keeping the struct partial is fine because we only
    // ever hold a `*mut sox_format_t` returned by libsox and read the two
    // public fields at known offsets.
    _rest: [u8; 0],
}

/// `sox_effect_handler_t` — opaque; we only hold pointers from `sox_find_effect`.
#[repr(C)]
pub struct sox_effect_handler_t {
    _opaque: [u8; 0],
}

/// `sox_effect_t` — opaque; we only hold pointers from `sox_create_effect`.
#[repr(C)]
pub struct sox_effect_t {
    _opaque: [u8; 0],
}

/// `sox_effects_chain_t` — opaque.
#[repr(C)]
pub struct sox_effects_chain_t {
    _opaque: [u8; 0],
}

/// Callback type for `sox_flow_effects`. We pass null.
pub type sox_flow_effects_callback =
    Option<unsafe extern "C" fn(all_done: sox_bool, client_data: *mut c_void) -> c_int>;

extern "C" {
    pub fn sox_init() -> c_int;
    pub fn sox_quit() -> c_int;

    pub fn sox_open_read(
        path: *const c_char,
        signal: *const sox_signalinfo_t,
        encoding: *const sox_encodinginfo_t,
        filetype: *const c_char,
    ) -> *mut sox_format_t;

    pub fn sox_open_write(
        path: *const c_char,
        signal: *const sox_signalinfo_t,
        encoding: *const sox_encodinginfo_t,
        filetype: *const c_char,
        oob: *const sox_oob_t,
        overwrite_permitted: Option<unsafe extern "C" fn(filename: *const c_char) -> sox_bool>,
    ) -> *mut sox_format_t;

    pub fn sox_close(ft: *mut sox_format_t) -> c_int;

    pub fn sox_find_effect(name: *const c_char) -> *const sox_effect_handler_t;
    pub fn sox_create_effect(eh: *const sox_effect_handler_t) -> *mut sox_effect_t;
    pub fn sox_effect_options(
        effp: *mut sox_effect_t,
        argc: c_int,
        argv: *mut *mut c_char,
    ) -> c_int;

    pub fn sox_create_effects_chain(
        in_enc: *const sox_encodinginfo_t,
        out_enc: *const sox_encodinginfo_t,
    ) -> *mut sox_effects_chain_t;
    pub fn sox_delete_effects_chain(ecp: *mut sox_effects_chain_t);
    pub fn sox_delete_effects(ecp: *mut sox_effects_chain_t);
    pub fn sox_add_effect(
        chain: *mut sox_effects_chain_t,
        effp: *mut sox_effect_t,
        input: *mut sox_signalinfo_t,
        output: *const sox_signalinfo_t,
    ) -> c_int;
    pub fn sox_flow_effects(
        chain: *mut sox_effects_chain_t,
        callback: sox_flow_effects_callback,
        client_data: *mut c_void,
    ) -> c_int;
}

/// SOX_SUCCESS = 0.
pub const SOX_SUCCESS: c_int = 0;
/// SOX_EOF = -1.
pub const SOX_EOF: c_int = -1;
