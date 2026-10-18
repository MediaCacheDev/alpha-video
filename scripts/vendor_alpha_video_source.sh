#!/bin/bash
# Copies the native range reader and stores from an alpha-video-source checkout into
# third_party/alpha-video-source, and records its commit there.
#
#   scripts/vendor_alpha_video_source.sh [path to alpha-video-source, default ../alpha-video-source]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE_DIR="$(cd "${1:-$PROJECT_DIR/../alpha-video-source}" && pwd)"
DEST="$PROJECT_DIR/third_party/alpha-video-source"

COMMIT="$(git -C "$SOURCE_DIR" rev-parse HEAD)"
if [ -n "$(git -C "$SOURCE_DIR" status --porcelain -- native/src)" ]; then
    COMMIT="$COMMIT, with uncommitted changes"
fi

rm -rf "$DEST/src"
mkdir -p "$DEST/src"
cp "$SOURCE_DIR"/native/src/*.h "$SOURCE_DIR"/native/src/*.cpp "$DEST/src/"
cp "$SOURCE_DIR/LICENSE" "$DEST/LICENSE"

cat > "$DEST/README.md" <<EOF
# alpha-video-source (native)

\`src/\` is \`native/src\` from [alpha-video-source](https://github.com/MediaCacheDev/alpha-video-source), unmodified, at commit \`$COMMIT\`. MIT, by the same author as this repository.

It reads a file by range from an upstream (the platform's HTTP fetcher or an app's data source), through blocks in memory and an optional store on disk or in memory. The players' \`cpp/ByteSource\` reads through it for URLs and data sources. Its sources compile with the core's, with \`src/\` on the include path.

To update, run \`scripts/vendor_alpha_video_source.sh\` against a newer checkout. Its tests live in that repository (\`native/test\`).
EOF
echo "Vendored alpha-video-source native sources at $COMMIT"
