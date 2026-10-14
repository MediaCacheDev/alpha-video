#!/bin/bash
#
# build_ffmpeg_ios_simulator.sh
# Builds FFmpeg for iOS Simulator (universal: arm64 + x86_64)
# With VP9 (libvpx) and AV1 (dav1d, 8- and 10-bit) decoders for WebM alpha playback
# Needs meson and ninja for dav1d (brew install meson ninja), and nasm for x86_64
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FFMPEG_VERSION="6.1"
LIBVPX_VERSION="1.14.0"
DAV1D_VERSION="1.5.1"
BUILD_DIR="$SCRIPT_DIR/build/ios-simulator"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/ios-simulator"

# iOS deployment target
IOS_MIN_VERSION="13.0"

# Architectures to build for simulator
ARCHS=("arm64" "x86_64")

# FFmpeg and libvpx embed their configure lines in the libraries, so installs are staged
# with DESTDIR and libvpx is referenced by relative path; no local paths end up in them.
INSTALL_PREFIX="/usr/local"

# libvpx and dav1d install into one staging root per architecture, so that FFmpeg's
# pkg-config (PKG_CONFIG_SYSROOT_DIR) finds both.
deps_stage() {
    echo "$BUILD_DIR/deps-$1-sim"
}

echo "🔧 Building FFmpeg $FFMPEG_VERSION for iOS Simulator (arm64 + x86_64)"
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

# Build libvpx for a specific architecture
build_libvpx_arch() {
    local ARCH=$1
    echo "🔨 Building libvpx for iOS Simulator ($ARCH)..."
    
    cd "$BUILD_DIR/libvpx-$LIBVPX_VERSION"
    
    local STAGE="$(deps_stage "$ARCH")"
    
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
        --prefix="$INSTALL_PREFIX" \
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
    make install DESTDIR="$STAGE"
    
    echo "✅ libvpx $ARCH simulator build complete: $STAGE"
}

# Build dav1d for a specific architecture: the AV1 decoder, 8-bit and 10/12-bit
# (16bpc)
build_dav1d_arch() {
    local ARCH=$1
    echo "🔨 Building dav1d for iOS Simulator ($ARCH)..."
    
    local BUILD="$BUILD_DIR/dav1d-build-$ARCH-sim"
    local CROSS_FILE="$BUILD_DIR/dav1d-$ARCH-sim.meson"
    rm -rf "$BUILD"
    
    local SDK_PATH=$(xcrun --sdk iphonesimulator --show-sdk-path)
    local CC="$(xcrun --sdk iphonesimulator -f clang)"
    local CPU_FAMILY="$ARCH"
    [ "$ARCH" = "arm64" ] && CPU_FAMILY="aarch64"
    
    cat > "$CROSS_FILE" <<EOF
[binaries]
c = ['$CC', '-arch', '$ARCH', '-isysroot', '$SDK_PATH']
ar = '$(xcrun --sdk iphonesimulator --find ar)'
strip = '$(xcrun --sdk iphonesimulator --find strip)'

[built-in options]
c_args = ['-target', '$ARCH-apple-ios$IOS_MIN_VERSION-simulator']
c_link_args = ['-target', '$ARCH-apple-ios$IOS_MIN_VERSION-simulator']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'darwin'
subsystem = 'ios-simulator'
kernel = 'xnu'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU_FAMILY'
endian = 'little'
EOF
    
    # The flags libvpx exported are for the simulator; meson would apply them to the build
    # machine's compiler.
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
    DESTDIR="$(deps_stage "$ARCH")" meson install -C "$BUILD" --no-rebuild
    
    echo "✅ dav1d $ARCH simulator build complete"
}

# Build FFmpeg for a specific architecture
build_ffmpeg_arch() {
    local ARCH=$1
    echo "🔨 Building FFmpeg for iOS Simulator ($ARCH)..."
    
    cd "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION"
    
    local STAGE="$BUILD_DIR/ffmpeg-$ARCH-sim"
    # Relative to the FFmpeg source dir, where configure and make run.
    local DEPS_DIR="../deps-$ARCH-sim$INSTALL_PREFIX"
    # pkg-config must find the libvpx and dav1d built above, never ones installed on the host.
    export PKG_CONFIG_PATH=""
    export PKG_CONFIG_LIBDIR="$(deps_stage "$ARCH")$INSTALL_PREFIX/lib/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$(deps_stage "$ARCH")"
    
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    
    # Clean previous build
    make clean 2>/dev/null || true
    
    local SDK_PATH=$(xcrun --sdk iphonesimulator --show-sdk-path)
    local CC="$(xcrun --sdk iphonesimulator -f clang)"
    
    local CFLAGS="-arch $ARCH -isysroot $SDK_PATH -mios-simulator-version-min=$IOS_MIN_VERSION -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    local LDFLAGS="-arch $ARCH -isysroot $SDK_PATH -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --enable-cross-compile \
        --target-os=darwin \
        --arch=$ARCH \
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
    
    echo "✅ FFmpeg $ARCH simulator build complete: $STAGE"
}

# Create universal (fat) libraries using lipo
create_universal_libs() {
    echo "🔗 Creating universal (fat) libraries..."
    
    # Clear output lib dir
    rm -rf "$OUTPUT_DIR/lib"
    mkdir -p "$OUTPUT_DIR/lib"
    
    # Get list of libraries from first arch
    local FIRST_ARCH="${ARCHS[0]}"
    local FFMPEG_LIBS=$(ls "$BUILD_DIR/ffmpeg-$FIRST_ARCH-sim$INSTALL_PREFIX/lib/"*.a 2>/dev/null | xargs -n1 basename)
    local DEPS_LIBS=$(ls "$(deps_stage "$FIRST_ARCH")$INSTALL_PREFIX/lib/"*.a 2>/dev/null | xargs -n1 basename)
    
    # Create fat FFmpeg libraries
    for lib in $FFMPEG_LIBS; do
        local inputs=""
        for arch in "${ARCHS[@]}"; do
            inputs="$inputs $BUILD_DIR/ffmpeg-$arch-sim$INSTALL_PREFIX/lib/$lib"
        done
        echo "   Creating universal $lib..."
        lipo -create $inputs -output "$OUTPUT_DIR/lib/$lib"
    done
    
    # Create fat libvpx and dav1d libraries
    for lib in $DEPS_LIBS; do
        local inputs=""
        for arch in "${ARCHS[@]}"; do
            inputs="$inputs $(deps_stage "$arch")$INSTALL_PREFIX/lib/$lib"
        done
        echo "   Creating universal $lib..."
        lipo -create $inputs -output "$OUTPUT_DIR/lib/$lib"
    done
    
    # Copy headers (same for all archs)
    rm -rf "$OUTPUT_DIR/include"
    mkdir -p "$OUTPUT_DIR/include"
    cp -R "$BUILD_DIR/ffmpeg-$FIRST_ARCH-sim$INSTALL_PREFIX/include/"* "$OUTPUT_DIR/include/"
    cp -R "$(deps_stage "$FIRST_ARCH")$INSTALL_PREFIX/include/"* "$OUTPUT_DIR/include/"
    
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
    
    for tool in meson ninja; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "❌ $tool not found on PATH; dav1d needs it (brew install meson ninja)"
            exit 1
        fi
    done
    
    download_ffmpeg
    download_libvpx
    download_dav1d
    
    # Build each architecture
    for arch in "${ARCHS[@]}"; do
        echo ""
        echo "================================================"
        echo "Building for $arch..."
        echo "================================================"
        rm -rf "$(deps_stage "$arch")"
        build_libvpx_arch "$arch"
        build_dav1d_arch "$arch"
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
