# react-native-webm-alpha-player

High-performance WebM VP9 video player with alpha transparency support for React Native (iOS, Android) and Web.

## Features

- 🎬 **VP9 Alpha Transparency** - Full support for WebM videos with alpha channel
- ⚡ **High Performance** - Native C++ core with GPU-accelerated rendering
- 📱 **iOS** - Metal-based rendering with YUVA→RGBA GPU shader
- 🤖 **Android** - TextureView with OpenGL ES 2.0, proper z-ordering and ScrollView support
- 🌐 **Web** - HTML5 video fallback for react-native-web compatibility
- 🔄 **Zero-Copy Frame Pipeline** - Efficient memory usage with LRU frame caching
- 🎯 **React Native Optimized** - Works correctly with ScrollView, proper clipping, and z-ordering

## Installation

```bash
npm install react-native-webm-alpha-player
# or
yarn add react-native-webm-alpha-player
```

### Expo / Managed Workflow

- **Expo Go:** Not supported (needs custom native modules and FFmpeg binaries).
- **Expo with custom dev client / EAS Build:** Supported. Run `expo prebuild` (or use EAS Build) so pods/Gradle deps are compiled in, then open the native projects or build a custom dev client.
- **Bare React Native:** Fully supported.

### iOS Setup

```bash
cd ios && pod install
```

### Android Setup

Android autolinking should handle everything. If you encounter issues, make sure your `android/app/build.gradle` has:

```gradle
android {
    defaultConfig {
        ndk {
            abiFilters 'armeabi-v7a', 'arm64-v8a', 'x86_64'
        }
    }
}
```

## Usage

```tsx
import { WebMAlphaView } from 'react-native-webm-alpha-player';

function MyComponent() {
  return (
    <WebMAlphaView
      source={{ uri: 'https://example.com/video.webm' }}
      autoPlay={true}
      loop={true}
      style={{ width: 300, height: 300 }}
    />
  );
}
```

### Props

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `source` | `{ uri: string }` | required | Video source URL or local file path |
| `autoPlay` | `boolean` | `true` | Auto-play video when loaded |
| `loop` | `boolean` | `true` | Loop video playback |
| `paused` | `boolean` | `false` | Pause video playback |
| `playbackSpeed` | `number` | `1.0` | Playback speed multiplier |
| `style` | `ViewStyle` | - | Container style |
| `onLoad` | `function` | - | Callback when video loads |
| `onError` | `function` | - | Callback when error occurs |

### Ref Methods

```tsx
import { WebMAlphaView, WebMAlphaViewRef } from 'react-native-webm-alpha-player';

const playerRef = useRef<WebMAlphaViewRef>(null);

// Control playback
playerRef.current?.play();
playerRef.current?.pause();
playerRef.current?.stop();
playerRef.current?.seekToFrame(30);
```

## Supported Source Types

- **HTTP/HTTPS URLs**: `{ uri: 'https://example.com/video.webm' }`
- **Local Files**: `{ uri: 'file:///path/to/video.webm' }`
- **Asset Bundle (iOS)**: `{ uri: 'asset://video.webm' }`
- **Android Assets**: `{ uri: 'asset://video.webm' }`

## Technical Details

### Architecture Overview

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   React Native  │    │   Native Bridge │    │   C++ Core      │
│   TypeScript    │◄──►│   iOS/Android   │◄──►│   WebMAlphaPlayer│
│   Component     │    │   ViewManager   │    │   (FFmpeg)      │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   GPU Rendering │    │   Metal/OpenGL  │    │   Multithreaded │
│   YUVA→RGBA     │    │   TextureView   │    │   Frame Cache   │
│   Triple Buffer │    │   MTKView       │    │   LRU Cache     │
└─────────────────┘    └─────────────────┘    └─────────────────┘
```

### Core Components

#### 🚀 **C++ Core (WebMAlphaPlayer)**
- **FFmpeg Integration**: Direct C API calls for maximum performance
- **VP9 Alpha Decoding**: libvpx-powered YUVA frame extraction
- **Multithreaded Architecture**: 
  - Main thread: Playback timing and frame requests
  - Decode thread: FFmpeg frame decoding
  - Render thread: GPU texture uploads
- **Frame Caching**: LRU cache prevents re-decoding during loops
- **Zero-Copy Pipeline**: Direct memory access from decoder to GPU

#### 📱 **iOS Implementation (Metal)**
- **WebMAlphaMetalPlayerView**: MTKView subclass with Metal renderer
- **YUVA→RGBA Shader**: GPU-accelerated color space conversion
- **Triple Buffering**: Prevents tearing during rapid frame updates
- **Memory Management**: Efficient texture reuse and cleanup

#### 🤖 **Android Implementation (OpenGL ES)**
- **WebMAlphaTextureView**: SurfaceTexture-based rendering
- **EGL Context**: Proper OpenGL ES 2.0 setup with z-ordering
- **ScrollView Compatible**: Uses TextureView instead of SurfaceView
- **Asset Loading**: Automatic bundling and caching

#### 🌐 **Web Fallback**
- **HTML5 Video**: Standard `<video>` element with alpha support
- **React Native Web**: Automatic platform detection and switching

### Performance Features

#### ⚡ **Multithreading & Concurrency**
- **Decode Thread**: Background FFmpeg processing
- **Playback Thread**: Precise timing control
- **Render Thread**: GPU texture uploads and display
- **Thread Safety**: Atomic operations and mutex protection

#### 🎨 **GPU-Accelerated Rendering**
- **YUVA Processing**: Hardware-accelerated color conversion
- **Triple Buffering**: Eliminates screen tearing
- **Texture Reuse**: Minimizes GPU memory allocations
- **Shader Optimization**: Platform-specific GPU shaders

#### 💾 **Memory Optimization**
- **Frame Caching**: LRU cache with configurable size (default: 250 frames)
- **Zero-Copy**: Direct buffer access from FFmpeg to GPU
- **Pre-decoding**: All frames loaded on startup for smooth playback
- **Memory Pool**: Reusable frame buffers

### FFmpeg Configuration

The library uses a minimal FFmpeg build with only VP9 decode support:
- FFmpeg 6.1 with libvpx 1.14.0
- Static libraries for minimal binary size
- Configured for decode-only operation
- Custom build scripts for all platforms

## Creating VP9 Alpha Videos

### FFmpeg Encoding Command

To create WebM VP9 videos with alpha transparency, use this FFmpeg command:

```bash
ffmpeg -i input_video.mp4 \
       -c:v libvpx-vp9 \
       -pix_fmt yuva420p \
       -crf 18 \
       -b:v 0 \
       -speed 0 \
       -auto-alt-ref 0 \
       output.webm
```

### Encoding Parameters Explained

| Parameter | Description |
|-----------|-------------|
| `-c:v libvpx-vp9` | VP9 video codec with alpha support |
| `-pix_fmt yuva420p` | YUVA pixel format (includes alpha channel) |
| `-crf 18` | Constant Rate Factor (18 = high quality, 0-63 range) |
| `-b:v 0` | Variable bitrate (required for alpha) |
| `-speed 0` | Slowest encoding (best quality) |
| `-auto-alt-ref 0` | Disable alt-ref frames (required for alpha) |

### Input Formats Supported

- **Video Files**: MP4, MOV, AVI, etc.
- **Image Sequences**: `frame_%04d.png` (numbered PNGs)
- **Animated Content**: GIF, APNG, etc.
- **Screen Recordings**: With alpha channel

### Quality Recommendations

- **CRF 18-24**: Good balance of quality/size
- **CRF 12-18**: High quality (larger files)
- **CRF 24-30**: Smaller files (lower quality)

### Example Commands

```bash
# From MP4 video
ffmpeg -i animation.mp4 -c:v libvpx-vp9 -pix_fmt yuva420p -crf 20 -b:v 0 output.webm

# From PNG sequence
ffmpeg -framerate 30 -i frame_%04d.png -c:v libvpx-vp9 -pix_fmt yuva420p -crf 18 -b:v 0 animation.webm

# From MOV with alpha
ffmpeg -i transparent_video.mov -c:v libvpx-vp9 -pix_fmt yuva420p -crf 15 -b:v 0 -speed 0 final.webm
```

### Testing Your Videos

After encoding, test with:
```bash
ffprobe -v quiet -print_format json -show_format -show_streams output.webm
```

Look for:
- `codec_name: "vp9"`
- `pix_fmt: "yuva420p"`
- Alpha channel presence

## Building FFmpeg (Development)

If you need to rebuild FFmpeg libraries:

```bash
# iOS (both device + simulator)
npm run build:ffmpeg:ios

# iOS device only (arm64)
npm run build:ffmpeg:ios:device

# iOS simulator only (arm64 Apple Silicon)
npm run build:ffmpeg:ios:simulator

# Android (all ABIs: arm64-v8a, armeabi-v7a, x86_64)
npm run build:ffmpeg:android

# All platforms
npm run build:ffmpeg:all
```

### FFmpeg Output Directories

| Platform | Output Directory | Architecture |
|----------|-----------------|-------------|
| iOS Device | `prebuilt/ios/` | arm64 |
| iOS Simulator | `prebuilt/ios-simulator/` | arm64 (Apple Silicon) |
| Android | `prebuilt/android/{abi}/` | arm64-v8a, armeabi-v7a, x86_64 |

## License

LGPL v2.1+

This library is licensed under LGPL v2.1+ due to its use of FFmpeg, which is licensed under LGPL v2.1+. This ensures compliance with FFmpeg's licensing requirements while allowing commercial use with proper attribution and linking flexibility.

## Licensing Considerations

### FFmpeg and LGPL

This library uses FFmpeg, which is licensed under **LGPL v2.1+**. Here's what this means for your usage:

#### FFmpeg License Details
- **FFmpeg**: LGPL v2.1 or later
- **libvpx (VP9)**: BSD-style license (permissive)
- **Library Linking**: Static linking creates a derivative work

#### LGPL Compliance Options

**Option 1: LGPL License (Recommended)**
If you distribute this library with statically linked FFmpeg, you should license your library under LGPL v2.1+ to comply with FFmpeg's license terms.

**Option 2: Dynamic Linking**
Provide FFmpeg as dynamically linked libraries, allowing users to replace them.

**Option 3: Object Files + Linking Instructions**
Distribute FFmpeg object files separately and provide linking instructions, allowing users to relink with modified FFmpeg versions.

#### Current Implementation
This library currently uses static FFmpeg linking for optimal performance and minimal binary size. If you plan to distribute this library commercially or in closed-source applications, consider the LGPL implications.

#### Why LGPL Matters
LGPL ensures that improvements to FFmpeg can be shared back with the community, while still allowing commercial use with proper attribution and linking flexibility.

## Credits

Built with:
- [FFmpeg](https://ffmpeg.org/) - Video decoding
- [libvpx](https://www.webmproject.org/code/) - VP9 codec
- [React Native](https://reactnative.dev/) - Cross-platform framework
