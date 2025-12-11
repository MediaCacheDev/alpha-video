cmake_minimum_required(VERSION 3.22.1)
project(webmalphaplayer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get the module root directory (react-native-webm-alpha-player/)
get_filename_component(MODULE_ROOT "${CMAKE_SOURCE_DIR}/../../../.." ABSOLUTE)

# Source files
set(NATIVE_SOURCES
    ${MODULE_ROOT}/cpp/WebMAlphaPlayer.cpp
    ${MODULE_ROOT}/cpp/WebMAlphaPlayer_jni.cpp
)

# FFmpeg paths - use absolute path from module root
set(FFMPEG_ROOT "${MODULE_ROOT}/prebuilt/android/${ANDROID_ABI}")

# Include directories
include_directories(
    ${MODULE_ROOT}/cpp
    ${MODULE_ROOT}/prebuilt/include
)

# Link directories
link_directories(${FFMPEG_ROOT}/lib)

# Create the native library
add_library(${PROJECT_NAME} SHARED ${NATIVE_SOURCES})

# Find Android log library
find_library(log-lib log)

# Link FFmpeg libraries and Android dependencies
target_link_libraries(${PROJECT_NAME}
    # FFmpeg libraries (order matters for static linking!)
    ${FFMPEG_ROOT}/lib/libavformat.a
    ${FFMPEG_ROOT}/lib/libavcodec.a
    ${FFMPEG_ROOT}/lib/libswscale.a
    ${FFMPEG_ROOT}/lib/libswresample.a
    ${FFMPEG_ROOT}/lib/libavutil.a
    ${FFMPEG_ROOT}/lib/libvpx.a
    
    # Android system libraries
    ${log-lib}
    android
    z
    m
)

# Set library properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
)
