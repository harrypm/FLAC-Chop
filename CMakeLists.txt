cmake_minimum_required(VERSION 3.16)

project(FLACChop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Build version, shown in the window title, --version, and the update check.
#
# CI packaging (.github/workflows/build.yml) ALWAYS passes the resolved
# release version explicitly via -DFLAC_CHOP_VERSION=<version> (tag push or
# release dispatch), so a packaged release build can never fall back to
# "dev-<sha>" — this was the bug that shipped v1.0.8 with "dev-e744de2"
# in-app: the tagless shallow CI checkout made git describe fail.
#
# Without the override (local builds) the version is derived from git to
# match the CI artifact versioning:
#   git describe --tags --dirty --match 'v*'
# falling back to "dev-<short-sha>" when there are no tags / git is absent.
if(NOT DEFINED FLAC_CHOP_VERSION)
    set(FLAC_CHOP_VERSION "dev-unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --match "v*"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _git_desc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _git_rc)
        if(_git_rc EQUAL 0 AND _git_desc)
            set(FLAC_CHOP_VERSION "${_git_desc}")
        else()
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE _git_sha
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_git_sha)
                set(FLAC_CHOP_VERSION "dev-${_git_sha}")
            endif()
        endif()
    endif()
endif()
message(STATUS "FLAC-Chop version: ${FLAC_CHOP_VERSION}")

# Qt6 on this system is 6.2.4 (Debian/Ubuntu qt6-base-dev). The ld-decode /
# ld-analyse project uses the same Widgets + CMake approach.
# Use individual find_package calls instead of find_package(Qt6 COMPONENTS ...)
# because a qtbase-only static build does NOT generate the meta
# Qt6Config.cmake (only module-level configs like Qt6WidgetsConfig.cmake).
# Individual calls work with both shared and static Qt installs.
find_package(Qt6Widgets REQUIRED)
find_package(Qt6Concurrent REQUIRED)
find_package(Qt6Network REQUIRED)

add_subdirectory(gui)
