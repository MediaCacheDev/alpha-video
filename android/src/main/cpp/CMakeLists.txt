cmake_minimum_required(VERSION 3.22.1)
project(alphavideo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get the module root directory (react-native-webm-alpha-player/). CMAKE_CURRENT_SOURCE_DIR, not
# CMAKE_SOURCE_DIR, so that React Native's build can add this directory to its own.
get_filename_component(MODULE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# libwebm's WebM parser, built from source
set(WEBM_PARSER "${MODULE_ROOT}/third_party/libwebm/webm_parser")
file(GLOB WEBM_PARSER_SOURCES "${WEBM_PARSER}/src/*.cc")

# range-source, vendored by scripts/vendor_range_source.sh: its C interface and the basic input
# (include/), and the add-on's reader and stores (src/), compiled in until the add-on is its own package
set(RANGE_SOURCE "${MODULE_ROOT}/third_party/range-source")

# Source files. The software decoders are codec modules (android/vpx, android/av1), each its own
# library, which register with the core at run time (AlphaVideoCodecs.java).
set(NATIVE_SOURCES
    ${MODULE_ROOT}/cpp/AlphaDemuxer.cpp
    ${MODULE_ROOT}/cpp/Av1SequenceHeader.cpp
    ${MODULE_ROOT}/cpp/ByteSource.cpp
    ${RANGE_SOURCE}/src/RangeReader.cpp
    ${RANGE_SOURCE}/src/Store.cpp
    ${RANGE_SOURCE}/src/range_source.cpp
    ${MODULE_ROOT}/cpp/VideoDecoder.cpp
    ${MODULE_ROOT}/cpp/WebMAlphaPlayer.cpp
    ${MODULE_ROOT}/cpp/WebMAlphaPlayer_jni.cpp
    ${MODULE_ROOT}/cpp/YuvToRgb.cpp
    ${WEBM_PARSER_SOURCES}
)

# Create the native library
add_library(alphavideo SHARED ${NATIVE_SOURCES})

target_include_directories(alphavideo PRIVATE
    ${MODULE_ROOT}/cpp
    ${RANGE_SOURCE}/include
    ${RANGE_SOURCE}/src
    ${WEBM_PARSER}
    ${WEBM_PARSER}/include
)

# Google Play requires 16 KB-aligned segments for Android 15+; NDK r27 and older link 4 KB by default.
target_link_options(alphavideo PRIVATE "-Wl,-z,max-page-size=16384")

# Find Android log library
find_library(log-lib log)

target_link_libraries(alphavideo
    ${log-lib}
    android
    m
)

# Set library properties
set_target_properties(alphavideo PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
)
