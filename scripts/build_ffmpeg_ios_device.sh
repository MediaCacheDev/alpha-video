#!/bin/bash
#
# build_ffmpeg_ios_device.sh
# Builds FFmpeg for iOS Device (arm64 only)
# With VP9 (libvpx) and AV1 (dav1d, 8- and 10-bit) decoders for WebM alpha playback
# Needs meson and ninja for dav1d (brew install meson ninja)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FFMPEG_VERSION="6.1"
LIBVPX_VERSION="1.14.0"
DAV1D_VERSION="1.5.1"
BUILD_DIR="$SCRIPT_DIR/build/ios-device"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/ios"

# iOS deployment target
IOS_MIN_VERSION="13.0"

# FFmpeg and libvpx embed their configure lines in the libraries, so installs are staged
# with DESTDIR and libvpx is referenced by relative path; no local paths end up in them.
INSTALL_PREFIX="/usr/local"
# libvpx and dav1d install into one staging root, so that FFmpeg's pkg-config
# (PKG_CONFIG_SYSROOT_DIR) finds both.
DEPS_STAGE="$BUILD_DIR/deps-arm64"

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

# Download dav1d source
download_dav1d() {
    if [ ! -d "$BUILD_DIR/dav1d-$DAV1D_VERSION" ]; then
        echo "📥 Downloading dav1d $DAV1D_VERSION..."
        cd "$BUILD_DIR"
        curl -L "https://github.com/videolan/dav1d/archive/refs/tags/$DAV1D_VERSION.tar.gz" -o dav1d.tar.gz
        tar xf dav1d.tar.gz
        rm dav1d.tar.gz
    fi
}

# Build libvpx for iOS Device
build_libvpx_device() {
    echo "🔨 Building libvpx for iOS Device (arm64)..."
    
    cd "$BUILD_DIR/libvpx-$LIBVPX_VERSION"
    
    local STAGE="$DEPS_STAGE"
    
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

# Build dav1d for iOS Device: the AV1 decoder, 8-bit and 10/12-bit (16bpc)
build_dav1d_device() {
    echo "🔨 Building dav1d for iOS Device (arm64)..."
    
    local BUILD="$BUILD_DIR/dav1d-build-arm64"
    local CROSS_FILE="$BUILD_DIR/dav1d-arm64.meson"
    rm -rf "$BUILD"
    
    local SDK_PATH=$(xcrun --sdk iphoneos --show-sdk-path)
    local CC="$(xcrun --sdk iphoneos -f clang)"
    
    cat > "$CROSS_FILE" <<EOF
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK_PATH']
ar = '$(xcrun --sdk iphoneos --find ar)'
strip = '$(xcrun --sdk iphoneos --find strip)'

[built-in options]
c_args = ['-mios-version-min=$IOS_MIN_VERSION']
c_link_args = ['-mios-version-min=$IOS_MIN_VERSION']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
    
    # The flags libvpx exported are for iOS; meson would apply them to the build machine's compiler.
    env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS \
    meson setup "$BUILD" "$BUILD_DIR/dav1d-$DAV1D_VERSION" \
        --cross-file "$CROSS_FILE" \
        --prefix="$INSTALL_PREFIX" \
        --libdir=lib \
        --buildtype=release \
        --default-library=static \
        -Dbitdepths=8,16 \
        -Denable_tools=false \
        -Denable_tests=false
    ninja -C "$BUILD"
    DESTDIR="$DEPS_STAGE" meson install -C "$BUILD" --no-rebuild
    
    echo "✅ dav1d device build complete: $DEPS_STAGE"
}

# Build FFmpeg for iOS Device
build_ffmpeg_device() {
    echo "🔨 Building FFmpeg for iOS Device (arm64)..."
    
    cd "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION"
    
    local STAGE="$BUILD_DIR/ffmpeg-arm64"
    # Relative to the FFmpeg source dir, where configure and make run.
    local DEPS_DIR="../deps-arm64$INSTALL_PREFIX"
    # pkg-config must find the libvpx and dav1d built above, never ones installed on the host.
    export PKG_CONFIG_PATH=""
    export PKG_CONFIG_LIBDIR="$DEPS_STAGE$INSTALL_PREFIX/lib/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$DEPS_STAGE"
    
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
        --extra-cflags="$CFLAGS -I$DEPS_DIR/include" \
        --extra-ldflags="$LDFLAGS -L$DEPS_DIR/lib" \
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
        --enable-decoder=vp9,libvpx_vp9,libdav1d \
        --enable-parser=vp9 \
        --enable-protocol=file \
        --enable-libvpx \
        --enable-libdav1d \
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
    local DEPS_PREFIX="$DEPS_STAGE$INSTALL_PREFIX"
    local COMMON_INCLUDE="$PROJECT_DIR/prebuilt/include"
    
    # Clear output lib dir
    rm -rf "$OUTPUT_DIR/lib"
    mkdir -p "$OUTPUT_DIR/lib"
    
    # Copy FFmpeg static libraries
    cp "$FFMPEG_PREFIX/lib/"*.a "$OUTPUT_DIR/lib/"
    
    # Copy libvpx and dav1d static libraries
    cp "$DEPS_PREFIX/lib/"*.a "$OUTPUT_DIR/lib/"
    
    # Copy headers to common prebuilt/include (shared by all platforms)
    rm -rf "$COMMON_INCLUDE"
    mkdir -p "$COMMON_INCLUDE"
    cp -R "$FFMPEG_PREFIX/include/"* "$COMMON_INCLUDE/"
    cp -R "$DEPS_PREFIX/include/"* "$COMMON_INCLUDE/"
    
    echo "📋 Libraries built:"
    ls -la "$OUTPUT_DIR/lib/"
}

# Main build process
main() {
    echo "🚀 Starting iOS Device FFmpeg build..."
    echo ""
    
    for tool in meson ninja; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "❌ $tool not found on PATH; dav1d needs it (brew install meson ninja)"
            exit 1
        fi
    done
    
    download_ffmpeg
    download_libvpx
    download_dav1d
    
    rm -rf "$DEPS_STAGE"
    build_libvpx_device
    build_dav1d_device
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
