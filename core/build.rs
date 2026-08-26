//! Build script for flac_chop_core.
//!
//! Only does anything when the `static-sox` cargo feature is enabled. In that
//! mode it links a prebuilt static `libsox` + its dependencies, located via env
//! vars that the CI packaging script (`deps/build-sox-static.sh`) sets:
//!
//!   SOX_STATIC_PREFIX — install prefix (contains lib/ and include/)
//!   SOX_STATIC_LIBS   — space-separated dep lib names to link AFTER libsox
//!                       (e.g. "FLAC ogg vorbis vorbisenc png z")
//!
//! libsox itself is linked with --whole-archive so its static format/effect
//! plugin registration symbols are all pulled in (a static libsox otherwise
//! drops unused plugin objects and cannot open FLAC, etc.).
//!
//! When the feature is off (the default shell-out backend), this script is a
//! no-op so plain `cargo check`/`cargo build` works without any SoX present.

fn main() {
    // Re-run if these change.
    println!("cargo:rerun-if-env-changed=SOX_STATIC_PREFIX");
    println!("cargo:rerun-if-env-changed=SOX_STATIC_LIBS");
    println!("cargo:rerun-if-env-changed=SOX_STATIC_SYSTEM_LIBS");

    if !cfg!(feature = "static-sox") {
        return;
    }

    let prefix = std::env::var("SOX_STATIC_PREFIX").unwrap_or_default();
    if prefix.is_empty() {
        // Feature on but no prefix: nothing to link now. `cargo check` still
        // succeeds (it does not link); a real `cargo build` will fail at link
        // time, which is the intended loud signal that the env is unset.
        return;
    }

    // Header include path (in case any C side needs sox.h via this prefix).
    println!("cargo:rustc-link-search=native={prefix}/lib");

    // libsox whole-archive so all plugin objects are retained (static libsox
    // otherwise drops unused format/effect plugin objects and cannot open
    // FLAC, etc.). The +whole-archive modifier applies per-lib, so deps below
    // are linked normally.
    println!("cargo:rustc-link-lib=static:+whole-archive=sox");

    // Static deps of libsox (order: dependents before dependencies).
    let libs = std::env::var("SOX_STATIC_LIBS").unwrap_or_default();
    for lib in libs.split_whitespace() {
        if lib.is_empty() {
            continue;
        }
        println!("cargo:rustc-link-lib=static={lib}");
    }

    // System link libs needed by a static sox build (platform-dependent),
    // passed through as a verbatim list, e.g. "m" on Unix or "" on Windows.
    let syslibs = std::env::var("SOX_STATIC_SYSTEM_LIBS").unwrap_or_default();
    for lib in syslibs.split_whitespace() {
        if lib.is_empty() {
            continue;
        }
        println!("cargo:rustc-link-lib=dylib={lib}");
    }
}
