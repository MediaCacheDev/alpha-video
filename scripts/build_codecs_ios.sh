#!/bin/bash
#
# build_codecs_ios.sh
# Convenience script to build libvpx and dav1d for both iOS Device and Simulator
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "🚀 Building libvpx + dav1d for iOS (Device + Simulator)"
echo ""

# Build for device first
"$SCRIPT_DIR/build_codecs_ios_device.sh"

echo ""
echo "================================================"
echo ""

# Build for simulator
"$SCRIPT_DIR/build_codecs_ios_simulator.sh"

echo ""
echo "================================================"
echo "✅ All iOS builds complete!"
echo ""
echo "📁 Output directories:"
echo "   Device:    $PROJECT_DIR/prebuilt/ios/"
echo "   Simulator: $PROJECT_DIR/prebuilt/ios-simulator/"
