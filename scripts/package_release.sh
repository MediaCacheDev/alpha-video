#!/bin/bash
#
# package_release.sh
#
# Collects the GitHub Release assets for v<version> (package.json) in dist/release-v<version>/:
#
#   prebuilt-ffmpeg<F>-libvpx<V>.tar.gz   prebuilt/ for Android and iOS, what fetch-prebuilt.sh unpacks
#   FFmpegVPX.xcframework.zip             the Package.swift binary target, as built by build_xcframework.sh
#   ffmpeg-<F>.tar.xz, libvpx-<V>.tar.gz  the corresponding source (LGPL), checked against
#                                         THIRD_PARTY_NOTICES.md
#   SHA256SUMS
#
# Uploads nothing. Afterwards, fetch-prebuilt.sh's SHA256 must be the prebuilt archive's, and
# Package.swift's checksum the zip's; the script stops or warns when they differ.
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FFMPEG_VERSION="6.1"
LIBVPX_VERSION="1.14.0"
FFMPEG_SHA256="488c76e57dd9b3bee901f71d5c95eaf1db4a5a31fe46a28654e837144207c270"
LIBVPX_SHA256="5f21d2db27071c8a46f1725928a10227ae45c5cd1cad3727e4aafbe476e321fa"

VERSION="$(node -p "require('$ROOT/package.json').version")"
OUT="$ROOT/dist/release-v$VERSION"
CACHE="$ROOT/dist/sources"
PREBUILT_ARCHIVE="prebuilt-ffmpeg$FFMPEG_VERSION-libvpx$LIBVPX_VERSION.tar.gz"

sha256() {
    shasum -a 256 "$1" | cut -d' ' -f1
}

fetch_source() {
    local name="$1" url="$2" expected="$3"
    if [ ! -f "$CACHE/$name" ]; then
        mkdir -p "$CACHE"
        curl -fL --retry 3 -o "$CACHE/$name.part" "$url"
        mv "$CACHE/$name.part" "$CACHE/$name"
    fi
    if [ "$(sha256 "$CACHE/$name")" != "$expected" ]; then
        echo "❌ $name does not match THIRD_PARTY_NOTICES.md ($CACHE/$name)" >&2
        exit 1
    fi
    cp "$CACHE/$name" "$OUT/$name"
}

for dir in android ios/lib ios-simulator/lib include; do
    if [ ! -d "$ROOT/prebuilt/$dir" ]; then
        echo "❌ prebuilt/$dir is missing: run the build_ffmpeg_*.sh scripts" >&2
        exit 1
    fi
done
ZIP="$ROOT/prebuilt/FFmpegVPX.xcframework.zip"
if [ ! -f "$ZIP" ]; then
    echo "❌ $ZIP is missing: run scripts/build_xcframework.sh" >&2
    exit 1
fi
if ! grep -q "checksum: \"$(sha256 "$ZIP")\"" "$ROOT/Package.swift"; then
    echo "❌ Package.swift's checksum is not FFmpegVPX.xcframework.zip's ($(sha256 "$ZIP"))" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

# The archive carries the notices, since it is a copy of LGPL binaries on its own.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/prebuilt/ios" "$STAGE/prebuilt/ios-simulator"
cp -R "$ROOT/prebuilt/android" "$ROOT/prebuilt/include" "$STAGE/prebuilt/"
cp -R "$ROOT/prebuilt/ios/lib" "$STAGE/prebuilt/ios/"
cp -R "$ROOT/prebuilt/ios-simulator/lib" "$STAGE/prebuilt/ios-simulator/"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/prebuilt/"
cp -R "$ROOT/licenses" "$STAGE/prebuilt/"
find "$STAGE" -name .DS_Store -delete
# Reproducible, so packaging the same libraries again keeps fetch-prebuilt.sh's checksum valid:
# fixed times, sorted entries, no owners or macOS metadata, no gzip timestamp.
find "$STAGE/prebuilt" -exec touch -h -t 202601010000 {} +
(cd "$STAGE" && find prebuilt | LC_ALL=C sort) \
    | COPYFILE_DISABLE=1 tar -c -n -T - -C "$STAGE" --no-mac-metadata \
        --uid 0 --gid 0 --uname "" --gname "" -f - \
    | gzip -n -9 > "$OUT/$PREBUILT_ARCHIVE"

cp "$ZIP" "$OUT/"
fetch_source "ffmpeg-$FFMPEG_VERSION.tar.xz" \
    "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" "$FFMPEG_SHA256"
fetch_source "libvpx-$LIBVPX_VERSION.tar.gz" \
    "https://github.com/webmproject/libvpx/archive/refs/tags/v$LIBVPX_VERSION.tar.gz" "$LIBVPX_SHA256"

(cd "$OUT" && shasum -a 256 -- * > SHA256SUMS)
echo "📦 $OUT"
sed 's/^/   /' "$OUT/SHA256SUMS"

PREBUILT_SHA256="$(sha256 "$OUT/$PREBUILT_ARCHIVE")"
if ! grep -q "^SHA256=\"$PREBUILT_SHA256\"" "$ROOT/scripts/fetch-prebuilt.sh"; then
    echo "⚠️  Set SHA256=\"$PREBUILT_SHA256\" in scripts/fetch-prebuilt.sh before uploading"
fi
