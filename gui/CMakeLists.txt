# --- Rust core staticlib -----------------------------------------------------
#
# The Rust core is built by cargo (not CMake) and linked as a plain C static
# library. We declare a custom command that reruns `cargo build --release` when
# any Rust source changes, and depend the executable on the resulting lib.
# The staticlib name differs by toolchain: libflac_chop_core.a on Unix/MinGW,
# flac_chop_core.lib on MSVC.

# --- Single-binary build options ---------------------------------------------
#
# FLAC_CHOP_STATIC_SOX: build the Rust core with the `static-sox` cargo feature
#   (in-process libSoX, no external sox binary). Requires SOX_STATIC_PREFIX
#   (and optional SOX_STATIC_LIBS / SOX_STATIC_SYSTEM_LIBS) to point at a
#   prebuilt static libsox + deps. See deps/build-sox-static.sh.
# FLAC_CHOP_STATIC_QT: link Qt6 statically. Requires CMAKE_PREFIX_PATH to point
#   at a static Qt6 install. Pulls in the required platform/imageformat plugins
#   via qt_import_plugins (no-op for shared Qt) so the single exe starts without
#   any DLL beside it.
option(FLAC_CHOP_STATIC_SOX "Link libSoX statically (in-process cut, no sox binary)" OFF)
option(FLAC_CHOP_STATIC_QT "Link Qt6 statically (single-binary builds)" OFF)

set(CORE_DIR "${CMAKE_SOURCE_DIR}/core")
# Rust staticlib naming: lib<name>.a for every MinGW-style toolchain (gcc on
# MINGW64, clang on CLANGARM64/aarch64-pc-windows-gnullvm) and <name>.lib for
# MSVC. Gate on MSVC (not `WIN32 AND NOT MINGW`) because CMake does not
# reliably set MINGW for the clang-mingw toolchains, which would wrongly pick
# .lib and fail to find the gnullvm staticlib (.a).
if(MSVC)
    set(CORE_LIB "${CORE_DIR}/target/release/flac_chop_core.lib")
else()
    set(CORE_LIB "${CORE_DIR}/target/release/libflac_chop_core.a")
endif()

set(RUST_SOURCES
    "${CORE_DIR}/Cargo.toml"
    "${CORE_DIR}/src/lib.rs"
    "${CORE_DIR}/src/probe.rs"
    "${CORE_DIR}/src/msps.rs"
    "${CORE_DIR}/src/rate.rs"
    "${CORE_DIR}/src/vorbis.rs"
    "${CORE_DIR}/src/companions.rs"
    "${CORE_DIR}/src/chop.rs"
    "${CORE_DIR}/src/ffi.rs"
    "${CORE_DIR}/src/sox_ffi.rs"
    "${CORE_DIR}/build.rs"
)

if(FLAC_CHOP_STATIC_SOX)
    set(CARGO_BUILD_CMD ${CMAKE_COMMAND} -E env
        "SOX_STATIC_PREFIX=${SOX_STATIC_PREFIX}"
        "SOX_STATIC_LIBS=${SOX_STATIC_LIBS}"
        "SOX_STATIC_SYSTEM_LIBS=${SOX_STATIC_SYSTEM_LIBS}"
        cargo build --release --features static-sox)
    set(CARGO_COMMENT "Building Rust core (flac_chop_core) via cargo [static-sox]")
else()
    set(CARGO_BUILD_CMD cargo build --release)
    set(CARGO_COMMENT "Building Rust core (flac_chop_core) via cargo")
endif()

add_custom_command(
    OUTPUT "${CORE_LIB}"
    COMMAND ${CARGO_BUILD_CMD}
    WORKING_DIRECTORY "${CORE_DIR}"
    DEPENDS ${RUST_SOURCES}
    COMMENT "${CARGO_COMMENT}"
    VERBATIM)

add_custom_target(flac_chop_core DEPENDS "${CORE_LIB}")

# --- GUI executable ----------------------------------------------------------

set(GUI_SOURCES
    main.cpp
    mainwindow.cpp
    rangeslider.cpp
    rangeslider.h
    resources.qrc
)
if(WIN32)
    list(APPEND GUI_SOURCES windows_app_icon.rc)
endif()
if(WIN32)
    add_executable(flac-chop WIN32 ${GUI_SOURCES})
else()
    add_executable(flac-chop ${GUI_SOURCES})
endif()
if(WIN32)
    set_source_files_properties(
        windows_app_icon.rc
        PROPERTIES
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/icons/flac-chop-icon.ico")
endif()

target_include_directories(flac-chop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Inject the git-derived version (from the top-level CMakeLists.txt) so the
# GUI reports the real release tag, not a hard-coded string.
target_compile_definitions(flac-chop PRIVATE FLAC_CHOP_VERSION="${FLAC_CHOP_VERSION}")

# Link the Rust staticlib + the Rust std's native deps (which must be supplied
# by the C/C++ linker when statically linking a Rust lib). These differ by
# platform; see https://github.com/rust-lang/cargo/issues/4671.
target_link_libraries(flac-chop PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    "${CORE_LIB}"
)
if(WIN32)
    # MinGW/MSVC: Rust std needs these Win32 import libraries when statically
    # linking a Rust staticlib. ntdll supplies the NT Native API entry points
    # (NtReadFile/NtCreateFile/NtOpenFile/NtWriteFile/RtlNtStatusToDosError/...)
    # that newer Rust std (>= ~1.75 gnu target) calls for async I/O/stdin.
    target_link_libraries(flac-chop PRIVATE ws2_32 advapi32 userenv bcrypt ntdll)
    # Static libsox (SoX 14.4.2) references OpenMP (GOMP_*/omp_*) symbols from
    # its effects/dsp code; supply libgomp on the final link. Only needed for
    # the in-process static-sox build (MinGW gcc); the shared-sox arm64 build
    # doesn't set FLAC_CHOP_STATIC_SOX. -lgomp is resolved to libgomp.a by the
    # -static link option below.
    if(FLAC_CHOP_STATIC_SOX)
        target_link_libraries(flac-chop PRIVATE gomp)
    endif()
elseif(APPLE)
    target_link_libraries(flac-chop PRIVATE pthread dl m iconv)
else()
    target_link_libraries(flac-chop PRIVATE pthread dl m)
endif()

# Statically link the C/C++ runtime so the exe doesn't need libgcc_s_seh-1 /
# libwinpthread-1 / libstdc++-6 (gcc) or libc++ / libunwind (clang-mingw) DLLs.
#
# For a static-Qt single-binary build we always pass -static on every Windows
# toolchain (gcc + clang-mingw) so no runtime DLL is emitted beside the exe.
# For the shared-Qt build we only do this on gcc (CLANGARM64's runtime DLLs are
# instead bundled into dist/ by the ldd-walk step in the workflow).
if(WIN32)
    if(FLAC_CHOP_STATIC_QT)
        target_link_options(flac-chop PRIVATE -static)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_link_options(flac-chop PRIVATE -static-libgcc -static-libstdc++)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(flac-chop PRIVATE -static-libgcc -static-libstdc++ -static)
    endif()
endif()

# --- Static Qt plugin import -------------------------------------------------
# When Qt is linked statically the platform + imageformat plugins must be
# pulled in explicitly, otherwise the exe starts with no platform plugin.
# Only emit this for a static Qt build: a shared Qt install does not export
# the plugin CMake targets (qt_import_plugins would warn "not known to the
# current Qt installation"), and shared builds load plugins at runtime via
# windeployqt instead. PNG is built into QtGui via libpng, so only the ICO
# image format plugin is needed beyond the platform plugin.
if(FLAC_CHOP_STATIC_QT)
    if(WIN32)
        qt_import_plugins(flac-chop
            INCLUDE QWindowsIntegrationPlugin QICOPlugin)
    else()
        qt_import_plugins(flac-chop
            INCLUDE QCocoaIntegrationPlugin QICOPlugin)
    endif()
endif()

# Make sure the Rust lib is (re)built before we link against it.
add_dependencies(flac-chop flac_chop_core)
