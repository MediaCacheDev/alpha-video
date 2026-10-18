# The React Native package's native libraries: the standalone library's core, and the codec
# modules in ALPHA_VIDEO_CODECS (build.gradle), each its own library as in the Maven artifacts.
cmake_minimum_required(VERSION 3.22.1)
project(alphavideo_reactnative)

# "none" rather than empty: an empty -D value doesn't reach CMake, which would then build both.
set(ALPHA_VIDEO_CODECS "vpx;av1" CACHE STRING "Codec modules to build: vpx, av1, or none")
set(ANDROID_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../android")

add_subdirectory("${ANDROID_DIR}/src/main/cpp" core)
list(REMOVE_ITEM ALPHA_VIDEO_CODECS none)
foreach(codec IN LISTS ALPHA_VIDEO_CODECS)
    add_subdirectory("${ANDROID_DIR}/${codec}/src/main/cpp" "${codec}")
endforeach()
