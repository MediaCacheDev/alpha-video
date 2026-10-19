#!/bin/bash
#
# build_codecs_android.sh
# Downloads and builds the decoders for Android (arm64-v8a, armeabi-v7a, x86_64): libvpx for VP9
# and dav1d for AV1 (8- and 10/12-bit), as static libraries in prebuilt/android/<abi>/lib.
# Needs meson and ninja for dav1d (brew install meson ninja), and nasm for x86_64
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LIBVPX_VERSION="1.14.0"
DAV1D_VERSION="1.5.1"
BUILD_DIR="$SCRIPT_DIR/build/android"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/android"

# Android settings
ANDROID_API=24
NDK_PATH="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.1.12297006}"

if [ ! -d "$NDK_PATH" ]; then
    echo "❌ Android NDK not found at $NDK_PATH"
    echo "   Set ANDROID_NDK_HOME or install NDK via Android Studio"
    exit 1
fi

TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"
# Tools are called by name and installs are staged with DESTDIR, because libvpx embeds its
# configure line in the library; that way no local paths end up in it.
export PATH="$TOOLCHAIN/bin:$PATH"
INSTALL_PREFIX="/usr/local"

echo "🔧 Building libvpx $LIBVPX_VERSION and dav1d $DAV1D_VERSION for Android"
echo "   NDK: $NDK_PATH"
echo "   Build dir: $BUILD_DIR"
echo "   Output dir: $OUTPUT_DIR"

mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

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

# libvpx and dav1d install into one staging root per ABI
deps_stage() {
    echo "$BUILD_DIR/deps-$1"
}

# Build libvpx for Android
build_libvpx_android() {
    local ABI=$1
    local ARCH=$2
    local TARGET=$3
    
    echo "🔨 Building libvpx for $ABI..."
    
    cd "$BUILD_DIR/libvpx-$LIBVPX_VERSION"
    
    local STAGE="$(deps_stage "$ABI")"
    
    make clean 2>/dev/null || true
    
    export CC="${TARGET}${ANDROID_API}-clang"
    export CXX="${TARGET}${ANDROID_API}-clang++"
    export AR="llvm-ar"
    export RANLIB="llvm-ranlib"
    export STRIP="llvm-strip"
    
    if [ "$ABI" = "arm64-v8a" ] || [ "$ABI" = "armeabi-v7a" ]; then
        export AS="${TARGET}${ANDROID_API}-clang"
        export ASFLAGS="-c"
    else
        unset AS
        unset ASFLAGS

        if command -v nasm >/dev/null 2>&1; then
            export NASM="$(command -v nasm)"
        elif command -v yasm >/dev/null 2>&1; then
            export YASM="$(command -v yasm)"
        else
            echo "⚠️  NASM/YASM not found on PATH; x86 builds may fail."
        fi

        if command -v readelf >/dev/null 2>&1; then
            export READELF="$(command -v readelf)"
        elif command -v greadelf >/dev/null 2>&1; then
            export READELF="$(command -v greadelf)"
        else
            export READELF="llvm-readelf"
        fi
    fi
    
    local VPX_TARGET=""
    case $ABI in
        arm64-v8a)
            VPX_TARGET="arm64-android-gcc"
            ;;
        armeabi-v7a)
            VPX_TARGET="armv7-android-gcc"
            ;;
        x86_64)
            VPX_TARGET="x86_64-android-gcc"
            ;;
    esac
    
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --target="$VPX_TARGET" \
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
        --disable-shared \
        --extra-cflags="-fPIC"
    
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    make install DESTDIR="$STAGE"
}

# Build dav1d for Android: the AV1 decoder, 8-bit and 10/12-bit (16bpc)
build_dav1d_android() {
    local ABI=$1
    local TARGET=$2
    local CPU_FAMILY=$3
    
    echo "🔨 Building dav1d for $ABI..."
    
    local BUILD="$BUILD_DIR/dav1d-build-$ABI"
    local CROSS_FILE="$BUILD_DIR/dav1d-$ABI.meson"
    rm -rf "$BUILD"
    
    cat > "$CROSS_FILE" <<EOF
[binaries]
c = '${TARGET}${ANDROID_API}-clang'
ar = 'llvm-ar'
strip = 'llvm-strip'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'android'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU_FAMILY'
endian = 'little'
EOF
    
    meson setup "$BUILD" "$BUILD_DIR/dav1d-$DAV1D_VERSION" \
        --cross-file "$CROSS_FILE" \
        --prefix="$INSTALL_PREFIX" \
        --libdir=lib \
        --buildtype=release \
        --default-library=static \
        -Db_pie=true \
        -Dbitdepths=8,16 \
        -Denable_tools=false \
        -Denable_tests=false
    ninja -C "$BUILD"
    DESTDIR="$(deps_stage "$ABI")" meson install -C "$BUILD" --no-rebuild
}

# Copy outputs for each ABI
package_output() {
    local ABI=$1
    
    echo "📦 Packaging $ABI..."
    
    local OUT="$OUTPUT_DIR/$ABI"
    local DEPS_DIR="$(deps_stage "$ABI")$INSTALL_PREFIX"
    rm -rf "$OUT/lib"
    mkdir -p "$OUT/lib"
    
    # Copy static archives (.a)
    cp "$DEPS_DIR/lib/"*.a "$OUT/lib/"
    
    # Copy headers to common prebuilt/include (same for all platforms)
    local COMMON_INCLUDE="$PROJECT_DIR/prebuilt/include"
    mkdir -p "$COMMON_INCLUDE"
    rm -rf "$COMMON_INCLUDE/vpx" "$COMMON_INCLUDE/dav1d"
    cp -R "$DEPS_DIR/include/"* "$COMMON_INCLUDE/"
}

# Main build process
main() {
    echo "🚀 Starting Android libvpx + dav1d build..."
    
    for tool in meson ninja; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "❌ $tool not found on PATH; dav1d needs it (brew install meson ninja)"
            exit 1
        fi
    done
    
    download_libvpx
    download_dav1d
    
    # Build for arm64-v8a (most common)
    rm -rf "$(deps_stage arm64-v8a)"
    build_libvpx_android "arm64-v8a" "aarch64" "aarch64-linux-android"
    build_dav1d_android "arm64-v8a" "aarch64-linux-android" "aarch64"
    package_output "arm64-v8a"
    
    # Build for armeabi-v7a (older devices)
    rm -rf "$(deps_stage armeabi-v7a)"
    build_libvpx_android "armeabi-v7a" "arm" "armv7a-linux-androideabi"
    build_dav1d_android "armeabi-v7a" "armv7a-linux-androideabi" "arm"
    package_output "armeabi-v7a"
    
    # Build for x86_64 (emulator)
    rm -rf "$(deps_stage x86_64)"
    build_libvpx_android "x86_64" "x86_64" "x86_64-linux-android"
    build_dav1d_android "x86_64" "x86_64-linux-android" "x86_64"
    package_output "x86_64"
    
    echo "✅ Android libvpx + dav1d build complete!"
    echo "   Output: $OUTPUT_DIR"
    ls -la "$OUTPUT_DIR"
}

main "$@"
