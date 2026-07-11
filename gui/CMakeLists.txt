# --- Rust core staticlib -----------------------------------------------------
#
# The Rust core is built by cargo (not CMake) and linked as a plain C static
# library. We declare a custom command that reruns `cargo build --release` when
# any Rust source changes, and depend the executable on the resulting .a.

set(CORE_DIR "${CMAKE_SOURCE_DIR}/core")
set(CORE_LIB "${CORE_DIR}/target/release/libflac_chop_core.a")

set(RUST_SOURCES
    "${CORE_DIR}/Cargo.toml"
    "${CORE_DIR}/src/lib.rs"
    "${CORE_DIR}/src/probe.rs"
    "${CORE_DIR}/src/msps.rs"
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

target_link_libraries(flac-chop PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    "${CORE_LIB}"
    pthread
    dl
    m
)

# Make sure the Rust lib is (re)built before we link against it.
add_dependencies(flac-chop flac_chop_core)
