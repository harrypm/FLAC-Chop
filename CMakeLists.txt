cmake_minimum_required(VERSION 3.16)

project(FLACChop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt6 on this system is 6.2.4 (Debian/Ubuntu qt6-base-dev). The ld-decode /
# ld-analyse project uses the same Widgets + CMake approach.
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

add_subdirectory(gui)
