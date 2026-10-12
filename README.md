# alpha-video

Transparent video for iOS, Android, React Native and the web. Plays **software-decoded VP9 with alpha** (single-stream YUVA WebM) everywhere, and **hardware-decoded dual-stream VP9/AV1** (color and alpha in separate video tracks, decoded by MediaCodec) on Android.

One repository, three packages:

| Package | For | Install |
|---|---|---|
| `react-native-alpha-video` (npm) | React Native, and react-native-web | [React Native](#react-native) |
| `dev.mediacache:alpha-video` (Maven Central) | Android apps without React Native | [Android](#android-without-react-native) |
| `AlphaVideo` (Swift Package Manager) | iOS apps without React Native | [iOS](#ios-without-react-native) |

## Features

- **VP9 Alpha Transparency** - Full support for WebM videos with embedded YUVA alpha channel
- **Dual-Stream Hardware Decode** - Color + alpha as separate VP9 or AV1 streams, decoded via Android MediaCodec with zero CPU involvement
- **AV1 Support** - Dual-stream AV1 hardware decode on devices with AV1 MediaCodec support
- **High Performance** - Native C++ core (FFmpeg) with GPU-accelerated rendering
- **iOS** - Metal-based rendering with YUVA-to-RGBA GPU shader
- **Android** - TextureView with OpenGL ES 2.0, proper z-ordering and ScrollView support
- **Web** - HTML5 video fallback for react-native-web compatibility (Chrome and Firefox; see [Web](#web))
- **Zero-Copy Frame Pipeline** - Efficient memory usage with LRU frame caching (software path) and OES texture compositing (hardware path)

## Decision Matrix

Choosing the right encoding format depends on your use case:

| | Single-Stream YUVA (Software) | Dual-Stream VP9/AV1 (Hardware) |
|---|---|---|
| **Decoding** | CPU (FFmpeg libvpx) | GPU (MediaCodec) |
| **CPU usage during playback** | Depends on `mode`: none once `preload` has decoded every frame (upload and render only); continuous decoding in `stream`; first pass only in `streamAndPreload` | Near-zero (decode offloaded) |
| **Looping** | Seamless | Tiny stall at loop boundary (MediaCodec flush) |
| **Max resolution** | ~720p practical limit | 1080p+ (hardware limited) |
| **Best for** | Short loops, small animations, many simultaneous players | Long-duration, high-res, single-instance playback |
| **iOS support** | Yes (Metal) | No (Android only) |
| **Codec** | VP9 only | VP9 or AV1 |
| **Instances** | Unlimited (CPU bound) | Limited by MediaCodec slots (typically 8-16 per codec) |

**Rule of thumb**: Use single-stream YUVA for short looping effects (glows, particles, badges). Use dual-stream for hero videos, full-screen overlays, or anything over 720p / 10 seconds.

## Installation

### React Native

```bash
npm install react-native-alpha-video
# or
yarn add react-native-alpha-video
```

The npm package carries the prebuilt FFmpeg libraries, so nothing else is downloaded.

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

### Android without React Native

```gradle
dependencies {
    implementation 'dev.mediacache:alpha-video:1.0.0'
}
```

`dev.mediacache.alphavideo.AlphaVideoView` is a `TextureView`. It takes the same sources and options as the React Native component, through setters:

```kotlin
val view = AlphaVideoView(context)
view.setMode("preload")
view.setCallback(object : AlphaVideoView.PlayerCallback {
    override fun onLoad(totalFrames: Int, width: Int, height: Int) {}
    override fun onError(error: String) {}
    override fun onEnd() {}
    override fun onPreloadComplete(frames: Int, timeMs: Double) {}
})
view.setSource("asset://videos/glow.webm")  // loads on the next main-thread turn
container.addView(view)

// When the view is gone for good. Detaching keeps the player so the view can be re-attached.
view.release()
```

Callbacks arrive on the main thread. http(s) sources need the `INTERNET` permission in the app's manifest.

### iOS without React Native

In Xcode, **File > Add Package Dependencies…** with `https://github.com/danielkpl2/alpha-video`, or in `Package.swift`:

```swift
.package(url: "https://github.com/danielkpl2/alpha-video", from: "1.0.0")
// target dependency:
.product(name: "AlphaVideo", package: "alpha-video")
```

FFmpeg comes as a prebuilt `FFmpegVPX.xcframework` from the GitHub Release. `AlphaVideoView` is a `UIView`:

```swift
import AlphaVideo

let view = AlphaVideoView(frame: bounds)
view.delegate = self       // AlphaVideoViewDelegate: didLoadFrames, didFailWithError, didEnd
view.mode = "preload"
view.source = "asset://videos/glow.webm"  // loads on the next main-queue turn
addSubview(view)
```

Delegate calls arrive on the main queue. iOS 13 or later.

## Usage

```tsx
import { AlphaVideoView } from 'react-native-alpha-video';

function MyComponent() {
  return (
    <AlphaVideoView
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
| `source` | `{ uri: string }` | required | Video source URL or local file path (see [Supported Source Types](#supported-source-types)) |
| `cacheKey` | `string` | hash of the URL | Cache file path for an http(s) source, relative to the app's cache directory (may include subfolders, must stay inside it) |
| `autoPlay` | `boolean` | `true` | Auto-play video when loaded |
| `loop` | `boolean` | `true` | Loop video playback |
| `paused` | `boolean` | `false` | Pause video playback |
| `playbackSpeed` | `number` | `1.0` | Playback speed multiplier. Changeable while playing, and does **not** move the playhead — see below |
| `style` | `ViewStyle` | - | Container style |
| `onLoad` | `function` | - | Callback when video loads (`{ totalFrames, width, height }`) |
| `onError` | `function` | - | Callback when error occurs (`{ error }`) |
| `onEnd` | `function` | - | Callback when playback reaches the end with `loop` off |
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
<AlphaVideoView source={source} playbackSpeed={hovered ? 2 : 1} />
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
import { AlphaVideoView, AlphaVideoViewRef } from 'react-native-alpha-video';

const playerRef = useRef<AlphaVideoViewRef>(null);

playerRef.current?.play();
playerRef.current?.pause();
playerRef.current?.stop();
playerRef.current?.seekToFrame(30);
```

### Decoder Status API (Android)

Query hardware decoder capabilities at runtime:

```tsx
import { getDecoderStatus, getDecoderInfo } from 'react-native-alpha-video';

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

- **HTTP/HTTPS URLs**: `{ uri: 'https://example.com/video.webm' }`. Downloaded once into the app's cache directory by the platform's HTTP stack (`HttpURLConnection` on Android, `NSURLSession` on iOS) and played from there. The file is named by `cacheKey` if you give one (it may include subfolders), otherwise by a hash of the URL. See [Playing while downloading](#playing-while-downloading).
- **Local files**: `{ uri: 'file:///path/to/video.webm' }` or an absolute path.
- **Bundled files**: `{ uri: 'asset://videos/glow.webm' }`. On Android this is `android/app/src/main/assets/videos/glow.webm`. On iOS it is `videos/glow.webm` inside the app bundle. On web it is `/videos/glow.webm` from the site root.
- **Android `content://` URIs**, for example from a document picker.
- **`require()`**: pass `Image.resolveAssetSource(require('./glow.webm')).uri`. In debug builds this is a Metro `http://localhost:8081` URL. In release builds it is a file path on iOS and a `res/raw` resource name on Android.

Nothing is copied. Android reads `asset://` files and `res/raw` resources directly from the APK, so they must be stored uncompressed. `.webm` and `.mkv` are uncompressed by default. For other extensions, add them to `android.androidResources.noCompress` in the app's `build.gradle`.

### Playing while downloading

In `stream` and `streamAndPreload` mode (and on the Android hardware path in those modes), an http(s) clip starts playing as soon as its first bytes arrive: the decoder reads the cache file while it is still being written. `preload` waits for the whole file, since it decodes every frame before showing one.

- Views that ask for the same URL at the same time share one download. Once it is complete, later views play the cached file without a request.
- A download that fails, including one cut off part way, is reported through `onError` as `Download failed: …`. The player stops at the last frame that arrived instead of looping over a partial file. Nothing is cached, so the next load starts over.
- If the network is slower than the clip's bitrate, playback waits for frames on the software path. On the Android hardware path its clock can run ahead during the stall and catch up quickly afterwards.
- `seekToFrame` during a download waits until the file is complete, because a WebM file keeps its seek index (Cues) at the end. Looping back to the start only happens after the end has arrived, so it isn't affected.

### Web

On the web the component renders a muted HTML5 `<video>`, so the browser does the decoding and streaming. It honours `source`, `autoPlay`, `loop`, `paused`, `playbackSpeed`, `onLoad`, `onError`, `onEnd` and the ref methods; `mode`, `cacheKey` and the stream and queue props have no effect.

- **Chrome, Edge and Firefox** play single-stream YUVA VP9 WebM with its transparency.
- **Safari** doesn't show WebM's alpha channel. For Safari, serve a separate HEVC-with-alpha file.
- **Dual-stream files** show only the colour track in every browser: `<video>` plays one video track, so the alpha track is ignored.
- Browsers don't report a frame rate, so `onLoad`'s `totalFrames` and `seekToFrame` assume 30 fps.

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

**`AlphaVideoView`** — Android view
- TextureView-based (proper z-ordering, ScrollView support)
- Dedicated GL thread for EGL context and rendering
- Auto-detects single vs dual stream and routes to correct backend
- Handles view lifecycle (attach/detach/z-index reorder) gracefully

**iOS (Metal)** — software decode only
- `AlphaVideoView`: UIView that resolves the source, applies the mode and runs the global decode queue
- `AlphaVideoMetalView`: MTKView with YUVA-to-RGBA compute shader
- Triple buffering prevents tearing

**Web** — HTML5 `<video>` fallback with automatic platform detection

## Playback Modes (Software Path)

The `mode` prop controls the software decode path. The hardware dual-stream path always streams.

```tsx
<AlphaVideoView source={{ uri: 'video.webm' }} mode="preload" />
<AlphaVideoView source={{ uri: 'long-video.webm' }} mode="stream" />
<AlphaVideoView source={{ uri: 'video.webm' }} mode="streamAndPreload" />
```

| Mode | Startup | Memory | Looping | Best For |
|------|---------|--------|---------|----------|
| `preload` | Wait for full decode | High (all frames) | Seamless | Short loops, many simultaneous players |
| `stream` | Immediate | Low (cache only) | Re-decode | Long videos, memory-constrained |
| `streamAndPreload` | Immediate | High (after 1st pass) | Seamless | Fast start + smooth loops |

For http(s) sources, "Immediate" means as soon as the first bytes have downloaded, and `preload` also waits for the download ([Playing while downloading](#playing-while-downloading)).

**Key insight**: `preload` mode has **zero CPU cost during playback** since all frames are already decoded. This means 20+ preloaded animations can play simultaneously with minimal CPU impact (only GPU rendering).

## Global Decode Queue

When spawning many players simultaneously in `preload` or `streamAndPreload` mode, the library uses a **global decode queue** to serialize decoding operations. This prevents CPU saturation, UI freezes, and crashes.

```tsx
// Default behavior - uses global queue (safe for many simultaneous players)
<AlphaVideoView source={{ uri: 'video.webm' }} />

// Opt out of queue (only if you know you have a single player)
<AlphaVideoView source={{ uri: 'video.webm' }} useDecodeQueue={false} />
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

The library ships with prebuilt FFmpeg 6.1 + libvpx 1.14.0 static libraries: only `libavformat`, `libavcodec`, `libavutil` and `libvpx`, configured for the WebM demuxer and the VP9 decoders. To rebuild:

```bash
npm run build:ffmpeg:ios          # iOS (device + simulator)
npm run build:ffmpeg:android      # Android (arm64-v8a, armeabi-v7a, x86_64)
npm run build:ffmpeg:all          # All platforms
```

The Android script uses the NDK in `ANDROID_NDK_HOME` (default `$ANDROID_HOME/ndk/27.1.12297006`) and needs `nasm` for x86_64.

| Platform | Output Directory | Architecture |
|----------|-----------------|-------------|
| iOS Device | `prebuilt/ios/` | arm64 |
| iOS Simulator | `prebuilt/ios-simulator/` | arm64 + x86_64 |
| Android | `prebuilt/android/{abi}/` | arm64-v8a, armeabi-v7a, x86_64 |

`prebuilt/` is not committed. In a git clone, either rebuild it as above or download the release's copy:

```bash
scripts/fetch-prebuilt.sh             # prebuilt/ from the GitHub Release, checksum-verified
```

### Native packages and releases

```bash
scripts/build_xcframework.sh          # prebuilt/FFmpegVPX.xcframework(.zip) for Package.swift
scripts/publish-android.sh local      # dev.mediacache:alpha-video into ~/.m2
scripts/publish-android.sh central    # signed, to the Maven Central Portal
scripts/package_release.sh            # the GitHub Release assets, in dist/release-v<version>/
```

`Package.swift` uses `prebuilt/FFmpegVPX.xcframework` when it exists, and the release zip otherwise. Its checksum must match the zip attached to the GitHub Release, so rebuilding the xcframework means updating the checksum. The same goes for the prebuilt archive and `fetch-prebuilt.sh`, although packaging the same libraries again gives the same archive. `package_release.sh` stops or warns when either checksum is out of date. `publish-android.sh` lists the environment variables it needs for Central.

A release carries the prebuilt archive, `FFmpegVPX.xcframework.zip`, and the FFmpeg and libvpx source tarballs that [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) points to.

### Repository layout

| Path | Contents |
|---|---|
| `cpp/` | The C++ core (FFmpeg demux and libvpx decode), shared by every platform |
| `android/` | The Android library: `AlphaVideoView`, the MediaCodec dual-stream path and the JNI build |
| `ios/Sources/AlphaVideo/` | The iOS library: `AlphaVideoView` and the Metal renderer |
| `react-native/` | The React Native component (`src/`) and its thin native view managers |
| `Package.swift`, `react-native-alpha-video.podspec`, `package.json` | The SwiftPM, CocoaPods and npm packages |

## License

The code in this repository is MIT (see [LICENSE](LICENSE)).

The prebuilt FFmpeg libraries are LGPL-2.1-or-later and libvpx is BSD-3-Clause. LGPL covers FFmpeg itself, not the code that uses it, but anything that distributes the binaries (this package, and apps built with it) must include their notices and make FFmpeg's source available. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has the versions, configure options, source links and what LGPL asks of closed-source apps; the licence texts are in [`licenses/`](licenses).

## Credits

Built with:
- [FFmpeg](https://ffmpeg.org/) - Video demuxing and software decoding
- [libvpx](https://www.webmproject.org/code/) - VP9 codec
- [React Native](https://reactnative.dev/) - Cross-platform framework
