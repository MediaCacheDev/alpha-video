#!/bin/bash
# Builds the codec modules' binary targets in Package.swift, one xcframework per codec library:
#   prebuilt/AlphaVideoLibvpx.xcframework   libvpx, for AlphaVideoVPX
#   prebuilt/AlphaVideoDav1d.xcframework    dav1d, for AlphaVideoAV1
# from the static libraries that build_codecs_ios.sh leaves in prebuilt/ios and
# prebuilt/ios-simulator.
#
# Also writes a .zip of each and prints its SwiftPM checksum. Those zips are the files to attach
# to the GitHub Release; Package.swift's checksums must match them byte for byte.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREBUILT="$ROOT/prebuilt"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# build_xcframework <name> <library> <header directory>
build_xcframework() {
    local name="$1" library="$2" headers="$3"
    local out="$PREBUILT/$name.xcframework"
    local args=()
    mkdir -p "$WORK/$name/include"
    cp -R "$PREBUILT/include/$headers" "$WORK/$name/include/"
    for slice in ios ios-simulator; do
        local lib="$PREBUILT/$slice/lib/lib$library.a"
        if [ ! -f "$lib" ]; then
            echo "Missing $lib: run scripts/build_codecs_ios.sh first" >&2
            exit 1
        fi
        args+=(-library "$lib" -headers "$WORK/$name/include")
    done

    rm -rf "$out"
    xcodebuild -create-xcframework "${args[@]}" -output "$out" >/dev/null

    (
        cd "$PREBUILT"
        # Fixed times, so rebuilding from the same libraries gives the same zip and checksum.
        find "$name.xcframework" -exec touch -h -t 202601010000 {} +
        rm -f "$name.xcframework.zip"
        find "$name.xcframework" | LC_ALL=C sort | zip -q -X -D -9 "$name.xcframework.zip" -@
        echo "Wrote $out and $out.zip"
        echo "  checksum: $(swift package compute-checksum "$name.xcframework.zip")"
    )
}

build_xcframework AlphaVideoLibvpx vpx vpx
build_xcframework AlphaVideoDav1d dav1d dav1d
