# --- Rust core staticlib -----------------------------------------------------
#
# The Rust core is built by cargo (not CMake) and linked as a plain C static
# library. We declare a custom command that reruns `cargo build --release` when
# any Rust source changes, and depend the executable on the resulting lib.
# The staticlib name differs by toolchain: libflac_chop_core.a on Unix/MinGW,
# flac_chop_core.lib on MSVC.

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
)

add_custom_command(
    OUTPUT "${CORE_LIB}"
    COMMAND cargo build --release
    WORKING_DIRECTORY "${CORE_DIR}"
    DEPENDS ${RUST_SOURCES}
    COMMENT "Building Rust core (flac_chop_core) via cargo"
    VERBATIM)

add_custom_target(flac_chop_core DEPENDS "${CORE_LIB}")

# --- GUI executable ----------------------------------------------------------

add_executable(flac-chop
    main.cpp
    mainwindow.cpp
    rangeslider.cpp
    rangeslider.h
)

target_include_directories(flac-chop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Link the Rust staticlib + the Rust std's native deps (which must be supplied
# by the C/C++ linker when statically linking a Rust lib). These differ by
# platform; see https://github.com/rust-lang/cargo/issues/4671.
target_link_libraries(flac-chop PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    "${CORE_LIB}"
)
if(WIN32)
    # MinGW/MSVC: Rust std needs these Win32 import libraries when statically
    # linking a Rust staticlib. ntdll supplies the NT Native API entry points
    # (NtReadFile/NtCreateFile/NtOpenFile/NtWriteFile/RtlNtStatusToDosError/...)
    # that newer Rust std (>= ~1.75 gnu target) calls for async I/O/stdin.
    target_link_libraries(flac-chop PRIVATE ws2_32 advapi32 userenv bcrypt ntdll)
elseif(APPLE)
    target_link_libraries(flac-chop PRIVATE pthread dl m iconv)
else()
    target_link_libraries(flac-chop PRIVATE pthread dl m)
endif()

# Statically link the C++ runtime on MinGW-gcc so the exe itself doesn't need
# libgcc_s_seh-1 / libwinpthread-1 / libstdc++-6 DLLs. Gate on the GNU compiler
# id (not `WIN32 AND MINGW`) so these gcc-specific flags are never passed to
# clang on CLANGARM64; for that build the runtime DLLs are bundled into dist/
# by the ldd-walk step in .github/workflows/build.yml instead.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(flac-chop PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

# Make sure the Rust lib is (re)built before we link against it.
add_dependencies(flac-chop flac_chop_core)
