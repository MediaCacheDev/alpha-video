# react-native-webm-alpha-player

High-performance WebM video player with alpha transparency support for React Native (iOS, Android) and Web. Supports both **software-decoded VP9** (single-stream YUVA) and **hardware-accelerated dual-stream VP9/AV1** (color + alpha in separate video tracks decoded via MediaCodec).

## Features

- **VP9 Alpha Transparency** - Full support for WebM videos with embedded YUVA alpha channel
- **Dual-Stream Hardware Decode** - Color + alpha as separate VP9 or AV1 streams, decoded via Android MediaCodec with zero CPU involvement
- **AV1 Support** - Dual-stream AV1 hardware decode on devices with AV1 MediaCodec support
- **High Performance** - Native C++ core (FFmpeg) with GPU-accelerated rendering
- **iOS** - Metal-based rendering with YUVA-to-RGBA GPU shader
- **Android** - TextureView with OpenGL ES 2.0, proper z-ordering and ScrollView support
- **Web** - HTML5 video fallback for react-native-web compatibility
- **Zero-Copy Frame Pipeline** - Efficient memory usage with LRU frame caching (software path) and OES texture compositing (hardware path)

## Decision Matrix

Choosing the right encoding format depends on your use case:

| | Single-Stream YUVA (Software) | Dual-Stream VP9/AV1 (Hardware) |
|---|---|---|
| **Decoding** | CPU (FFmpeg libvpx) | GPU (MediaCodec) |
| **CPU usage during playback** | Moderate (decode + render) | Near-zero (decode offloaded) |
| **Looping** | Seamless | Tiny stall at loop boundary (MediaCodec flush) |
| **Max resolution** | ~720p practical limit | 1080p+ (hardware limited) |
| **Best for** | Short loops, small animations, many simultaneous players | Long-duration, high-res, single-instance playback |
| **iOS support** | Yes (Metal) | No (Android only) |
| **Codec** | VP9 only | VP9 or AV1 |
| **Instances** | Unlimited (CPU bound) | Limited by MediaCodec slots (typically 8-16 per codec) |

**Rule of thumb**: Use single-stream YUVA for short looping effects (glows, particles, badges). Use dual-stream for hero videos, full-screen overlays, or anything over 720p / 10 seconds.

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

The player automatically detects whether a file contains a single YUVA stream (software decode) or dual video streams (hardware decode via MediaCodec). No prop changes are needed.

### Props

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `source` | `{ uri: string }` | required | Video source URL or local file path |
| `autoPlay` | `boolean` | `true` | Auto-play video when loaded |
| `loop` | `boolean` | `true` | Loop video playback |
| `paused` | `boolean` | `false` | Pause video playback |
| `playbackSpeed` | `number` | `1.0` | Playback speed multiplier. Changeable while playing, and does **not** move the playhead — see below |
| `style` | `ViewStyle` | - | Container style |
| `onLoad` | `function` | - | Callback when video loads (`{ totalFrames, width, height }`) |
| `onError` | `function` | - | Callback when error occurs (`{ error }`) |
| `mode` | `'preload' \| 'stream' \| 'streamAndPreload'` | `'preload'` | Playback mode (software path only, see below) |
| `streamCacheSize` | `number` | `30` | Frames to keep in LRU cache when streaming |
| `streamPrefetchAhead` | `number` | `15` | Frames to decode ahead of playback position |
| `useDecodeQueue` | `boolean` | `true` | Serialize decoding across all players (prevents CPU saturation) |
| `verbose` | `boolean` | `false` | Enable per-frame debug logging |

### Playback speed

`playbackSpeed` can change at any time, including mid-playback, and only affects how long each
frame is shown. The playhead stays exactly where it was:

```tsx
// Speeds the animation up from wherever it currently is. No seek, no restart.
<WebMAlphaView source={source} playbackSpeed={hovered ? 2 : 1} />
```

That distinction matters for anything driven by interaction. A rate change that also reset the
position would read as a glitch rather than as an acceleration.

Values at or below zero are clamped to `0.01` rather than stopping playback — use `paused` to
stop.

### Glow buttons

A button that plays a multi-state glow sheet (default, hover and press stacked as rows in one
file) lives in its own package, [alpha-glow-button](https://github.com/danielkpl2/alpha-glow-button).
Its `alpha-glow-button/alpha-video` entry point uses this player, and relies on the live
`playbackSpeed` above to change speed per state without restarting the loop.

### Ref Methods

```tsx
import { WebMAlphaView, WebMAlphaViewRef } from 'react-native-webm-alpha-player';

const playerRef = useRef<WebMAlphaViewRef>(null);

playerRef.current?.play();
playerRef.current?.pause();
playerRef.current?.stop();
playerRef.current?.seekToFrame(30);
```

### Decoder Status API (Android)

Query hardware decoder capabilities at runtime:

```tsx
import { getDecoderStatus, getDecoderInfo } from 'react-native-webm-alpha-player';

const status = await getDecoderStatus();
console.log(`VP9 HW: ${status.vp9HardwareAvailable}`);
console.log(`AV1 HW: ${status.av1HardwareAvailable}`);
console.log(`Max dual-stream players: ${status.maxDualStreamPlayers}`);
console.log(`Can create more: ${status.canCreateMore}`);

// Detailed string dump of all codec capabilities
const info = await getDecoderInfo();
console.log(info);
```

## Supported Source Types

- **HTTP/HTTPS URLs**: `{ uri: 'https://example.com/video.webm' }`. Downloaded once into the app's cache directory and played from there. The file is named by `cacheKey` if you give one (it may include subfolders), otherwise by a hash of the URL.
- **Local files**: `{ uri: 'file:///path/to/video.webm' }` or an absolute path.
- **Bundled files**: `{ uri: 'asset://videos/glow.webm' }`. On Android this is `android/app/src/main/assets/videos/glow.webm`. On iOS it is `videos/glow.webm` inside the app bundle.
- **Android `content://` URIs**, for example from a document picker.
- **`require()`**: pass `Image.resolveAssetSource(require('./glow.webm')).uri`. In debug builds this is a Metro `http://localhost:8081` URL. In release builds it is a file path on iOS and a `res/raw` resource name on Android.

Nothing is copied. Android reads `asset://` files and `res/raw` resources directly from the APK, so they must be stored uncompressed. `.webm` and `.mkv` are uncompressed by default. For other extensions, add them to `android.androidResources.noCompress` in the app's `build.gradle`.

## Architecture Overview

```
┌──────────────┐    ┌───────────────┐    ┌───────────────────────────────────┐
│  React Native│    │ Native Bridge │    │         Decode Backends           │
│  TypeScript  │◄──►│ iOS / Android │◄──►│                                   │
│  Component   │    │ ViewManager   │    │  ┌─────────────┐ ┌─────────────┐ │
└──────────────┘    └───────────────┘    │  │  Software    │ │  Hardware   │ │
                                         │  │  (C++/FFmpeg)│ │  (MediaCodec│ │
                                         │  │  YUVA decode │ │  OES texture│ │
                                         │  │  + GPU upload│ │  compositing│ │
                                         │  └─────────────┘ └─────────────┘ │
                                         └───────────────────────────────────┘
```

### Decode Paths

**Software (single-stream YUVA)** — works on all platforms:
1. FFmpeg demuxes WebM, libvpx decodes VP9 with embedded alpha
2. YUVA420P frames cached in LRU `FrameCache`
3. Uploaded to GPU as 4 textures (Y, U, V, A) per frame
4. Fragment shader converts YUVA to RGBA on Metal (iOS) / OpenGL ES (Android)

**Hardware (dual-stream VP9/AV1)** — Android only:
1. FFmpeg demuxes WebM into color packets (stream 0) and alpha packets (stream 1)
2. Two `MediaCodec` instances decode in parallel, outputting to `SurfaceTexture` (OES textures)
3. One-frame-per-cycle release gate ensures color and alpha stay in sync
4. Fragment shader samples both OES textures: `gl_FragColor = vec4(color.rgb, alpha.r)`
5. Zero CPU involvement in decoding — all work is on the hardware video decoder

### Core Components

**C++ Core (`WebMAlphaPlayer`)** — software decode path
- FFmpeg 6.1 with libvpx 1.14.0 for VP9 YUVA decoding
- Multithreaded: decode thread, playback thread, render thread
- LRU `FrameCache` prevents re-decoding during loops
- Zero-copy pipeline from decoder to GPU

**`BufferedDualStreamPlayer`** — hardware decode path (Android)
- Manages demux loop, codec thread, playback loop, and GL thread
- PTS-gated frame release for temporal synchronization
- Handles loop transitions via `MediaCodec.flush()` + demuxer seek

**`SurfaceTextureVideoDecoder`** — codec-agnostic MediaCodec wrapper
- Supports VP9 (`video/x-vnd.on2.vp9`) and AV1 (`video/av01`)
- Automatically selects best hardware decoder, falls back to software
- Creates OES texture + SurfaceTexture per decoder instance
- One-frame-per-cycle release gate prevents output buffer flooding

**`WebMAlphaTextureView`** — Android view
- TextureView-based (proper z-ordering, ScrollView support)
- Dedicated GL thread for EGL context and rendering
- Auto-detects single vs dual stream and routes to correct backend
- Handles view lifecycle (attach/detach/z-index reorder) gracefully

**iOS (Metal)** — software decode only
- `WebMAlphaMetalPlayerView`: MTKView with YUVA-to-RGBA compute shader
- Triple buffering prevents tearing

**Web** — HTML5 `<video>` fallback with automatic platform detection

## Playback Modes (Software Path)

The `mode` prop controls the software decode path. The hardware dual-stream path always streams.

```tsx
<WebMAlphaView source={{ uri: 'video.webm' }} mode="preload" />
<WebMAlphaView source={{ uri: 'long-video.webm' }} mode="stream" />
<WebMAlphaView source={{ uri: 'video.webm' }} mode="streamAndPreload" />
```

| Mode | Startup | Memory | Looping | Best For |
|------|---------|--------|---------|----------|
| `preload` | Wait for full decode | High (all frames) | Seamless | Short loops, many simultaneous players |
| `stream` | Immediate | Low (cache only) | Re-decode | Long videos, memory-constrained |
| `streamAndPreload` | Immediate | High (after 1st pass) | Seamless | Fast start + smooth loops |

**Key insight**: `preload` mode has **zero CPU cost during playback** since all frames are already decoded. This means 20+ preloaded animations can play simultaneously with minimal CPU impact (only GPU rendering).

## Global Decode Queue

When spawning many players simultaneously in `preload` or `streamAndPreload` mode, the library uses a **global decode queue** to serialize decoding operations. This prevents CPU saturation, UI freezes, and crashes.

```tsx
// Default behavior - uses global queue (safe for many simultaneous players)
<WebMAlphaView source={{ uri: 'video.webm' }} />

// Opt out of queue (only if you know you have a single player)
<WebMAlphaView source={{ uri: 'video.webm' }} useDecodeQueue={false} />
```

| Scenario | `useDecodeQueue` | Why |
|----------|------------------|-----|
| Many players spawned at once | `true` (default) | Prevents freezing and crashes |
| Single player, fastest load time | `false` | No queue overhead |
| `mode="stream"` | N/A | Queue is automatically bypassed |
| Dual-stream hardware path | N/A | Not applicable (MediaCodec handles scheduling) |

## Creating Alpha Videos

### Single-Stream YUVA (VP9 with embedded alpha)

```bash
ffmpeg -i input.mov -c:v libvpx-vp9 -pix_fmt yuva420p -crf 18 -b:v 0 \
       -speed 0 -auto-alt-ref 0 output.webm
```

| Parameter | Description |
|-----------|-------------|
| `-c:v libvpx-vp9` | VP9 codec with alpha support |
| `-pix_fmt yuva420p` | YUVA pixel format (includes alpha channel) |
| `-crf 18` | Quality (0-63, lower = better; 18-24 is a good range) |
| `-auto-alt-ref 0` | Required for alpha |

### Dual-Stream (color + alpha as separate tracks)

The dual-stream format stores color and alpha as two independent video streams inside a single WebM container. Stream 0 is the color video, stream 1 is a grayscale video whose red channel is sampled as alpha at render time.

**From a single-stream YUVA source** (extracts and re-encodes both channels):

```bash
# AV1 dual-stream from existing YUVA WebM
ffmpeg -c:v libvpx-vp9 -i input.webm \
       -filter_complex "[0:v]format=yuv420p[rgb]; [0:v]alphaextract,format=gray[alpha]" \
       -map "[rgb]" -map "[alpha]" \
       -c:v:0 libsvtav1 -crf 28 -preset 8 \
       -c:v:1 libsvtav1 -crf 28 -preset 8 \
       -metadata:s:v:1 title=Alpha \
       output_dual_av1.webm

# VP9 dual-stream from existing YUVA WebM
ffmpeg -c:v libvpx-vp9 -i input.webm \
       -filter_complex "[0:v]format=yuv420p[rgb]; [0:v]alphaextract,format=gray[alpha]" \
       -map "[rgb]" -map "[alpha]" \
       -c:v:0 libvpx-vp9 -crf 20 -b:v 0 \
       -c:v:1 libvpx-vp9 -crf 20 -b:v 0 \
       -metadata:s:v:1 title=Alpha \
       output_dual_vp9.webm
```

| Parameter | Description |
|-----------|-------------|
| `-c:v libvpx-vp9` (input) | Decode the YUVA source with libvpx so the alpha plane is available |
| `format=yuv420p` | Strip alpha from the color stream |
| `alphaextract,format=gray` | Extract the alpha channel as a grayscale video |
| `-map "[rgb]" -map "[alpha]"` | Place color as stream 0, alpha as stream 1 |
| `-c:v:0` / `-c:v:1` | Encode each stream independently (can use different CRF) |
| `-metadata:s:v:1 title=Alpha` | Optional: tag the alpha stream for identification |

**From separate color + alpha files** (e.g. from After Effects or Blender):

```bash
# AV1 dual-stream
ffmpeg -i color.mp4 -i alpha_grayscale.mp4 \
       -map 0:v -map 1:v \
       -c:v:0 libsvtav1 -crf 28 -preset 8 \
       -c:v:1 libsvtav1 -crf 28 -preset 8 \
       output_dual_av1.webm

# VP9 dual-stream
ffmpeg -i color.mp4 -i alpha_grayscale.mp4 \
       -map 0:v -map 1:v \
       -c:v libvpx-vp9 -crf 20 -b:v 0 \
       output_dual_vp9.webm
```

**Tips:**
- AV1 (`libsvtav1`) gives better compression than VP9 at the same visual quality, but requires an AV1 MediaCodec on the device.
- `-preset 8` is a good speed/quality tradeoff for AV1 encoding. Lower = slower but better compression.
- Both streams must use the same codec (both VP9 or both AV1). Mixing codecs is not supported.

### Verifying Your Files

```bash
# Check stream layout
ffprobe -v quiet -print_format json -show_streams output.webm

# Single-stream: look for codec_name=vp9, pix_fmt=yuva420p
# Dual-stream: look for 2 video streams (both vp9 or both av1)
```

## FFmpeg Build

The library ships with prebuilt FFmpeg 6.1 + libvpx 1.14.0 static libraries. To rebuild:

```bash
npm run build:ffmpeg:ios          # iOS (device + simulator)
npm run build:ffmpeg:android      # Android (arm64-v8a, armeabi-v7a, x86_64)
npm run build:ffmpeg:all          # All platforms
```

| Platform | Output Directory | Architecture |
|----------|-----------------|-------------|
| iOS Device | `prebuilt/ios/` | arm64 |
| iOS Simulator | `prebuilt/ios-simulator/` | arm64 (Apple Silicon) |
| Android | `prebuilt/android/{abi}/` | arm64-v8a, armeabi-v7a, x86_64 |

## License

LGPL v2.1+

This library is licensed under LGPL v2.1+ due to its use of FFmpeg. This ensures compliance with FFmpeg's licensing requirements while allowing commercial use with proper attribution and linking flexibility.

### FFmpeg and LGPL

- **FFmpeg**: LGPL v2.1 or later
- **libvpx (VP9)**: BSD-style license (permissive)
- **Static linking** creates a derivative work under LGPL

**Compliance options**: (1) License under LGPL v2.1+ (recommended), (2) provide FFmpeg as dynamic libraries, or (3) distribute object files with relinking instructions.

## Credits

Built with:
- [FFmpeg](https://ffmpeg.org/) - Video demuxing and software decoding
- [libvpx](https://www.webmproject.org/code/) - VP9 codec
- [React Native](https://reactnative.dev/) - Cross-platform framework
