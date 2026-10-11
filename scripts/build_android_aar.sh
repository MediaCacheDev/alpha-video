#!/bin/bash
# Build Android AAR library

set -e

echo "Building Android WebM Alpha Player AAR..."

# Ensure we're in the android directory
cd "$(dirname "$0")/../android"

# The module has no Gradle wrapper of its own
GRADLE=./gradlew
[ -x "$GRADLE" ] || GRADLE=gradle

# Build FFmpeg first
echo "Building FFmpeg libraries..."
if [ ! -d "../prebuilt/android" ]; then
    echo "FFmpeg libraries not found. Building..."
    cd ..
    npm run build:ffmpeg:android
    cd android
fi

# Clean previous build
"$GRADLE" clean

# Build release AAR
"$GRADLE" assembleRelease

# Copy AAR to output directory
mkdir -p ../dist/android
cp build/outputs/aar/*-release.aar ../dist/android/

echo "Android AAR built successfully in ../dist/android/"
echo ""
echo "To publish to Maven Central:"
echo "1. Sign the AAR with jarsigner"
echo "2. Upload to Sonatype OSSRH"
echo "3. Release through Sonatype"
echo ""
echo "For local testing, copy the AAR to your project's libs/ folder"