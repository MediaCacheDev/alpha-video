#!/bin/bash
#
# build_ffmpeg_android.sh
# Downloads and builds FFmpeg for Android (arm64-v8a, armeabi-v7a, x86_64)
# With VP9 (libvpx) support for WebM alpha playback
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FFMPEG_VERSION="6.1"
LIBVPX_VERSION="1.14.0"
BUILD_DIR="$SCRIPT_DIR/build/android"
OUTPUT_DIR="$PROJECT_DIR/prebuilt/android"

# Android settings
ANDROID_API=24
NDK_PATH="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/26.1.10909125}"

if [ ! -d "$NDK_PATH" ]; then
    echo "❌ Android NDK not found at $NDK_PATH"
    echo "   Set ANDROID_NDK_HOME or install NDK via Android Studio"
    exit 1
fi

TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"

echo "🔧 Building FFmpeg $FFMPEG_VERSION for Android"
echo "   NDK: $NDK_PATH"
echo "   Build dir: $BUILD_DIR"
echo "   Output dir: $OUTPUT_DIR"

mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

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

# Build libvpx for Android
build_libvpx_android() {
    local ABI=$1
    local ARCH=$2
    local TARGET=$3
    
    echo "🔨 Building libvpx for $ABI..."
    
    cd "$BUILD_DIR/libvpx-$LIBVPX_VERSION"
    
    local PREFIX="$BUILD_DIR/libvpx-$ABI"
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"
    
    make clean 2>/dev/null || true
    
    export CC="$TOOLCHAIN/bin/${TARGET}${ANDROID_API}-clang"
    export CXX="$TOOLCHAIN/bin/${TARGET}${ANDROID_API}-clang++"
    export AR="$TOOLCHAIN/bin/llvm-ar"
    export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
    export STRIP="$TOOLCHAIN/bin/llvm-strip"

    if [ "$ABI" = "arm64-v8a" ] || [ "$ABI" = "armeabi-v7a" ]; then
        export AS="$TOOLCHAIN/bin/${TARGET}${ANDROID_API}-clang"
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
            echo "⚠️  readelf not found; install GNU binutils so libvpx can inspect NASM output."
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
        --prefix="$PREFIX" \
        --target="$VPX_TARGET" \
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
        --extra-cflags="-fPIC"
    
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    make install
}

# Build FFmpeg for Android
build_ffmpeg_android() {
    local ABI=$1
    local ARCH=$2
    local TARGET=$3
    local CROSS_PREFIX=$4
    
    echo "🔨 Building FFmpeg for $ABI..."
    
    cd "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION"
    
    local PREFIX="$BUILD_DIR/ffmpeg-$ABI"
    local LIBVPX_PREFIX="$BUILD_DIR/libvpx-$ABI"
    
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"
    
    make clean 2>/dev/null || true
    
    local CC="$TOOLCHAIN/bin/${TARGET}${ANDROID_API}-clang"
    local CXX="$TOOLCHAIN/bin/${TARGET}${ANDROID_API}-clang++"
    local SYSROOT="$TOOLCHAIN/sysroot"
    
    local EXTRA_CFLAGS="-fPIC -I$LIBVPX_PREFIX/include"
    local EXTRA_LDFLAGS="-L$LIBVPX_PREFIX/lib"
    
    # Architecture-specific flags
    case $ABI in
        arm64-v8a)
            EXTRA_CFLAGS="$EXTRA_CFLAGS"
            ;;
        armeabi-v7a)
            EXTRA_CFLAGS="$EXTRA_CFLAGS -mfloat-abi=softfp -mfpu=neon"
            ;;
    esac
    
    ./configure \
        --prefix="$PREFIX" \
        --enable-cross-compile \
        --cross-prefix="$TOOLCHAIN/bin/llvm-" \
        --target-os=android \
        --arch=$ARCH \
        --cc="$CC" \
        --cxx="$CXX" \
        --sysroot="$SYSROOT" \
        --extra-cflags="$EXTRA_CFLAGS" \
        --extra-ldflags="$EXTRA_LDFLAGS" \
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
        --enable-jni \
        --disable-asm
    
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    make install
}

# Copy outputs for each ABI
package_output() {
    local ABI=$1
    
    echo "📦 Packaging $ABI..."
    
    local OUT="$OUTPUT_DIR/$ABI"
    mkdir -p "$OUT/lib"
    
    # Copy static archives (.a)
    if ls "$BUILD_DIR/ffmpeg-$ABI/lib"/*.a >/dev/null 2>&1; then
        cp "$BUILD_DIR/ffmpeg-$ABI/lib/"*.a "$OUT/lib/"
    fi
    if ls "$BUILD_DIR/libvpx-$ABI/lib"/*.a >/dev/null 2>&1; then
        cp "$BUILD_DIR/libvpx-$ABI/lib/"*.a "$OUT/lib/"
    fi
    
    # Copy headers to common prebuilt/include (same for all platforms)
    local COMMON_INCLUDE="$PROJECT_DIR/prebuilt/include"
    if [ ! -d "$COMMON_INCLUDE/libavcodec" ]; then
        mkdir -p "$COMMON_INCLUDE"
        cp -R "$BUILD_DIR/ffmpeg-$ABI/include/"* "$COMMON_INCLUDE/"
        cp -R "$BUILD_DIR/libvpx-$ABI/include/"* "$COMMON_INCLUDE/"
    fi
}

# Main build process
main() {
    echo "🚀 Starting Android FFmpeg build..."
    
    download_ffmpeg
    download_libvpx
    
    # Build for arm64-v8a (most common)
    build_libvpx_android "arm64-v8a" "aarch64" "aarch64-linux-android"
    build_ffmpeg_android "arm64-v8a" "aarch64" "aarch64-linux-android" "aarch64-linux-android-"
    package_output "arm64-v8a"
    
    # Build for armeabi-v7a (older devices)
    build_libvpx_android "armeabi-v7a" "arm" "armv7a-linux-androideabi"
    build_ffmpeg_android "armeabi-v7a" "arm" "armv7a-linux-androideabi" "arm-linux-androideabi-"
    package_output "armeabi-v7a"
    
    # Build for x86_64 (emulator)
    build_libvpx_android "x86_64" "x86_64" "x86_64-linux-android"
    build_ffmpeg_android "x86_64" "x86_64" "x86_64-linux-android" "x86_64-linux-android-"
    package_output "x86_64"
    
    echo "✅ FFmpeg Android build complete!"
    echo "   Output: $OUTPUT_DIR"
    ls -la "$OUTPUT_DIR"
}

main "$@"
