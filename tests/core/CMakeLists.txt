# Host build of the C++ core (cpp/) against the system FFmpeg, with its tests.
#
#   cmake -S tests/core -B tests/core/build -G Ninja && cmake --build tests/core/build && tests/core/build/core_tests
#
# Needs FFmpeg 6.1 or later with libvpx and an AV1 decoder (macOS: brew install ffmpeg pkg-config).
cmake_minimum_required(VERSION 3.20)
project(alpha_video_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
find_package(Threads REQUIRED)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp)
add_executable(core_tests
  core_tests.cpp
  ${CORE}/WebMAlphaPlayer.cpp
  ${CORE}/FileRangeIO.cpp
  ${CORE}/GrowingFile.cpp
)
target_include_directories(core_tests PRIVATE ${CORE})
target_compile_definitions(core_tests PRIVATE FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
target_compile_options(core_tests PRIVATE -Wall -Wno-deprecated-declarations)
target_link_libraries(core_tests PRIVATE PkgConfig::FFMPEG Threads::Threads)
