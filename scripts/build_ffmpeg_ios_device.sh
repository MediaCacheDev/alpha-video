#!/bin/bash
#
# build_ffmpeg_ios_device.sh
# Builds FFmpeg for iOS Device (arm64 only)
# With VP9 (libvpx) support for WebM alpha playback
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FFMPEG_VERSION="6.1"
LIBVPX_VERSION="1.14.0"
BUILD_DIR="$SCRIPT_DIR/build/ios-device"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/ios"

# iOS deployment target
IOS_MIN_VERSION="13.0"

# FFmpeg and libvpx embed their configure lines in the libraries, so installs are staged
# with DESTDIR and libvpx is referenced by relative path; no local paths end up in them.
INSTALL_PREFIX="/usr/local"

echo "🔧 Building FFmpeg $FFMPEG_VERSION for iOS Device (arm64)"
echo "   Build dir: $BUILD_DIR"
echo "   Output dir: $OUTPUT_DIR"

mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR/lib"
mkdir -p "$OUTPUT_DIR/include"

# Download FFmpeg source
download_ffmpeg() {
    if [ ! -d "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION" ]; then
        echo "📥 Downloading FFmpeg $FFMPEG_VERSION..."
        cd "$BUILD_DIR"
        curl -L "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" -o ffmpeg.tar.xz
        tar xf ffmpeg.tar.xz
        rm ffmpeg.tar.xz
    fi
}

# Download libvpx source
download_libvpx() {
    if [ ! -d "$BUILD_DIR/libvpx-$LIBVPX_VERSION" ]; then
        echo "📥 Downloading libvpx $LIBVPX_VERSION..."
        cd "$BUILD_DIR"
        curl -L "https://github.com/webmproject/libvpx/archive/refs/tags/v$LIBVPX_VERSION.tar.gz" -o libvpx.tar.gz
        tar xf libvpx.tar.gz
        rm libvpx.tar.gz
    fi
}

# Build libvpx for iOS Device
build_libvpx_device() {
    echo "🔨 Building libvpx for iOS Device (arm64)..."
    
    cd "$BUILD_DIR/libvpx-$LIBVPX_VERSION"
    
    local STAGE="$BUILD_DIR/libvpx-arm64"
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    
    # Clean previous build
    make clean 2>/dev/null || true
    
    local SDK_PATH=$(xcrun --sdk iphoneos --show-sdk-path)
    local CC="$(xcrun --sdk iphoneos -f clang)"
    local CXX="$(xcrun --sdk iphoneos -f clang++)"
    
    local CFLAGS="-arch arm64 -isysroot $SDK_PATH -mios-version-min=$IOS_MIN_VERSION"
    local LDFLAGS="-arch arm64 -isysroot $SDK_PATH"
    
    # Export compiler environment for libvpx
    export CC="$CC"
    export CXX="$CXX"
    export CFLAGS="$CFLAGS"
    export CXXFLAGS="$CFLAGS"
    export LDFLAGS="$LDFLAGS"
    export CROSS="$SDK_PATH"

    export AR="$(xcrun --sdk iphoneos --find ar)"
    export RANLIB="$(xcrun --sdk iphoneos --find ranlib)"
    export NM="$(xcrun --sdk iphoneos --find nm)"
    
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --target=arm64-darwin-gcc \
        --disable-examples \
        --disable-tools \
        --disable-docs \
        --disable-unit-tests \
        --enable-vp9-decoder \
        --disable-vp8-encoder \
        --disable-vp9-encoder \
        --enable-pic \
        --enable-static \
        --disable-shared
    
    make -j$(sysctl -n hw.ncpu)
    make install DESTDIR="$STAGE"
    
    echo "✅ libvpx device build complete: $STAGE"
}

# Build FFmpeg for iOS Device
build_ffmpeg_device() {
    echo "🔨 Building FFmpeg for iOS Device (arm64)..."
    
    cd "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION"
    
    local STAGE="$BUILD_DIR/ffmpeg-arm64"
    # Relative to the FFmpeg source dir, where configure and make run.
    local LIBVPX_DIR="../libvpx-arm64$INSTALL_PREFIX"
    # pkg-config must find the libvpx built above, never one installed on the host.
    export PKG_CONFIG_PATH=""
    export PKG_CONFIG_LIBDIR="$BUILD_DIR/libvpx-arm64$INSTALL_PREFIX/lib/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$BUILD_DIR/libvpx-arm64"
    
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    
    # Clean previous build
    make clean 2>/dev/null || true
    
    local SDK_PATH=$(xcrun --sdk iphoneos --show-sdk-path)
    local CC="$(xcrun --sdk iphoneos -f clang)"
    
    local CFLAGS="-arch arm64 -isysroot $SDK_PATH -mios-version-min=$IOS_MIN_VERSION"
    local LDFLAGS="-arch arm64 -isysroot $SDK_PATH"
    
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --enable-cross-compile \
        --target-os=darwin \
        --arch=arm64 \
        --cc="$CC" \
        --sysroot="$SDK_PATH" \
        --extra-cflags="$CFLAGS -I$LIBVPX_DIR/include" \
        --extra-ldflags="$LDFLAGS -L$LIBVPX_DIR/lib" \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-network \
        --disable-gpl \
        --disable-everything \
        --disable-autodetect \
        --enable-zlib \
        --enable-pthreads \
        --disable-avdevice \
        --disable-avfilter \
        --disable-swresample \
        --disable-swscale \
        --enable-demuxer=matroska,webm \
        --enable-decoder=vp9,libvpx_vp9 \
        --enable-parser=vp9 \
        --enable-protocol=file \
        --enable-libvpx \
        --enable-static \
        --disable-shared \
        --enable-pic \
        --disable-asm
    
    make -j$(sysctl -n hw.ncpu)
    make install DESTDIR="$STAGE"
    
    echo "✅ FFmpeg device build complete: $STAGE"
}

# Copy outputs to final directory
copy_outputs() {
    echo "📦 Copying libraries to output directory..."
    
    local FFMPEG_PREFIX="$BUILD_DIR/ffmpeg-arm64$INSTALL_PREFIX"
    local LIBVPX_PREFIX="$BUILD_DIR/libvpx-arm64$INSTALL_PREFIX"
    local COMMON_INCLUDE="$PROJECT_DIR/prebuilt/include"
    
    # Clear output lib dir
    rm -rf "$OUTPUT_DIR/lib"
    mkdir -p "$OUTPUT_DIR/lib"
    
    # Copy FFmpeg static libraries
    cp "$FFMPEG_PREFIX/lib/"*.a "$OUTPUT_DIR/lib/"
    
    # Copy libvpx static library
    cp "$LIBVPX_PREFIX/lib/"*.a "$OUTPUT_DIR/lib/"
    
    # Copy headers to common prebuilt/include (shared by all platforms)
    rm -rf "$COMMON_INCLUDE"
    mkdir -p "$COMMON_INCLUDE"
    cp -R "$FFMPEG_PREFIX/include/"* "$COMMON_INCLUDE/"
    cp -R "$LIBVPX_PREFIX/include/"* "$COMMON_INCLUDE/"
    
    echo "📋 Libraries built:"
    ls -la "$OUTPUT_DIR/lib/"
}

# Main build process
main() {
    echo "🚀 Starting iOS Device FFmpeg build..."
    echo ""
    
    download_ffmpeg
    download_libvpx
    
    build_libvpx_device
    build_ffmpeg_device
    
    copy_outputs
    
    echo ""
    echo "✅ FFmpeg iOS Device build complete!"
    echo "   Output: $OUTPUT_DIR"
    echo ""
    echo "📝 Libraries built for: arm64 (iOS Device)"
    echo "   Use prebuilt/ios-simulator/ for simulator builds"
}

main "$@"
