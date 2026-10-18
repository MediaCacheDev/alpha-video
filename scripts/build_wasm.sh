#!/bin/bash
#
# build_wasm.sh
# The WebAssembly modules of alpha-video-web (web/), each with its .wasm inlined so the npm
# package has no files to host:
#   web/src/wasm/demuxer.mjs      the core's demuxer (cpp/AlphaDemuxer, on libwebm's parser), reading
#                                 through JavaScript so files stream in by range, and the shared
#                                 colour conversion (cpp/YuvToRgb), for the WebCodecs and software
#                                 engines
#   web/src/wasm/software.mjs     FFmpeg's VP9 decoder and dav1d for AV1, single-threaded, run in
#                                 a Web Worker per stream by the software engine
#   web/src/wasm/software-mt.mjs  the same with threads (FFmpeg's frame threads, dav1d's own),
#                                 for cross-origin isolated pages, where SharedArrayBuffer exists
#
# Needs Emscripten, meson and ninja (brew install emscripten meson ninja).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

FFMPEG_VERSION="6.1"
DAV1D_VERSION="1.5.1"

BUILD_DIR="$SCRIPT_DIR/build/wasm"
OUT_DIR="$PROJECT_DIR/web/src/wasm"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

for tool in emcc em++ emconfigure emmake meson ninja pkg-config; do
    if ! command -v "$tool" >/dev/null; then
        echo "❌ $tool not found on PATH (brew install emscripten meson ninja pkg-config)"
        exit 1
    fi
done

mkdir -p "$BUILD_DIR"

download() {
    local name=$1 url=$2 android_copy=$3
    if [ -d "$BUILD_DIR/$name" ]; then return; fi
    # The Android build downloads the same releases; reuse them when they're there
    if [ -d "$SCRIPT_DIR/build/android/$android_copy" ]; then
        cp -R "$SCRIPT_DIR/build/android/$android_copy" "$BUILD_DIR/$name"
        return
    fi
    echo "📥 Downloading $name..."
    mkdir -p "$BUILD_DIR/$name"
    curl -L "$url" | tar xz --strip-components=1 -C "$BUILD_DIR/$name" 2>/dev/null \
        || curl -L "$url" | tar xJ --strip-components=1 -C "$BUILD_DIR/$name"
}

# build_dav1d <variant> [-pthread]: into stage-<variant>
build_dav1d() {
    local variant=$1 pthread=${2:-}
    local stage="$BUILD_DIR/stage-$variant"
    if [ -f "$stage/lib/libdav1d.a" ]; then return; fi
    echo "🔨 Building dav1d $DAV1D_VERSION for WebAssembly ($variant)..."
    local cross="$BUILD_DIR/$variant-emscripten.meson"
    local flags="'-Oz', '-flto'${pthread:+, '$pthread'}"
    cat > "$cross" <<EOF
[binaries]
c = 'emcc'
ar = 'emar'
ranlib = 'emranlib'

[built-in options]
c_args = [$flags]
c_link_args = [$flags]

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'
EOF
    rm -rf "$BUILD_DIR/$variant-build"
    # Meson's threads dependency always adds -pthread, which needs SharedArrayBuffer at run time;
    # the cross file adds it for the threaded variant only. Single-threaded, FFmpeg's wrapper is
    # given one thread, so dav1d starts none.
    sed -i '' "s/^    thread_dependency = dependency('threads')$/    thread_dependency = host_machine.system() == 'emscripten' ? declare_dependency() : dependency('threads')/" \
        "$BUILD_DIR/dav1d-$DAV1D_VERSION/meson.build"
    meson setup "$BUILD_DIR/$variant-build" "$BUILD_DIR/dav1d-$DAV1D_VERSION" \
        --cross-file "$cross" \
        --prefix="$stage" \
        --libdir=lib \
        --buildtype=release \
        --default-library=static \
        -Denable_asm=false \
        -Dbitdepths=8,16 \
        -Dlogging=false \
        -Denable_tools=false \
        -Denable_tests=false
    ninja -C "$BUILD_DIR/$variant-build"
    meson install -C "$BUILD_DIR/$variant-build" --no-rebuild
}

# build_ffmpeg <variant> <configure flags...>: out of tree in ffmpeg-<variant>, installed to stage-<variant>
build_ffmpeg() {
    local variant=$1
    shift
    local stage="$BUILD_DIR/stage-$variant"
    if [ -f "$stage/lib/libavcodec.a" ]; then return; fi
    echo "🔨 Building FFmpeg $FFMPEG_VERSION for WebAssembly ($variant)..."
    local build="$BUILD_DIR/ffmpeg-$variant"
    rm -rf "$build"
    mkdir -p "$build"
    cd "$build"
    # Without threads unless a variant enables them (later flags win), so pages need no
    # cross-origin isolation
    emconfigure "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION/configure" \
        --prefix="$stage" \
        --target-os=none \
        --arch=x86_32 \
        --enable-cross-compile \
        --cc=emcc \
        --cxx=em++ \
        --ar=emar \
        --ranlib=emranlib \
        --nm="$(command -v llvm-nm || echo emnm)" \
        --pkg-config=pkg-config \
        --extra-cflags="-Oz -flto" \
        --extra-ldflags="-flto" \
        --disable-asm \
        --disable-inline-asm \
        --disable-x86asm \
        --disable-stripping \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-network \
        --disable-gpl \
        --disable-everything \
        --disable-autodetect \
        --disable-iconv \
        --disable-pthreads \
        --disable-w32threads \
        --disable-os2threads \
        --disable-avdevice \
        --disable-avfilter \
        --disable-swresample \
        --disable-swscale \
        --enable-demuxer=matroska,webm \
        --enable-static \
        --disable-shared \
        "$@"
    emmake make -j"$JOBS"
    emmake make install
    cd "$PROJECT_DIR"
}

# link_module <output> <sources, libraries and flags...>
link_module() {
    local output=$1
    shift
    echo "🔨 Linking $output..."
    mkdir -p "$OUT_DIR"
    # web,worker only: the node environment adds import("node:module"), which webpack can't
    # resolve for the browser. Base64, not the raw-byte string, so bundlers pass it through intact.
    em++ -std=c++17 -Oz -flto \
        "$@" \
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createModule \
        -sSINGLE_FILE=1 -sSINGLE_FILE_BINARY_ENCODE=0 -sENVIRONMENT=web,worker \
        -sDYNAMIC_EXECUTION=0 \
        -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
        -sEXPORTED_FUNCTIONS=_malloc,_free \
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,HEAPF32,HEAPF64,wasmMemory \
        -o "$OUT_DIR/$output"
    echo "✅ $(du -h "$OUT_DIR/$output" | cut -f1) $OUT_DIR/$output"
}

download "ffmpeg-$FFMPEG_VERSION" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" "ffmpeg-$FFMPEG_VERSION"
download "dav1d-$DAV1D_VERSION" "https://github.com/videolan/dav1d/archive/refs/tags/$DAV1D_VERSION.tar.gz" "dav1d-$DAV1D_VERSION"
# An in-tree configure (the Android build's copy) blocks out-of-tree builds
if [ -f "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION/config.h" ] || [ -f "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION/ffbuild/config.mak" ]; then
    (cd "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION" && make distclean >/dev/null 2>&1 || true)
    rm -f "$BUILD_DIR/ffmpeg-$FFMPEG_VERSION/config.h"
fi

build_dav1d dav1d
build_dav1d dav1d-mt -pthread
# emconfigure points pkg-config at Emscripten's sysroot, adding EM_PKG_CONFIG_PATH
EM_PKG_CONFIG_PATH="$BUILD_DIR/stage-dav1d/lib/pkgconfig" \
    build_ffmpeg software --disable-avformat --enable-decoder=vp9,libdav1d --enable-libdav1d
EM_PKG_CONFIG_PATH="$BUILD_DIR/stage-dav1d-mt/lib/pkgconfig" \
    build_ffmpeg software-mt --disable-avformat --enable-decoder=vp9,libdav1d --enable-libdav1d \
    --enable-pthreads --extra-cflags=-pthread --extra-ldflags=-pthread

WEBM_PARSER="$PROJECT_DIR/third_party/libwebm/webm_parser"
link_module demuxer.mjs \
    -I"$PROJECT_DIR/cpp" -I"$WEBM_PARSER" -I"$WEBM_PARSER/include" \
    "$PROJECT_DIR/web/wasm/demuxer_bindings.cpp" "$PROJECT_DIR/cpp/AlphaDemuxer.cpp" \
    "$PROJECT_DIR/cpp/Av1SequenceHeader.cpp" "$PROJECT_DIR/cpp/YuvToRgb.cpp" "$WEBM_PARSER"/src/*.cc \
    -sMALLOC=emmalloc -sINITIAL_MEMORY=16777216

# software_module <output> <FFmpeg variant> <dav1d variant> <flags...>
software_module() {
    local output=$1 stage="$BUILD_DIR/stage-$2" dav1d="$BUILD_DIR/stage-$3"
    shift 3
    link_module "$output" \
        -I"$stage/include" "$PROJECT_DIR/web/wasm/decoder_bindings.cpp" \
        "$stage/lib/libavcodec.a" "$stage/lib/libavutil.a" "$dav1d/lib/libdav1d.a" \
        -sINITIAL_MEMORY=33554432 "$@"
}
software_module software.mjs software dav1d -sMALLOC=emmalloc
# The pool is created when the module starts, sized by its pthreadPoolSize option: a thread
# started later would wait for a Worker to load while its creator blocks. Running out is an
# error rather than a hang. Its Workers load mainScriptUrlOrBlob, the decode Worker's own blob:
# URL, as there is no software-mt.mjs file next to the code.
# Shared memory can't move, so browsers reserve its maximum up front; WebKit runs out of address
# space after a few at the default 2 GB.
software_module software-mt.mjs software-mt dav1d-mt \
    -pthread -sPTHREAD_POOL_SIZE=Module.pthreadPoolSize -sPTHREAD_POOL_SIZE_STRICT=2 \
    -sINCOMING_MODULE_JS_API=mainScriptUrlOrBlob -sMAXIMUM_MEMORY=1073741824
