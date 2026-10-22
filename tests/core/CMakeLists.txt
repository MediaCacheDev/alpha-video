# Host build of the C++ core (cpp/) against the system libvpx and dav1d, with its tests.
#
#   cmake -S tests/core -B tests/core/build -G Ninja && cmake --build tests/core/build && tests/core/build/core_tests
#   tests/core/build/codec_registry_tests
#
# Needs libvpx and dav1d (macOS: brew install libvpx dav1d pkg-config), and a range-source checkout
# beside this repository (or -DRANGE_SOURCE_DIR=...): the tests link the add-on as an app does.
# With FFmpeg 6.1 or later too (brew install ffmpeg), it also builds demuxer_compare, which checks
# the core's demuxer against the web's FFmpeg one.
cmake_minimum_required(VERSION 3.20)
project(alpha_video_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)
pkg_check_modules(DAV1D REQUIRED IMPORTED_TARGET dav1d)
pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
find_package(Threads REQUIRED)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CORE ${ROOT}/cpp)
set(WEBM_PARSER ${ROOT}/third_party/libwebm/webm_parser)
set(RANGE_SOURCE ${ROOT}/third_party/range-source)
file(GLOB WEBM_PARSER_SOURCES ${WEBM_PARSER}/src/*.cc)

add_library(webm_parser STATIC ${WEBM_PARSER_SOURCES})
target_include_directories(webm_parser PUBLIC ${WEBM_PARSER} ${WEBM_PARSER}/include)

set(RANGE_SOURCE_DIR "" CACHE PATH "A range-source checkout")
if(NOT RANGE_SOURCE_DIR)
  foreach(candidate ${ROOT}/../range-source ${ROOT}/../../range-source)
    if(EXISTS ${candidate}/native/src/range_source.cpp)
      set(RANGE_SOURCE_DIR ${candidate})
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS ${RANGE_SOURCE_DIR}/native/src/range_source.cpp)
  message(FATAL_ERROR "No range-source checkout found beside this repository; pass -DRANGE_SOURCE_DIR=")
endif()
set(RANGE_SOURCE_ADDON ${RANGE_SOURCE_DIR}/native/src)
add_library(range_source STATIC ${RANGE_SOURCE_ADDON}/RangeReader.cpp ${RANGE_SOURCE_ADDON}/Store.cpp ${RANGE_SOURCE_ADDON}/range_source.cpp)
target_include_directories(range_source PRIVATE ${RANGE_SOURCE_DIR}/native/include ${RANGE_SOURCE_ADDON})
target_link_libraries(range_source PUBLIC Threads::Threads)

add_library(alpha_video_core STATIC
  ${CORE}/WebMAlphaPlayer.cpp
  ${CORE}/AlphaDemuxer.cpp
  ${CORE}/Av1SequenceHeader.cpp
  ${CORE}/VideoDecoder.cpp
  ${CORE}/YuvToRgb.cpp
  ${CORE}/ByteSource.cpp
)
target_include_directories(alpha_video_core PUBLIC ${CORE} ${RANGE_SOURCE}/include)
target_compile_options(alpha_video_core PRIVATE -Wall)
target_link_libraries(alpha_video_core PUBLIC webm_parser Threads::Threads)

# The codec modules: decoder plugins the player's core finds only once they're registered
add_library(alpha_video_vpx STATIC ${ROOT}/codecs/vpx/VpxDecoderPlugin.cpp)
target_include_directories(alpha_video_vpx PUBLIC ${CORE})
target_compile_options(alpha_video_vpx PRIVATE -Wall)
target_link_libraries(alpha_video_vpx PUBLIC PkgConfig::VPX)
add_library(alpha_video_av1 STATIC ${ROOT}/codecs/av1/Dav1dDecoderPlugin.cpp)
target_include_directories(alpha_video_av1 PUBLIC ${CORE})
target_compile_options(alpha_video_av1 PRIVATE -Wall)
target_link_libraries(alpha_video_av1 PUBLIC PkgConfig::DAV1D)

add_executable(core_tests core_tests.cpp)
target_compile_definitions(core_tests PRIVATE FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
target_compile_options(core_tests PRIVATE -Wall)
target_link_libraries(core_tests PRIVATE alpha_video_core alpha_video_vpx alpha_video_av1 range_source)

add_executable(codec_registry_tests codec_registry_tests.cpp)
target_compile_definitions(codec_registry_tests PRIVATE FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
target_compile_options(codec_registry_tests PRIVATE -Wall)
target_link_libraries(codec_registry_tests PRIVATE alpha_video_core alpha_video_vpx alpha_video_av1)

if(FFMPEG_FOUND)
  # The FFmpeg demuxer the web used before the core's, renamed so that it links beside the core's
  add_library(ffmpeg_demuxer STATIC ffmpeg/AlphaDemuxer.cpp)
  target_compile_definitions(ffmpeg_demuxer PRIVATE webm=ffwebm)
  target_link_libraries(ffmpeg_demuxer PUBLIC PkgConfig::FFMPEG)

  add_executable(demuxer_compare demuxer_compare.cpp
    ${CORE}/AlphaDemuxer.cpp ${CORE}/Av1SequenceHeader.cpp ${CORE}/YuvToRgb.cpp ${CORE}/ByteSource.cpp)
  target_include_directories(demuxer_compare PRIVATE ${CORE} ${RANGE_SOURCE}/include)
  target_compile_options(demuxer_compare PRIVATE -Wall)
  target_link_libraries(demuxer_compare PRIVATE ffmpeg_demuxer webm_parser range_source Threads::Threads)
endif()
