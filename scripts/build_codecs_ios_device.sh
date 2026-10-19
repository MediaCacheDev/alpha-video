#!/bin/bash
#
# build_codecs_ios_device.sh
# Builds the decoders for iOS Device (arm64 only): libvpx for VP9 and dav1d for AV1 (8- and
# 10/12-bit), as static libraries in prebuilt/ios/lib
# Needs meson and ninja for dav1d (brew install meson ninja)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LIBVPX_VERSION="1.14.0"
DAV1D_VERSION="1.5.1"
BUILD_DIR="$SCRIPT_DIR/build/ios-device"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/ios"

# iOS deployment target
IOS_MIN_VERSION="13.0"

# libvpx embeds its configure line in the library, so installs are staged with DESTDIR; no
# local paths end up in it.
INSTALL_PREFIX="/usr/local"
# libvpx and dav1d install into one staging root
DEPS_STAGE="$BUILD_DIR/deps-arm64"

echo "🔧 Building libvpx $LIBVPX_VERSION and dav1d $DAV1D_VERSION for iOS Device (arm64)"
echo "   Build dir: $BUILD_DIR"
echo "   Output dir: $OUTPUT_DIR"

mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR/lib"
mkdir -p "$OUTPUT_DIR/include"

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
        --enable-vp8-decoder \
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

# Copy outputs to final directory
copy_outputs() {
    echo "📦 Copying libraries to output directory..."
    
    local DEPS_PREFIX="$DEPS_STAGE$INSTALL_PREFIX"
    local COMMON_INCLUDE="$PROJECT_DIR/prebuilt/include"
    
    # Clear output lib dir
    rm -rf "$OUTPUT_DIR/lib"
    mkdir -p "$OUTPUT_DIR/lib"
    
    # Copy libvpx and dav1d static libraries
    cp "$DEPS_PREFIX/lib/"*.a "$OUTPUT_DIR/lib/"
    
    # Copy headers to common prebuilt/include (shared by all platforms)
    rm -rf "$COMMON_INCLUDE"
    mkdir -p "$COMMON_INCLUDE"
    cp -R "$DEPS_PREFIX/include/"* "$COMMON_INCLUDE/"
    
    echo "📋 Libraries built:"
    ls -la "$OUTPUT_DIR/lib/"
}

# Main build process
main() {
    echo "🚀 Starting iOS Device libvpx + dav1d build..."
    echo ""
    
    for tool in meson ninja; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "❌ $tool not found on PATH; dav1d needs it (brew install meson ninja)"
            exit 1
        fi
    done
    
    download_libvpx
    download_dav1d
    
    rm -rf "$DEPS_STAGE"
    build_libvpx_device
    build_dav1d_device
    
    copy_outputs
    
    echo ""
    echo "✅ iOS Device libvpx + dav1d build complete!"
    echo "   Output: $OUTPUT_DIR"
    echo ""
    echo "📝 Libraries built for: arm64 (iOS Device)"
    echo "   Use prebuilt/ios-simulator/ for simulator builds"
}

main "$@"
