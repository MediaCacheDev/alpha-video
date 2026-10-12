# Publishing Standalone Libraries

This guide explains how to publish separate Android and iOS versions of the WebM Alpha Player.

## Overview

The React Native library can be split into standalone platform-specific versions:

- **Android**: AAR library for Gradle dependency management
- **iOS**: Framework for CocoaPods/Swift Package Manager

## Prerequisites

1. **FFmpeg Libraries**: Build platform-specific FFmpeg libraries first
   ```bash
   npm run build:ffmpeg:all
   ```

2. **Repository Setup**: Create separate repositories for each platform
   - `webm-alpha-player-android`
   - `webm-alpha-player-ios`

## Android AAR Publication

### Build the AAR

```bash
npm run build:android:aar
```

This creates: `dist/android/webmalphaplayer-release.aar`

### Publish to Maven Central

1. **Sign the AAR**:
   ```bash
   jarsigner -verbose -sigalg SHA256withRSA -digestalg SHA-256 \
     -keystore your-keystore.jks \
     dist/android/webmalphaplayer-release.aar your-alias
   ```

2. **Upload to Sonatype OSSRH**:
   - Create JIRA ticket at https://issues.sonatype.org
   - Upload AAR through Sonatype UI
   - Release through Sonatype

3. **Gradle Configuration** for publishing:
   ```gradle
   publishing {
       repositories {
           maven {
               name = "ossrh"
               url = "https://s01.oss.sonatype.org/service/local/staging/deploy/maven2/"
               credentials {
                   username = System.getenv("OSSRH_USERNAME")
                   password = System.getenv("OSSRH_PASSWORD")
               }
           }
       }
   }
   ```

### Local Testing

For testing before publishing:
```gradle
dependencies {
    implementation files('path/to/webmalphaplayer-release.aar')
}
```

## iOS Framework Publication

### Build the Framework

```bash
npm run build:ios:framework
```

This creates: `dist/ios/WebMAlphaPlayer.framework`

### Publish to CocoaPods

1. **Create Podspec**: Use the provided `WebMAlphaPlayer-iOS.podspec`

2. **Validate Podspec**:
   ```bash
   pod spec lint WebMAlphaPlayer-iOS.podspec
   ```

3. **Push to CocoaPods**:
   ```bash
   pod trunk push WebMAlphaPlayer-iOS.podspec
   ```

### Swift Package Manager

Create a `Package.swift` in the iOS repository:

```swift
// swift-tools-version:5.3
import PackageDescription

let package = Package(
    name: "WebMAlphaPlayer",
    platforms: [.iOS(.v12)],
    products: [
        .library(
            name: "WebMAlphaPlayer",
            targets: ["WebMAlphaPlayer"]
        )
    ],
    targets: [
        .binaryTarget(
            name: "WebMAlphaPlayer",
            url: "https://github.com/danielkpl2/webm-alpha-player-ios/releases/download/1.0.0/WebMAlphaPlayer.xcframework.zip",
            checksum: "framework-checksum"
        )
    ]
)
```

## Repository Structure

### Android Repository (`webm-alpha-player-android`)

```
webm-alpha-player-android/
├── README.md (Android-specific)
├── build.gradle
├── src/main/
│   ├── AndroidManifest.xml
│   ├── java/com/danielkpl2/webmalphaplayer/
│   └── cpp/
├── prebuilt/android/ (FFmpeg libs)
└── scripts/
```

### iOS Repository (`webm-alpha-player-ios`)

```
webm-alpha-player-ios/
├── README.md (iOS-specific)
├── WebMAlphaPlayer.xcodeproj
├── Sources/
│   └── WebMAlphaPlayer/
├── Libraries/ios/ (FFmpeg libs)
├── WebMAlphaPlayer-iOS.podspec
└── Package.swift
```

## API Differences

### Android (Standalone)

- Direct `WebMAlphaTextureView` usage
- Kotlin/Java API
- Gradle dependency management

### iOS (Standalone)

- `WebMAlphaMetalPlayerView` UIView subclass
- Swift/Obj-C API
- CocoaPods/SPM distribution

### React Native (Bridge)

- TypeScript API
- Cross-platform abstraction
- npm distribution

## Licensing Considerations

The player code is MIT. Every artifact that contains the FFmpeg binaries (LGPL-2.1-or-later) must ship `THIRD_PARTY_NOTICES.md` and `licenses/`.

## Build Scripts

- `npm run build:android:aar` - Build Android AAR
- `npm run build:ios:framework` - Build iOS Framework
- `npm run build:standalone:all` - Build both platforms

## Testing

### Android
```kotlin
// Unit test
@Test
fun testVideoLoading() {
    val playerView = WebMAlphaTextureView(context)
    playerView.loadFromAsset("test.webm")
    // Assert loading success
}
```

### iOS
```swift
// Unit test
func testVideoPlayback() {
    let playerView = WebMAlphaMetalPlayerView()
    let url = Bundle(for: type(of: self)).url(forResource: "test", withExtension: "webm")!
    playerView.load(from: url)
    // Assert playback starts
}
```

## Distribution Strategy

1. **React Native**: Primary distribution via npm
2. **Android**: Secondary via Maven Central
3. **iOS**: Secondary via CocoaPods + SPM

This allows developers to choose the most appropriate integration method for their project.