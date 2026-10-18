# Host build of the C++ core (cpp/) against the system libvpx and dav1d, with its tests.
#
#   cmake -S tests/core -B tests/core/build -G Ninja && cmake --build tests/core/build && tests/core/build/core_tests
#
# Needs libvpx and dav1d (macOS: brew install libvpx dav1d pkg-config). With FFmpeg 6.1 or later
# too (brew install ffmpeg), it also builds demuxer_compare, which checks the core's demuxer
# against the web's FFmpeg one.
cmake_minimum_required(VERSION 3.20)
project(alpha_video_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CODECS REQUIRED IMPORTED_TARGET vpx dav1d)
pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
find_package(Threads REQUIRED)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CORE ${ROOT}/cpp)
set(WEBM_PARSER ${ROOT}/third_party/libwebm/webm_parser)
file(GLOB WEBM_PARSER_SOURCES ${WEBM_PARSER}/src/*.cc)

add_library(webm_parser STATIC ${WEBM_PARSER_SOURCES})
target_include_directories(webm_parser PUBLIC ${WEBM_PARSER} ${WEBM_PARSER}/include)

add_library(alpha_video_core STATIC
  ${CORE}/WebMAlphaPlayer.cpp
  ${CORE}/AlphaDemuxer.cpp
  ${CORE}/VideoDecoder.cpp
  ${CORE}/YuvToRgb.cpp
  ${CORE}/ByteSource.cpp
  ${CORE}/GrowingFile.cpp
)
target_include_directories(alpha_video_core PUBLIC ${CORE})
target_compile_options(alpha_video_core PRIVATE -Wall)
target_link_libraries(alpha_video_core PUBLIC webm_parser PkgConfig::CODECS Threads::Threads)

add_executable(core_tests core_tests.cpp)
target_compile_definitions(core_tests PRIVATE FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
target_compile_options(core_tests PRIVATE -Wall)
target_link_libraries(core_tests PRIVATE alpha_video_core)

if(FFMPEG_FOUND)
  # The FFmpeg demuxer the web used before the core's, renamed so that it links beside the core's
  add_library(ffmpeg_demuxer STATIC ffmpeg/AlphaDemuxer.cpp)
  target_compile_definitions(ffmpeg_demuxer PRIVATE webm=ffwebm)
  target_link_libraries(ffmpeg_demuxer PUBLIC PkgConfig::FFMPEG)

  add_executable(demuxer_compare demuxer_compare.cpp
    ${CORE}/AlphaDemuxer.cpp ${CORE}/YuvToRgb.cpp ${CORE}/ByteSource.cpp ${CORE}/GrowingFile.cpp)
  target_include_directories(demuxer_compare PRIVATE ${CORE})
  target_compile_options(demuxer_compare PRIVATE -Wall)
  target_link_libraries(demuxer_compare PRIVATE ffmpeg_demuxer webm_parser PkgConfig::CODECS Threads::Threads)
endif()
