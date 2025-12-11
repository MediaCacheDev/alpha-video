#!/bin/bash
#
# build_ffmpeg_ios_simulator.sh
# Builds FFmpeg for iOS Simulator (universal: arm64 + x86_64)
# With VP9 (libvpx) support for WebM alpha playback
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FFMPEG_VERSION="6.1"
LIBVPX_VERSION="1.14.0"
BUILD_DIR="$SCRIPT_DIR/build/ios-simulator"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/ios-simulator"

# iOS deployment target
IOS_MIN_VERSION="13.0"

# Architectures to build for simulator
ARCHS=("arm64" "x86_64")

echo "�� Building FFmpeg $FFMPEG_VERSION for iOS Simulator (arm64 + x86_64)"
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

# Build libvpx for a specific architecture
build_libvpx_arch() {
    local ARCH=$1
    echo "🔨 Building libvpx for iOS Simulator ($ARCH)..."
    
    cd "$BUILD_DIR/libvpx-$LIBVPX_VERSION"
    
    local PREFIX="$BUILD_DIR/libvpx-$ARCH-sim"
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"
    
    # Clean previous build
    make clean 2>/dev/null || true
    
    local SDK_PATH=$(xcrun --sdk iphonesimulator --show-sdk-path)
    local CC="$(xcrun --sdk iphonesimulator -f clang)"
    local CXX="$(xcrun --sdk iphonesimulator -f clang++)"
    
    local CFLAGS="-arch $ARCH -isysroot $SDK_PATH -mios-simulator-version-min=$IOS_MIN_VERSION -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    local LDFLAGS="-arch $ARCH -isysroot $SDK_PATH -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    
    # Export compiler environment for libvpx
    export CC="$CC"
    export CXX="$CXX"
    export CFLAGS="$CFLAGS"
    export CXXFLAGS="$CFLAGS"
    export LDFLAGS="$LDFLAGS"
    export AR="$(xcrun --sdk iphonesimulator --find ar)"
    export RANLIB="$(xcrun --sdk iphonesimulator --find ranlib)"
    export NM="$(xcrun --sdk iphonesimulator --find nm)"
    
    # Use generic-gnu target for libvpx cross-compile to simulator
    ./configure \
        --prefix="$PREFIX" \
        --target=generic-gnu \
        --disable-examples \
        --disable-tools \
        --disable-docs \
        --disable-unit-tests \
        --enable-vp9-decoder \
        --disable-vp8-encoder \
        --disable-vp9-encoder \
        --enable-pic \
        --enable-static \
        --disable-shared \
        --extra-cflags="$CFLAGS" \
        --extra-cxxflags="$CFLAGS"
    
    make -j$(sysctl -n hw.ncpu)
    make install
    
    echo "✅ libvpx $ARCH simulator build complete: $PREFIX"
}

# Build FFmpeg for a specific architecture
build_ffmpeg_arch() {
    local ARCH=$1
    echo "🔨 Building FFmpeg for iOS Simulator ($ARCH)..."
    
    cd "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION"
    
    local PREFIX="$BUILD_DIR/ffmpeg-$ARCH-sim"
    local LIBVPX_PREFIX="$BUILD_DIR/libvpx-$ARCH-sim"
    
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"
    
    # Clean previous build
    make clean 2>/dev/null || true
    
    local SDK_PATH=$(xcrun --sdk iphonesimulator --show-sdk-path)
    local CC="$(xcrun --sdk iphonesimulator -f clang)"
    
    local CFLAGS="-arch $ARCH -isysroot $SDK_PATH -mios-simulator-version-min=$IOS_MIN_VERSION -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    local LDFLAGS="-arch $ARCH -isysroot $SDK_PATH -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    
    ./configure \
        --prefix="$PREFIX" \
        --enable-cross-compile \
        --target-os=darwin \
        --arch=$ARCH \
        --cc="$CC" \
        --sysroot="$SDK_PATH" \
        --extra-cflags="$CFLAGS -I$LIBVPX_PREFIX/include" \
        --extra-ldflags="$LDFLAGS -L$LIBVPX_PREFIX/lib" \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-network \
        --disable-gpl \
        --disable-everything \
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
    make install
    
    echo "✅ FFmpeg $ARCH simulator build complete: $PREFIX"
}

# Create universal (fat) libraries using lipo
create_universal_libs() {
    echo "🔗 Creating universal (fat) libraries..."
    
    # Clear output lib dir
    rm -rf "$OUTPUT_DIR/lib"
    mkdir -p "$OUTPUT_DIR/lib"
    
    # Get list of libraries from first arch
    local FIRST_ARCH="${ARCHS[0]}"
    local FFMPEG_LIBS=$(ls "$BUILD_DIR/ffmpeg-$FIRST_ARCH-sim/lib/"*.a 2>/dev/null | xargs -n1 basename)
    local VPX_LIBS=$(ls "$BUILD_DIR/libvpx-$FIRST_ARCH-sim/lib/"*.a 2>/dev/null | xargs -n1 basename)
    
    # Create fat FFmpeg libraries
    for lib in $FFMPEG_LIBS; do
        local inputs=""
        for arch in "${ARCHS[@]}"; do
            inputs="$inputs $BUILD_DIR/ffmpeg-$arch-sim/lib/$lib"
        done
        echo "   Creating universal $lib..."
        lipo -create $inputs -output "$OUTPUT_DIR/lib/$lib"
    done
    
    # Create fat libvpx library
    for lib in $VPX_LIBS; do
        local inputs=""
        for arch in "${ARCHS[@]}"; do
            inputs="$inputs $BUILD_DIR/libvpx-$arch-sim/lib/$lib"
        done
        echo "   Creating universal $lib..."
        lipo -create $inputs -output "$OUTPUT_DIR/lib/$lib"
    done
    
    # Copy headers (same for all archs)
    rm -rf "$OUTPUT_DIR/include"
    mkdir -p "$OUTPUT_DIR/include"
    cp -R "$BUILD_DIR/ffmpeg-$FIRST_ARCH-sim/include/"* "$OUTPUT_DIR/include/"
    cp -R "$BUILD_DIR/libvpx-$FIRST_ARCH-sim/include/"* "$OUTPUT_DIR/include/"
    
    echo "📋 Universal libraries built:"
    for lib in "$OUTPUT_DIR/lib/"*.a; do
        echo "   $(basename $lib): $(lipo -archs $lib)"
    done
}

# Sync headers to canonical prebuilt/include for Xcode compatibility
sync_common_includes() {
    local COMMON_INCLUDE_DIR="$PROJECT_DIR/prebuilt/include"
    echo "🔄 Syncing headers to canonical include dir: $COMMON_INCLUDE_DIR"
    mkdir -p "$COMMON_INCLUDE_DIR"
    if command -v rsync >/dev/null 2>&1; then
        rsync -a "$OUTPUT_DIR/include/" "$COMMON_INCLUDE_DIR/"
    else
        cp -R "$OUTPUT_DIR/include/." "$COMMON_INCLUDE_DIR/"
    fi
}

# Main build process
main() {
    echo "🚀 Starting iOS Simulator FFmpeg build (universal)..."
    echo ""
    
    download_ffmpeg
    download_libvpx
    
    # Build each architecture
    for arch in "${ARCHS[@]}"; do
        echo ""
        echo "================================================"
        echo "Building for $arch..."
        echo "================================================"
        build_libvpx_arch "$arch"
        build_ffmpeg_arch "$arch"
    done
    
    # Create universal libraries
    echo ""
    echo "================================================"
    create_universal_libs
    sync_common_includes
    
    echo ""
    echo "✅ FFmpeg iOS Simulator build complete!"
    echo "   Output: $OUTPUT_DIR"
    echo ""
    echo "📝 Universal libraries built for: ${ARCHS[*]}"
    echo "   Supports both Apple Silicon and Intel Macs"
}

main "$@"
