#!/bin/bash
# Build iOS Framework

set -e

echo "Building iOS WebM Alpha Player Framework..."

# Ensure we're in the ios directory
cd "$(dirname "$0")/../ios"

# Build FFmpeg first
echo "Building FFmpeg libraries..."
if [ ! -d "../prebuilt/ios" ]; then
    echo "FFmpeg libraries not found. Building..."
    cd ..
    npm run build:ffmpeg:ios
    cd ios
fi

# Create Xcode project if it doesn't exist
if [ ! -d "WebMAlphaPlayer.xcodeproj" ]; then
    echo "Creating Xcode project..."
    # This would need to be done manually or with a script
    echo "Please create an Xcode framework project manually"
    echo "Include the following files:"
    echo "- WebMAlphaPlayerBridge.h/.mm"
    echo "- WebMAlphaMetalPlayerView.h/.mm"
    echo "- C++ files from ../cpp/"
    echo "- FFmpeg libraries from ../prebuilt/ios/"
    exit 1
fi

# Build for device
echo "Building for iOS device..."
xcodebuild -project WebMAlphaPlayer.xcodeproj \
           -scheme WebMAlphaPlayer \
           -configuration Release \
           -sdk iphoneos \
           -arch arm64 \
           clean build

# Build for simulator
echo "Building for iOS simulator..."
xcodebuild -project WebMAlphaPlayer.xcodeproj \
           -scheme WebMAlphaPlayer \
           -configuration Release \
           -sdk iphonesimulator \
           -arch x86_64 \
           clean build

# Create universal framework
echo "Creating universal framework..."
FRAMEWORK_DIR="build/Release-iphoneos/WebMAlphaPlayer.framework"
SIMULATOR_DIR="build/Release-iphonesimulator/WebMAlphaPlayer.framework"
UNIVERSAL_DIR="../dist/ios/WebMAlphaPlayer.framework"

mkdir -p "../dist/ios"
cp -R "$FRAMEWORK_DIR" "$UNIVERSAL_DIR"

# Combine architectures
lipo -create \
     "$FRAMEWORK_DIR/WebMAlphaPlayer" \
     "$SIMULATOR_DIR/WebMAlphaPlayer" \
     -output "$UNIVERSAL_DIR/WebMAlphaPlayer"

echo "iOS Framework built successfully: ../dist/ios/WebMAlphaPlayer.framework"
echo ""
echo "To publish to CocoaPods:"
echo "1. Create podspec file"
echo "2. Push to CocoaPods trunk: pod trunk push WebMAlphaPlayer.podspec"
echo ""
echo "For local testing, copy the framework to your Xcode project"