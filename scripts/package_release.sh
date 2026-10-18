#!/bin/bash
#
# package_release.sh
#
# Collects the GitHub Release assets for v<version> (package.json) in dist/release-v<version>/:
#
#   prebuilt-libvpx<V>-dav1d<D>.tar.gz    prebuilt/ for Android and iOS, what fetch-prebuilt.sh unpacks
#   AlphaVideoCodecs.xcframework.zip      the Package.swift binary target, as built by build_xcframework.sh
#   alpha-video-<version>-ios.zip         the CocoaPods source (alpha-video.podspec): the iOS sources,
#                                         libwebm's parser, alpha-video-source's reader and
#                                         AlphaVideoCodecs.xcframework
#   SHA256SUMS
#
# Uploads nothing. Afterwards, fetch-prebuilt.sh's SHA256 must be the prebuilt archive's,
# alpha-video.podspec's sha256 the pod zip's, and Package.swift's checksum the xcframework zip's;
# the script stops or warns when they differ.
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBVPX_VERSION="1.14.0"
DAV1D_VERSION="1.5.1"

VERSION="$(node -p "require('$ROOT/package.json').version")"
OUT="$ROOT/dist/release-v$VERSION"
PREBUILT_ARCHIVE="prebuilt-libvpx$LIBVPX_VERSION-dav1d$DAV1D_VERSION.tar.gz"

sha256() {
    shasum -a 256 "$1" | cut -d' ' -f1
}

for dir in android ios/lib ios-simulator/lib include; do
    if [ ! -d "$ROOT/prebuilt/$dir" ]; then
        echo "❌ prebuilt/$dir is missing: run the build_codecs_*.sh scripts" >&2
        exit 1
    fi
done
ZIP="$ROOT/prebuilt/AlphaVideoCodecs.xcframework.zip"
if [ ! -f "$ZIP" ]; then
    echo "❌ $ZIP is missing: run scripts/build_xcframework.sh" >&2
    exit 1
fi
if ! grep -q "checksum: \"$(sha256 "$ZIP")\"" "$ROOT/Package.swift"; then
    echo "❌ Package.swift's checksum is not AlphaVideoCodecs.xcframework.zip's ($(sha256 "$ZIP"))" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

# The archive carries the notices, since it is a copy of the libraries on its own.
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

# The CocoaPods source: prebuilt/ isn't in git, so the pod downloads the sources and the codecs
# together. Reproducible in the same way, for alpha-video.podspec's sha256.
POD="$STAGE/pod"
mkdir -p "$POD/cpp" "$POD/ios/Sources" "$POD/third_party"
cp "$ROOT"/cpp/*.h "$ROOT"/cpp/*.cpp "$POD/cpp/"
rm "$POD/cpp/WebMAlphaPlayer_jni.cpp"
cp -R "$ROOT/ios/Sources/AlphaVideo" "$POD/ios/Sources/"
cp -R "$ROOT/third_party/libwebm" "$ROOT/third_party/alpha-video-source" "$POD/third_party/"
cp -R "$ROOT/prebuilt/AlphaVideoCodecs.xcframework" "$POD/"
cp -R "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/licenses" "$POD/"
find "$POD" -name .DS_Store -delete
find "$POD" -exec touch -h -t 202601010000 {} +
POD_ZIP="alpha-video-$VERSION-ios.zip"
(cd "$POD" && find . -mindepth 1 | sed 's#^\./##' | LC_ALL=C sort | zip -q -X -D -9 "$OUT/$POD_ZIP" -@)

cp "$ZIP" "$OUT/"

(cd "$OUT" && shasum -a 256 -- * > SHA256SUMS)
echo "📦 $OUT"
sed 's/^/   /' "$OUT/SHA256SUMS"

PREBUILT_SHA256="$(sha256 "$OUT/$PREBUILT_ARCHIVE")"
if ! grep -q "^SHA256=\"$PREBUILT_SHA256\"" "$ROOT/scripts/fetch-prebuilt.sh"; then
    echo "⚠️  Set SHA256=\"$PREBUILT_SHA256\" in scripts/fetch-prebuilt.sh before uploading"
fi
POD_SHA256="$(sha256 "$OUT/$POD_ZIP")"
if ! grep -q ":sha256 => '$POD_SHA256'" "$ROOT/alpha-video.podspec"; then
    echo "⚠️  Set :sha256 => '$POD_SHA256' in alpha-video.podspec before uploading"
fi
