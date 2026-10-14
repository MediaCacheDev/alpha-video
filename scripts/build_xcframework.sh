#!/bin/bash
# Builds prebuilt/FFmpegVPX.xcframework, the FFmpeg + libvpx binary target in Package.swift, from
# the static libraries that build_ffmpeg_ios.sh leaves in prebuilt/ios and prebuilt/ios-simulator.
# Each slice is one static library: avformat, avcodec, avutil, vpx and dav1d merged with libtool.
#
# Also writes prebuilt/FFmpegVPX.xcframework.zip and prints its SwiftPM checksum. That zip is the
# file to attach to the GitHub Release; Package.swift's checksum must match it byte for byte.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREBUILT="$ROOT/prebuilt"
OUT="$PREBUILT/FFmpegVPX.xcframework"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for slice in ios ios-simulator; do
    lib="$PREBUILT/$slice/lib"
    if [ ! -f "$lib/libavformat.a" ]; then
        echo "Missing $lib/libavformat.a: run scripts/build_ffmpeg_ios.sh first" >&2
        exit 1
    fi
    mkdir -p "$WORK/$slice"
    # Member names repeat across the FFmpeg libraries (utils.o, version.o); the linker resolves
    # through the symbol table, so libtool's "duplicate member name" warnings are harmless.
    libtool -static -no_warning_for_no_symbols -o "$WORK/$slice/libFFmpegVPX.a" \
        "$lib/libavformat.a" "$lib/libavcodec.a" "$lib/libavutil.a" "$lib/libvpx.a" "$lib/libdav1d.a"
done

rm -rf "$OUT"
xcodebuild -create-xcframework \
    -library "$WORK/ios/libFFmpegVPX.a" -headers "$PREBUILT/include" \
    -library "$WORK/ios-simulator/libFFmpegVPX.a" -headers "$PREBUILT/include" \
    -output "$OUT"

cd "$PREBUILT"
rm -f FFmpegVPX.xcframework.zip
zip -qry FFmpegVPX.xcframework.zip FFmpegVPX.xcframework
echo "Wrote $OUT and $PREBUILT/FFmpegVPX.xcframework.zip"
echo "checksum: $(swift package compute-checksum FFmpegVPX.xcframework.zip)"
