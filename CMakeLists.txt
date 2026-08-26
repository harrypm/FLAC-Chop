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
# Use individual find_package calls instead of find_package(Qt6 COMPONENTS ...)
# because a qtbase-only static build does NOT generate the meta
# Qt6Config.cmake (only module-level configs like Qt6WidgetsConfig.cmake).
# Individual calls work with both shared and static Qt installs.
find_package(Qt6Widgets REQUIRED)
find_package(Qt6Concurrent REQUIRED)
find_package(Qt6Network REQUIRED)

add_subdirectory(gui)
