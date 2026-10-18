cmake_minimum_required(VERSION 3.22.1)
project(alphavideo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get the module root directory (react-native-webm-alpha-player/)
get_filename_component(MODULE_ROOT "${CMAKE_SOURCE_DIR}/../../../.." ABSOLUTE)

# libwebm's WebM parser, built from source
set(WEBM_PARSER "${MODULE_ROOT}/third_party/libwebm/webm_parser")
file(GLOB WEBM_PARSER_SOURCES "${WEBM_PARSER}/src/*.cc")

# Range reading and storage, vendored from alpha-video-source (scripts/vendor_alpha_video_source.sh)
set(SOURCE_READER "${MODULE_ROOT}/third_party/alpha-video-source/src")

# Source files
set(NATIVE_SOURCES
    ${MODULE_ROOT}/cpp/AlphaDemuxer.cpp
    ${MODULE_ROOT}/cpp/ByteSource.cpp
    ${SOURCE_READER}/RangeReader.cpp
    ${SOURCE_READER}/Store.cpp
    ${MODULE_ROOT}/cpp/VideoDecoder.cpp
    ${MODULE_ROOT}/cpp/WebMAlphaPlayer.cpp
    ${MODULE_ROOT}/cpp/WebMAlphaPlayer_jni.cpp
    ${MODULE_ROOT}/cpp/YuvToRgb.cpp
    ${WEBM_PARSER_SOURCES}
)

# libvpx and dav1d, prebuilt per ABI (scripts/build_android_codecs.sh)
set(CODECS_ROOT "${MODULE_ROOT}/prebuilt/android/${ANDROID_ABI}")

# Include directories
include_directories(
    ${MODULE_ROOT}/cpp
    ${SOURCE_READER}
    ${MODULE_ROOT}/prebuilt/include
    ${WEBM_PARSER}
    ${WEBM_PARSER}/include
)

# Create the native library
add_library(${PROJECT_NAME} SHARED ${NATIVE_SOURCES})

# Google Play requires 16 KB-aligned segments for Android 15+; NDK r27 and older link 4 KB by default.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-z,max-page-size=16384")
# libvpx and dav1d stay private to this library, so an app that ships its own copies (ffmpeg-kit's
# libavcodec.so links them) can't interpose on them, or they on it.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--exclude-libs,ALL")

# Find Android log library
find_library(log-lib log)

target_link_libraries(${PROJECT_NAME}
    ${CODECS_ROOT}/lib/libvpx.a
    ${CODECS_ROOT}/lib/libdav1d.a
    
    # Android system libraries
    ${log-lib}
    android
    m
)

# Set library properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
)
