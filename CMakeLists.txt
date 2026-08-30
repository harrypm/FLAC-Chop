cmake_minimum_required(VERSION 3.16)

project(FLACChop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Derive the build version from git so the in-app version (and update check)
# match the actual release tag, not a hard-coded string. Mirrors the CI
# artifact versioning in .github/workflows/build.yml:
#   git describe --tags --dirty --match 'v*'
# falling back to "dev-<short-sha>" when there are no tags / git is absent.
find_package(Git QUIET)
set(FLAC_CHOP_VERSION "dev-unknown")
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
