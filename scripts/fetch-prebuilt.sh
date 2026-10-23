#!/bin/bash
#
# fetch-prebuilt.sh [--force]
#
# Downloads the prebuilt libvpx + dav1d static libraries into prebuilt/ from this repo's GitHub
# Release. A git clone needs them to build; the npm package already contains them. To build them
# from source instead, run build_codecs_android.sh and build_codecs_ios.sh.
#
# Leaves an existing prebuilt/ alone unless --force is given.
#

set -euo pipefail

RELEASE="v1.0.0"
ARCHIVE="prebuilt-libvpx1.14.0-dav1d1.5.1.tar.gz"
SHA256="b95e75aed9a75c41f92388a31cb3e1e311ab44f81d02140c4fbf5e570865195e"
URL="https://github.com/MediaCacheDev/alpha-video/releases/download/$RELEASE/$ARCHIVE"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREBUILT="$ROOT/prebuilt"

sha256() {
    if command -v sha256sum > /dev/null; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

if [ "${1:-}" != "--force" ] && [ -d "$PREBUILT/android" ] && [ -d "$PREBUILT/ios/lib" ]; then
    echo "prebuilt/ is already there (--force replaces it)"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "⬇️  $URL"
curl -fL --retry 3 -o "$TMP/$ARCHIVE" "$URL"
ACTUAL="$(sha256 "$TMP/$ARCHIVE")"
if [ "$ACTUAL" != "$SHA256" ]; then
    echo "❌ Checksum mismatch for $ARCHIVE: expected $SHA256, got $ACTUAL" >&2
    exit 1
fi

rm -rf "$PREBUILT/android" "$PREBUILT/ios" "$PREBUILT/ios-simulator" "$PREBUILT/include"
tar -xzf "$TMP/$ARCHIVE" -C "$ROOT"
echo "✅ prebuilt/ (libvpx 1.14.0, dav1d 1.5.1)"
