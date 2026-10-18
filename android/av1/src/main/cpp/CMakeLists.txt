# libalphavideo_av1.so: dav1d and its decoder plugin. It doesn't link the core: Av1Codec hands the
# core the plugin list's address (AlphaVideoCodecs.java).
cmake_minimum_required(VERSION 3.22.1)
project(alphavideo_av1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MODULE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
# dav1d, prebuilt per ABI (scripts/build_codecs_android.sh)
set(CODECS_ROOT "${MODULE_ROOT}/prebuilt/android/${ANDROID_ABI}")

add_library(alphavideo_av1 SHARED
    ${MODULE_ROOT}/codecs/av1/Dav1dDecoderPlugin.cpp
    Av1Codec_jni.cpp
)
target_include_directories(alphavideo_av1 PRIVATE
    ${MODULE_ROOT}/cpp
    ${MODULE_ROOT}/codecs/av1
    ${MODULE_ROOT}/prebuilt/include
)

# Google Play requires 16 KB-aligned segments for Android 15+; NDK r27 and older link 4 KB by default.
target_link_options(alphavideo_av1 PRIVATE "-Wl,-z,max-page-size=16384")
# dav1d stays private to this library, so an app that ships its own copy (ffmpeg-kit's
# libavcodec.so links it) can't interpose on it, or it on the app's.
target_link_options(alphavideo_av1 PRIVATE "-Wl,--exclude-libs,ALL")

target_link_libraries(alphavideo_av1 ${CODECS_ROOT}/lib/libdav1d.a m)

set_target_properties(alphavideo_av1 PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
)
