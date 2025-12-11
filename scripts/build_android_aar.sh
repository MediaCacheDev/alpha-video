#!/bin/bash
# Build Android AAR library

set -e

echo "Building Android WebM Alpha Player AAR..."

# Ensure we're in the android directory
cd "$(dirname "$0")/android"

# Build FFmpeg first
echo "Building FFmpeg libraries..."
if [ ! -d "../prebuilt/android" ]; then
    echo "FFmpeg libraries not found. Building..."
    cd ..
    npm run build:ffmpeg:android
    cd android
fi

# Clean previous build
./gradlew clean

# Build release AAR
./gradlew assembleRelease

# Copy AAR to output directory
mkdir -p ../dist/android
cp build/outputs/aar/webmalphaplayer-release.aar ../dist/android/

echo "Android AAR built successfully: ../dist/android/webmalphaplayer-release.aar"
echo ""
echo "To publish to Maven Central:"
echo "1. Sign the AAR with jarsigner"
echo "2. Upload to Sonatype OSSRH"
echo "3. Release through Sonatype"
echo ""
echo "For local testing, copy the AAR to your project's libs/ folder"