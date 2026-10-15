#!/bin/bash
# Builds prebuilt/AlphaVideoCodecs.xcframework, the libvpx + dav1d binary target in Package.swift,
# from the static libraries that build_codecs_ios.sh leaves in prebuilt/ios and
# prebuilt/ios-simulator. Each slice is one static library: vpx and dav1d merged with libtool.
#
# Also writes prebuilt/AlphaVideoCodecs.xcframework.zip and prints its SwiftPM checksum. That zip
# is the file to attach to the GitHub Release; Package.swift's checksum must match it byte for byte.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREBUILT="$ROOT/prebuilt"
OUT="$PREBUILT/AlphaVideoCodecs.xcframework"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for slice in ios ios-simulator; do
    lib="$PREBUILT/$slice/lib"
    for name in vpx dav1d; do
        if [ ! -f "$lib/lib$name.a" ]; then
            echo "Missing $lib/lib$name.a: run scripts/build_codecs_ios.sh first" >&2
            exit 1
        fi
    done
    mkdir -p "$WORK/$slice"
    ZERO_AR_DATE=1 libtool -static -no_warning_for_no_symbols -o "$WORK/$slice/libAlphaVideoCodecs.a" \
        "$lib/libvpx.a" "$lib/libdav1d.a"
done

mkdir -p "$WORK/include"
cp -R "$PREBUILT/include/vpx" "$PREBUILT/include/dav1d" "$WORK/include/"

rm -rf "$OUT"
xcodebuild -create-xcframework \
    -library "$WORK/ios/libAlphaVideoCodecs.a" -headers "$WORK/include" \
    -library "$WORK/ios-simulator/libAlphaVideoCodecs.a" -headers "$WORK/include" \
    -output "$OUT"

cd "$PREBUILT"
# Fixed times, so rebuilding from the same libraries gives the same zip and checksum.
find AlphaVideoCodecs.xcframework -exec touch -h -t 202601010000 {} +
rm -f AlphaVideoCodecs.xcframework.zip
find AlphaVideoCodecs.xcframework | LC_ALL=C sort | zip -q -X -D -9 AlphaVideoCodecs.xcframework.zip -@
echo "Wrote $OUT and $PREBUILT/AlphaVideoCodecs.xcframework.zip"
echo "checksum: $(swift package compute-checksum AlphaVideoCodecs.xcframework.zip)"
