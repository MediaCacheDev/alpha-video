# alpha-video

Transparent video that plays everywhere: Android, iOS, React Native and every modern browser, including browsers that can't show transparent video themselves. Safari drops WebM's alpha channel, and no browser shows the alpha track of a dual-stream file. If you want transparent video and need to be confident it plays on every platform, this is the package to use.

It plays **VP9 WebM with alpha** (standard YUVA WebM, what FFmpeg and libvpx write) and **dual-stream VP9/AV1** (colour and alpha in separate video tracks). Each platform tries the fastest way first and falls back until one works:

- **Android:** two MediaCodec decoders, one for colour and one for alpha, in hardware where the device has them. Then MediaCodec's software decoders, then the C++ core.
- **iOS:** the C++ core, which demuxes with libwebm, decodes with libvpx (VP9) and dav1d (AV1), and draws with Metal.
- **Web:** the browser's own `<video>` where it keeps the alpha, then WebCodecs, then the same decoders compiled to WebAssembly ([how it works](#web)).

One repository, four packages:

| Package | For | Install |
|---|---|---|
| `react-native-alpha-video` (npm) | React Native, and react-native-web | [React Native](#react-native) |
| `dev.mediacache:alpha-video` (Maven Central) | Android | [Android](#android) |
| `AlphaVideo` (Swift Package Manager), `alpha-video` (CocoaPods) | iOS | [iOS](#ios) |
| `alpha-video-web` (npm) | Browsers: plain JS/TS, or React | [Web](#web), [web/README.md](web/README.md) |

See it next to the other formats in the [demo app](#demo-and-related-packages).

## alpha-video or alpha-video-gpu?

Both play transparent videos on Android, iOS and the web, and they suit different jobs. alpha-video plays video codecs. [alpha-video-gpu](https://github.com/MediaCacheDev/alpha-video-gpu) doesn't decode anything: its frames are ASTC textures, which the GPU samples as they are.

| | alpha-video (this package) | [alpha-video-gpu](https://github.com/MediaCacheDev/alpha-video-gpu) |
|---|---|---|
| **Format** | WebM: VP9 with alpha, or dual-stream VP9/AV1 | KTX2: an ASTC texture array, one layer per frame |
| **Made with** | ffmpeg, or any tool that exports alpha WebM | [alpha-video-gpu-encoder](https://github.com/MediaCacheDev/alpha-video-gpu-encoder) (`texenc`) |
| **Decoding** | Every frame: libvpx or dav1d on the CPU, or MediaCodec on Android | None |
| **CPU and battery** | A decode per frame, or one pass up front with `mode="preload"` | Near zero |
| **What the device needs** | Nothing for software decoding. Hardware decoding needs a VP9 or AV1 decoder (Android only), and each player takes 2 of its few instances | ASTC texture support: every iPhone and iPad since the A8 (2014), nearly every Android GPU, Apple silicon Macs and mobile browsers. That covers more devices than VP9 or AV1 hardware decoding |
| **Many players at once** | Limited by the CPU, or by the hardware decoder's instances | Dozens: each one is just a texture |
| **File size** | Smallest | Several times larger |
| **Seeking** | Decodes from the previous keyframe | Any frame directly |
| **Web** | Every modern browser, Safari included: `<video>`, WebCodecs or WebAssembly decoding (`alpha-video-web`) | WebGL where the browser has ASTC |
| **Best for** | Long or large clips, downloads where size matters, files from standard video tools | UI effects, stickers and glows, many simultaneous loops, short to medium videos |

[alpha-video-ffmpegkit](https://github.com/MediaCacheDev/alpha-video-ffmpegkit) was the first attempt at transparent WebM, built on FFmpegKit. It still works and stays as a reference implementation, but alpha-video does everything it does, and more.

## Features

- **VP9 Alpha Transparency** - Full support for WebM videos with embedded YUVA alpha channel
- **Hardware Decode on Android** - Color and alpha decoded by two MediaCodec instances, from a standard alpha WebM (the alpha is a second VP9 stream in each block's BlockAdditional) or from separate VP9 or AV1 tracks
- **AV1 Support** - Dual-stream AV1, 8- or 10-bit, on Android and iOS: MediaCodec on Android (in hardware where the device has an AV1 decoder), dav1d in the C++ core elsewhere
- **High Performance** - Native C++ core (libvpx, dav1d) with GPU-accelerated rendering
- **iOS** - Metal-based rendering with YUVA-to-RGBA GPU shader
- **Android** - TextureView with OpenGL ES 2.0, proper z-ordering and ScrollView support
- **Web** - `alpha-video-web` shows the alpha in every modern browser: `<video>` where the browser keeps it, otherwise WebCodecs or WebAssembly decoding drawn in WebGL2 (see [Web](#web)). On react-native-web, `AlphaVideoView` uses it
- **Zero-Copy Frame Pipeline** - Efficient memory usage with LRU frame caching (software path) and OES texture compositing (hardware path)

## Decision Matrix

The player picks the decode path itself. On Android it uses MediaCodec when decoders that list the file's VP9 or AV1 profiles have 2 instances free, hardware first and then Google's software ones, and the C++ core otherwise (a Snapdragon 778G's VP9 decoder allows 3 instances, so one hardware player at a time). The [`decoder` prop](#choosing-the-decoder-android) forces an engine. iOS always uses the C++ core.

| | Software path (C++ core) | Hardware path (MediaCodec, Android) |
|---|---|---|
| **Files** | Single-stream YUVA VP9; dual-stream VP9 (8-bit) or AV1 (8- to 12-bit) | Single-stream YUVA VP9; dual-stream VP9 or AV1 |
| **Decoding** | CPU (libvpx for VP9, dav1d for AV1) | GPU (MediaCodec), or MediaCodec's software decoders |
| **CPU usage during playback** | Depends on `mode`: none once `preload` has decoded every frame (upload and render only); continuous decoding in `stream`; first pass only in `streamAndPreload` | Near-zero (decode offloaded) |
| **Looping** | Seamless | Seamless |
| **Max resolution** | ~720p practical limit | 1080p+ (hardware limited) |
| **Best for** | Short loops, small videos, many simultaneous players | Long-duration, high-res, single-instance playback |
| **iOS support** | Yes (Metal) | No (Android only) |
| **Codec** | VP9 or AV1 | VP9 or AV1 |
| **Instances** | Unlimited (CPU bound) | 2 decoder instances each, of the decoder's limit (3 on a Snapdragon 778G, often more); further players use MediaCodec's software decoders, then the software path |

**Rule of thumb**: Standard YUVA WebM plays everywhere, including in Chrome and Firefox, and gets the hardware path on Android when it's free. Use dual-stream for AV1, or to encode alpha at its own quality; browsers show only its colour track.

## Installation

One package per platform, all built from this repository.

### React Native

**npm**, package `react-native-alpha-video`:

```bash
npm install react-native-alpha-video
```

**iOS:** install the pods. The view builds from the podspec inside the npm package.

```bash
cd ios && pod install
```

**Android:** autolinking adds the library, so there's nothing else to do. Its native library is built for `arm64-v8a`, `armeabi-v7a` and `x86_64`. If your app also ships 32-bit `x86`, limit it to those three in `android/app/build.gradle`:

```gradle
android {
    defaultConfig {
        ndk {
            abiFilters 'armeabi-v7a', 'arm64-v8a', 'x86_64'
        }
    }
}
```

```tsx
import { AlphaVideoView } from 'react-native-alpha-video';

<AlphaVideoView
  source={{ uri: 'https://example.com/video.webm' }}
  cacheKey="videos/intro.webm" // what the stored copy is named; default, the URL
  storage="auto"               // where the bytes are kept between plays; see Streaming and storage
  maxStorageBytes={512 * 1024 * 1024} // the store's cap, over every clip in it
  style={{ width: 300, height: 300 }}
  autoPlay={true}              // the default; false shows the first frame and waits for play()
  loop={true}                  // the default; false plays once, then calls onEnd
  paused={false}               // a hold, for example while the screen is hidden
  playbackSpeed={1.0}          // changeable while playing; doesn't move the playhead
  decoder="auto"               // Android only; see Choosing the Decoder
  mode="stream"                // decodes a few frames ahead (default), or 'preload', 'streamAndPreload'
  streamCacheSize={30}         // 'stream': decoded frames kept
  streamPrefetchAhead={15}     // 'stream' and 'streamAndPreload': frames decoded ahead
  useDecodeQueue={true}        // 'preload' and 'streamAndPreload': one clip decodes at a time, app-wide
  verbose={false}              // true logs lifecycle and per-frame events; errors always log
  onLoad={({ totalFrames, width, height, decoder }) => console.log(totalFrames, decoder)}
  onError={({ error }) => console.error(error)}
  onEnd={() => console.log('played once')}
/>
```

The example sets every option; [Options](#options) says when each one takes effect.

The npm package carries the prebuilt libvpx and dav1d libraries, so nothing else is downloaded.

The package works on both React Native architectures: the new one (a Fabric view, and a TurboModule for the [decoder status API](#decoder-status-api-android); the default since 0.76) and the old one (`newArchEnabled=false`, up to 0.81). It is tested on 0.83, and on 0.81 with the new architecture turned off.

**Expo:**
- Expo Go: not supported, as the package has native code.
- A development build or EAS Build: supported. Run `npx expo prebuild` (EAS Build does it for you), so the pods and Gradle dependencies are compiled in.

### Android

**Maven Central**, artifact `dev.mediacache:alpha-video`:

```gradle
dependencies {
    implementation 'dev.mediacache:alpha-video:1.0.0'
}
```

The AAR includes libvpx and dav1d, built for `arm64-v8a`, `armeabi-v7a` and `x86_64`. Android 7.0 (API 24) or later. To read videos through a Media3 `DataSource.Factory`, add `dev.mediacache:alpha-video-media3` too ([Data sources](#data-sources)).

`dev.mediacache.alphavideo.AlphaVideoView` is a `TextureView`. It takes the same sources and options as the React Native component, through setters:

```kotlin
val view = AlphaVideoView(context)
view.setCacheKey("videos/intro.webm")  // what the stored copy is named; default, the URL
view.setStorage("auto")                // where the bytes are kept between plays; see Streaming and storage
view.setMaxStorageBytes(512L * 1024 * 1024)  // the store's cap, over every clip in it
view.setAutoPlay(true)                 // the default; false shows the first frame and waits for play()
view.setLoop(true)                     // the default; false plays once, then calls onEnd
view.setPaused(false)                  // a hold, for example while the screen is hidden
view.setPlaybackSpeed(1.0)             // changeable while playing; doesn't move the playhead
view.setDecoder("auto")                // see Choosing the Decoder
view.setMode("stream")                 // decodes a few frames ahead (default), or "preload", "streamAndPreload"
view.setStreamCacheSize(30)            // "stream": decoded frames kept
view.setStreamPrefetchAhead(15)        // "stream" and "streamAndPreload": frames decoded ahead
view.setUseDecodeQueue(true)           // "preload" and "streamAndPreload": one clip decodes at a time, app-wide
view.setVerbose(false)                 // true logs lifecycle and per-frame events to Logcat
view.setCallback(object : AlphaVideoView.PlayerCallback {
    override fun onLoad(totalFrames: Int, width: Int, height: Int, decoder: String) {}
    override fun onError(error: String) {}
    override fun onEnd() {}
    override fun onPreloadComplete(frames: Int, timeMs: Double) {}  // every frame decoded
})
view.setSource("https://example.com/video.webm")  // loads on the next main-thread turn
container.addView(view)

// When the view is gone for good. Detaching keeps the player so the view can be re-attached.
view.release()
```

Control playback with `play()`, `pause()`, `stop()`, `seekToFrame(frame)` and `seekToTime(seconds)`, and hold it with `setPaused(paused)`, as described under [Starting and pausing](#starting-and-pausing); seeks behave as described under [Ref Methods](#ref-methods). Callbacks arrive on the main thread. http(s) sources need the `INTERNET` permission in the app's manifest.

### iOS

**CocoaPods**, pod `alpha-video`:

```ruby
pod 'alpha-video', '1.0.0'
```

**Swift Package Manager**, package `alpha-video`, product `AlphaVideo`:

```swift
.package(url: "https://github.com/MediaCacheDev/alpha-video", from: "1.0.0")
// in the target's dependencies:
.product(name: "AlphaVideo", package: "alpha-video")
```

The module is `AlphaVideo` either way. libvpx and dav1d come with it, prebuilt. `AlphaVideoView` is a `UIView`:

```swift
import AlphaVideo

let view = AlphaVideoView(frame: bounds)
view.delegate = self                   // see below
view.cacheKey = "videos/intro.webm"    // what the stored copy is named; default, the URL
view.storage = "auto"                  // where the bytes are kept between plays; see Streaming and storage
view.maxStorageBytes = 512 * 1024 * 1024  // the store's cap, over every clip in it
view.autoPlay = true                   // the default; false shows the first frame and waits for play
view.loop = true                       // the default; false plays once, then calls didEnd
view.paused = false                    // a hold, for example while the screen is hidden
view.playbackSpeed = 1.0               // changeable while playing; doesn't move the playhead
view.mode = "stream"                   // decodes a few frames ahead (default), or "preload", "streamAndPreload"
view.streamCacheSize = 30              // "stream": decoded frames kept
view.streamPrefetchAhead = 15          // "stream" and "streamAndPreload": frames decoded ahead
view.useDecodeQueue = true             // "preload" and "streamAndPreload": one clip decodes at a time, app-wide
view.verbose = false                   // true logs lifecycle and per-frame events to the Xcode console
// No decoder setting: iOS always decodes in software, with the C++ core
view.source = "https://example.com/video.webm"  // loads on the next main-queue turn
addSubview(view)

extension MyViewController: AlphaVideoViewDelegate {
    func alphaVideoView(_ view: AlphaVideoView, didLoadFrames totalFrames: Int32, width: Int32, height: Int32) {}
    func alphaVideoView(_ view: AlphaVideoView, didFailWithError error: String) {}
    func alphaVideoViewDidEnd(_ view: AlphaVideoView) {}
}
```

Control playback with `play`, `pause`, `stop`, `seekToFrame:` and `seekToTime:`, and hold it with `paused`, as described under [Starting and pausing](#starting-and-pausing); seeks behave as described under [Ref Methods](#ref-methods). Delegate calls arrive on the main queue. iOS 13 or later.

## Usage

```tsx
import { useRef } from 'react';
import { AlphaVideoView, AlphaVideoViewRef } from 'react-native-alpha-video';

function MyComponent() {
  const player = useRef<AlphaVideoViewRef>(null);  // play, pause, stop, seekToFrame, seekToTime
  return (
    <AlphaVideoView
      ref={player}
      source={{ uri: 'https://example.com/video.webm' }}
      cacheKey="videos/intro.webm" // what the stored copy is named; default, the URL
      storage="auto"               // where the bytes are kept between plays; see Streaming and storage
      maxStorageBytes={512 * 1024 * 1024} // the store's cap, over every clip in it
      style={{ width: 300, height: 300 }}
      autoPlay={true}              // the default; false shows the first frame and waits for play()
      loop={true}                  // the default; false plays once, then calls onEnd
      paused={false}               // a hold, for example while the screen is hidden
      playbackSpeed={1.0}          // changeable while playing; doesn't move the playhead
      decoder="auto"               // Android only; see Choosing the Decoder
      mode="stream"                // decodes a few frames ahead (default), or 'preload', 'streamAndPreload'
      streamCacheSize={30}         // 'stream': decoded frames kept
      streamPrefetchAhead={15}     // 'stream' and 'streamAndPreload': frames decoded ahead
      useDecodeQueue={true}        // 'preload' and 'streamAndPreload': one clip decodes at a time, app-wide
      verbose={false}              // true logs lifecycle and per-frame events; errors always log
      onLoad={({ totalFrames, width, height, decoder }) => console.log(totalFrames, decoder)}
      onError={({ error }) => console.error(error)}
      onEnd={() => console.log('played once')}
    />
  );
}
```

The player automatically detects whether a file contains a single YUVA stream or dual video streams, and on Android whether MediaCodec can decode it (see [Decision Matrix](#decision-matrix)). No prop changes are needed; `decoder` only overrides the choice on Android.

### Starting and pausing

`autoPlay` sets how a video starts. Off, it loads, shows its first frame and waits for `play()`. `paused` is a hold to turn on and off, for example while the screen is hidden or the app is in the background. A video that starts paused also loads and shows its first frame, and unpausing plays it when `autoPlay` is on. With `autoPlay` off, unpausing leaves it where it is; call `play()`.

### Options

The examples under [Installation](#installation) and [Usage](#usage) set every option, each to its default. `paused`, `playbackSpeed` and `verbose` change at once; the others are read when a source loads, so set them before the source.

iOS has no `decoder`: it always decodes in software. The cache, prefetch and queue settings apply to the C++ core's software decoding; MediaCodec on Android streams with its own buffers and ignores them, and so does the web, where the browser plays the file. On the web `mode`, `decoder`, `source.dataSource` and the stream and queue props have no effect (see [Web](#web)).

### Props

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `source` | `{ uri: string, headers?: Record<string, string>, dataSource?: string }` | required | Video source URL or local file path (see [Supported Source Types](#supported-source-types)). `headers` go with every request for it; `dataSource` names a registered [data source](#data-sources) to read it through |
| `cacheKey` | `string` | the URL | What the store keeps the clip under, for URLs that change while the file doesn't (presigned URLs) |
| `storage` | `'auto' \| 'disk' \| 'memory' \| 'none'` | `'auto'` | Where the bytes are kept between plays: `'auto'` is `'disk'` for URLs and `'none'` for a data source (see [Streaming and storage](#streaming-and-storage)) |
| `maxStorageBytes` | `number` | 512 MB | The store's cap, over every clip in it, least recently played out first |
| `autoPlay` | `boolean` | `true` | Start playing once loaded. Off, the first frame shows and waits for `play()` |
| `loop` | `boolean` | `true` | Loop video playback |
| `paused` | `boolean` | `false` | Hold playback. Unpausing plays on when `autoPlay` is on |
| `playbackSpeed` | `number` | `1.0` | Playback speed multiplier. Changeable while playing, and does **not** move the playhead — see below |
| `style` | `ViewStyle` | - | Container style. On Android `backgroundColor` is ignored (the view is a `TextureView`); put a background on a parent view |
| `onLoad` | `function` | - | Callback when video loads (`{ totalFrames, width, height, decoder }`); `decoder` names the engine and decoders, e.g. `'mediacodec-hw (c2.qti.vp9.decoder)'`, `'software (libvpx)'`, `'software (dav1d)'`; on the web `'browser'`, `'webcodecs (vp09.00.41.08)'` or `'software (vp9)'` |
| `onError` | `function` | - | Callback when error occurs (`{ error }`; on the web also `code`, such as `'NO_ALPHA_PATH'`) |
| `onEnd` | `function` | - | Callback when playback reaches the end with `loop` off |
| `mode` | `'preload' \| 'stream' \| 'streamAndPreload'` | `'stream'` | Playback mode (software path only, see below) |
| `decoder` | `'auto' \| 'mediacodec' \| 'mediacodec-hw' \| 'mediacodec-sw' \| 'software'` | `'auto'` | Android: the decoding engine (see [Choosing the Decoder](#choosing-the-decoder-android)). iOS accepts it and uses the C++ core; the web has `webDecoder` |
| `webDecoder` | `'auto' \| 'browser' \| 'webcodecs' \| 'webcodecs-hw' \| 'webcodecs-sw' \| 'software'` | `'auto'` | Web: the engine (see [Web](#web)). `'browser'` is a plain `<video>`; a forced engine reports `onError` rather than fall back |
| `streamCacheSize` | `number` | `30` | Decoded frames kept in `stream` mode (an LRU cache) |
| `streamPrefetchAhead` | `number` | `15` | Frames decoded ahead of the playhead in `stream` and `streamAndPreload` mode |
| `useDecodeQueue` | `boolean` | `true` | In `preload` and `streamAndPreload` mode, decode one clip at a time across all players, so many at once don't saturate the CPU (see [Global Decode Queue](#global-decode-queue)) |
| `verbose` | `boolean` | `false` | Log lifecycle and per-frame events (Logcat, the Xcode console); errors always log |

### Playback speed

`playbackSpeed` can change at any time, including mid-playback, and only affects how long each
frame is shown. The playhead stays exactly where it was:

```tsx
// Speeds the video up from wherever it currently is. No seek, no restart.
<AlphaVideoView source={source} playbackSpeed={hovered ? 2 : 1} />
```

That distinction matters for anything driven by interaction. A rate change that also reset the
position would read as a glitch rather than as an acceleration.

Values at or below zero are clamped to `0.01` rather than stopping playback — use `paused` to
stop.

### Glow buttons

A button that plays a multi-state glow sheet (default, hover and press stacked as rows in one
file) lives in its own package, [alpha-glow-button](https://github.com/MediaCacheDev/alpha-glow-button).
Its `alpha-glow-button/alpha-video` entry point uses this player, and relies on the live
`playbackSpeed` above to change speed per state without restarting the loop.

### Ref Methods

```tsx
import { AlphaVideoView, AlphaVideoViewRef } from 'react-native-alpha-video';

const playerRef = useRef<AlphaVideoViewRef>(null);

playerRef.current?.play();
playerRef.current?.pause();
playerRef.current?.stop();
playerRef.current?.seekToFrame(30);   // frame number, from 0
playerRef.current?.seekToTime(1.5);   // seconds; the frame showing at that time
```

Seeking works in every mode and on both decode paths. The target is clamped to the clip, and the
player keeps playing from there or stays paused showing that frame. In `stream` mode a seek far
from the playhead jumps the decoder to the nearest keyframe before it, so the wait depends on the
keyframe interval. A seek before `onLoad` is ignored.

### Choosing the Decoder (Android)

`decoder` picks the engine on Android: MediaCodec (the platform's decoders, in hardware where the device has them) or the C++ core's own software decoders. iOS has no such setting and always uses the C++ core; the web uses the browser. `decoder` is read when a source loads, so set it before or together with `source`. Changing it on its own doesn't reload the video.

```tsx
<AlphaVideoView source={source} decoder="auto" />          // the default
<AlphaVideoView source={source} decoder="mediacodec-hw" /> // hardware only, or onError
```

| Value | Engine |
|---|---|
| `'auto'` (default) | Hardware MediaCodec for both tracks; else software MediaCodec for both; else the C++ core (`'software'`) |
| `'mediacodec'` | MediaCodec, chosen per track: hardware if one is free, else software, so the two tracks can get one of each |
| `'mediacodec-hw'` | Hardware MediaCodec for both tracks |
| `'mediacodec-sw'` | Software MediaCodec for both tracks (Google's `c2.android.*` decoders) |
| `'software'` | The C++ core's software decoders: libvpx for 8-bit VP9, dav1d for 8- to 12-bit AV1 (drawn at 8 bits) |

`'software'` is the player's own decoding, the same as on iOS, so it doesn't depend on what the device has. `'mediacodec-sw'` is Android's software decoders, run through MediaCodec.

At each load the player decides in this order:

1. **`'software'`** goes straight to the C++ core.
2. **Can MediaCodec take the file?** It needs colour and alpha as separate VP9 or AV1 bitstreams: standard YUVA WebM (the alpha is a second VP9 stream inside each block) or a dual-stream file. If the file has neither (a VP9 track without alpha, say), `'auto'` plays it on the C++ core and the `mediacodec` values report `onError`.
3. **Reserve 2 decoders, one per track.** A decoder qualifies if it decodes the track's codec, lists its profile (VP9 profiles 0 to 3, AV1 Main) and has an instance free. Instances are counted across every player in the app, and decoders cap them: a Snapdragon 778G's VP9 hardware decoder allows 3. Hardware decoders are tried in the order Android lists them.
4. **Fall back, or not.** `'auto'` tries MediaCodec hardware for both tracks, then MediaCodec software for both, then the C++ core; it doesn't mix hardware and software. Every other value stops at its own engine and reports `onError` when it can't get one, so a forced engine can be trusted for testing and benchmarks.

A player gives its decoders back when it unloads, or when its view loads a new source, before the new one reserves its own. With the Snapdragon 778G's 3 VP9 hardware instances:

| Players with VP9 files | `'auto'` | `'mediacodec'` |
|---|---|---|
| First | `mediacodec-hw` | `mediacodec-hw` |
| Second (1 hardware instance left) | `mediacodec-sw` | `mediacodec`: one hardware, one software |
| Third | `mediacodec-sw` | `mediacodec-sw` |
| Once MediaCodec's software instances run out | `software` | `onError` |

`onLoad`'s `decoder` says what was used: `'mediacodec-hw (c2.qti.vp9.decoder)'`, `'mediacodec-sw (c2.android.vp9.decoder)'`, `'mediacodec (c2.qti.vp9.decoder, c2.android.vp9.decoder)'` for one of each, `'software (libvpx)'` or `'software (dav1d)'`. 10-bit VP9 plays only on MediaCodec, since the C++ core decodes 8-bit VP9.

`mode` applies to the C++ core only; MediaCodec always streams. On a GPU without `GL_EXT_YUV_target`, frames from MediaCodec's software decoders can show BT.601 colours for BT.709 video, because the driver ignores their colour tag.

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

- **HTTP/HTTPS URLs**: `{ uri: 'https://example.com/video.webm', headers: { Authorization: '…' } }`. Read by range requests as it plays, by the platform's HTTP stack (`HttpURLConnection` on Android, `URLSession` on iOS), and kept on disk between plays. See [Streaming and storage](#streaming-and-storage).
- **Anything a data source reads**: `{ uri, dataSource: 'name' }`, for an app that fetches, caches or decrypts the bytes itself. See [Data sources](#data-sources).
- **Local files**: `{ uri: 'file:///path/to/video.webm' }` or an absolute path.
- **Bundled files**: `{ uri: 'asset://videos/glow.webm' }`. On Android this is `android/app/src/main/assets/videos/glow.webm`. On iOS it is `videos/glow.webm` inside the app bundle. On web it is `/videos/glow.webm` from the site root.
- **Android `content://` URIs**, for example from a document picker.
- **`require()`**: pass `Image.resolveAssetSource(require('./glow.webm')).uri`. In debug builds this is a Metro `http://localhost:8081` URL. In release builds it is a file path on iOS and a `res/raw` resource name on Android.

Nothing is copied. Android reads `asset://` files and `res/raw` resources directly from the APK, so they must be stored uncompressed. `.webm` and `.mkv` are uncompressed by default. For other extensions, add them to `android.androidResources.noCompress` in the app's `build.gradle`.

### Streaming and storage

An http(s) source is read by HTTP range requests as it plays, through the same reader as the web player ([alpha-video-source](https://github.com/MediaCacheDev/alpha-video-source)):

- In `stream` and `streamAndPreload` mode (and on the Android hardware path in those modes) playback starts with the first frames. Playing from the start is one request, read on as the player needs it; a seek opens another where it lands, after one for the Cues (the seek index, which a WebM file keeps at the end). `preload` fetches the whole file first, since it decodes every frame before showing one.
- A server without range support works too. The file is then read from the start, and a seek reads it again up to where it lands.
- A dropped connection is retried 3 times, carrying on from where it stopped. A read that still fails is reported through `onError` as `Reading the video failed: …`, and the player stops at the last frame it has.
- If the network is slower than the clip's bitrate, playback waits for frames on the software path. On the Android hardware path its clock can run ahead during the stall and catch up quickly afterwards.

`storage` sets where the bytes are kept between plays:

| Value | Kept |
|---|---|
| `'auto'` (default) | `'disk'` for a URL, `'none'` for a data source |
| `'disk'` | In the app's cache folder: `alpha-video` in `getCacheDir()` on Android, in `Caches` on iOS. The system may clear it when space runs low |
| `'memory'` | In memory, for the app's lifetime |
| `'none'` | Nowhere: each play fetches again |

Storage is partial: it keeps the ranges played so far, and a clip held whole plays again without the network, offline too. `maxStorageBytes` (default 512 MB) caps the store across every clip, evicting the least recently played. Every view shares one disk store and one memory store, so the cap set last applies to it. Stored clips are named by `cacheKey`, else the URL, so a presigned URL that changes on each visit can keep the same copy. A copy is dropped when the server's `ETag`, `Last-Modified` or length no longer matches it. A clip held whole isn't checked again; give a changed file a new `cacheKey` or URL.

Local files, bundled files and `content://` URIs are read in place and never stored.

### Data sources

An app that fetches, caches or decrypts video bytes itself registers a data source under a name, and names it in `source`. The player asks it for byte ranges as it plays and passes `uri` and `headers` through, so the URI can be anything the data source understands:

```tsx
<AlphaVideoView source={{ uri: 'https://cdn.example.com/intro.webm', dataSource: 'media3' }} />
```

A data source's bytes aren't stored by default, since it usually caches them itself; `storage="disk"` or `"memory"` stores them too. On the native views, set `setDataSource("name")` (Android) or `dataSource = "name"` (iOS) before the source, with `setHeaders` or `headers`.

**Android:** implement `dev.mediacache.alphavideo.AlphaVideoDataSource`. `open(uri, position, headers)` starts a read at `position` and returns the file's size and version (`new Info(size, etag, lastModified)`); `read` and `close` follow, and `cancel` (optional) stops a read blocked on the network from another thread. Register a factory, which makes one per player:

```kotlin
AlphaVideoDataSources.register("mine") { MyDataSource() }
```

To read through a Media3 `DataSource.Factory` (OkHttp, Cronet, a `CacheDataSource`), add the adapter, which uses the app's own `androidx.media3:media3-datasource` (compiled against 1.8.0):

```gradle
implementation 'dev.mediacache:alpha-video-media3:1.0.0'
```

```kotlin
AlphaVideoDataSources.register("media3", Media3AlphaVideoDataSource.factory(dataSourceFactory))
```

The built-in fetcher is public too, to register with other timeouts or a user agent: `AlphaVideoHttpDataSource.Factory().setConnectTimeoutMs(…).setReadTimeoutMs(…).setUserAgent(…)`.

**iOS:** conform to `AlphaVideoDataSource`, the role of `AVAssetResourceLoaderDelegate` without the AVFoundation types. Each call reads a window of a few megabytes: call `response` once with the file's size and version, then `data` with the bytes in order, then `completion`. One instance serves every player, so a read mustn't depend on the one before:

```swift
final class MyDataSource: NSObject, AlphaVideoDataSource {
    func read(url: URL, offset: Int64, length: Int64, headers: [String: String],
              response: @escaping (AlphaVideoDataInfo) -> Void,
              data: @escaping (Data) -> Void,
              completion: @escaping (Error?) -> Void) -> AlphaVideoCancellable {
        // response(AlphaVideoDataInfo(size: total, etag: etag, lastModified: nil)), data(…), completion(nil)
    }
}

AlphaVideoDataSources.register(MyDataSource(), name: "mine")
```

### Web

`alpha-video-web` (in [`web/`](web)) plays transparent WebM in every modern browser, whether or not the browser can show transparent video itself. It makes every effort to decode the video: it starts with the fastest way and moves to the next only when one can't show the alpha.

1. **The browser's own `<video>`.** The fastest: no JavaScript runs per frame, and the browser streams and caches. `canPlayType` can't tell whether a browser keeps WebM's alpha; Safari answers `"probably"` and drops it. So the first time a page plays a file, a 599-byte clip is drawn into a canvas and two of its pixels are read. `<video>` is used when that probe shows alpha and the file is a single VP9 track with alpha (Chrome, Edge and Firefox). The first 64 KB of the file tell which kind it is.
2. **WebCodecs.** The C++ core's demuxer, compiled to WebAssembly, splits the file into its colour and alpha bitstreams, reading it by HTTP range requests as it plays: playback starts with the first frames, and a seek fetches only from where it lands. Between plays the bytes are kept in the browser's storage (OPFS, else IndexedDB), so a clip played through plays again offline. It feeds them to two `VideoDecoder`s, one per stream, which can use the device's hardware decoders. WebGL2 draws the pair with the same colour maths as the Android and iOS players. This is what plays dual-stream files in every browser, and every alpha WebM in Safari.
3. **Software decoding in WebAssembly.** The same demuxer feeds FFmpeg's VP9 decoder and dav1d (AV1), each stream in its own Web Worker. On cross-origin isolated pages each decoder also runs threads of its own. This covers what WebCodecs can't decode, such as VP9 profile 1 (RGB-coded) alpha in Safari and Firefox.

Only a file that no engine can decode ends in an error (`NO_ALPHA_PATH`). Each WebAssembly module is loaded only when its engine runs, and nothing needs hosting: the modules and Workers are inside the package. Every test card passes through every engine that can decode it, in Playwright's Chromium, Firefox and WebKit; real Safari and phones are still to be tested. The API, the React component and speed figures are in [web/README.md](web/README.md).

**On react-native-web, `AlphaVideoView` plays through `alpha-video-web`**, which is installed with this package. iOS and Android bundles don't include it: the component imports it from a `.web.tsx` file. It honours `source` (with `source.headers`, sent with every request), `cacheKey` (naming the stored copy), `storage` (`'disk'` is the browser's storage, as `'auto'`), `maxStorageBytes`, `autoPlay`, `loop`, `paused`, `playbackSpeed`, `webDecoder`, `onLoad`, `onError`, `onEnd` and the ref methods. `mode`, `decoder`, `source.dataSource` and the stream and queue props have no effect. `asset://path` is served from the site root.

- **Bundling:** the WebAssembly engines are separate chunks, loaded by `import()` only when a file needs them. With webpack, Vite or esbuild that needs nothing extra.
- **Content-Security-Policy:** the software engine's Workers start from `blob:` URLs, so a page with a CSP must allow `worker-src blob:`.
- **Threads:** a cross-origin isolated page (COOP and COEP headers) lets the software decoders run threads ([details](web/README.md#threads)).
- **The old behaviour:** `webDecoder="browser"` gives a plain `<video>`. Safari then drops the alpha, and dual-stream files show only their colour track.

## Architecture Overview

```
┌──────────────┐    ┌───────────────┐    ┌───────────────────────────────────┐
│  React Native│    │ Native Bridge │    │         Decode Backends           │
│  TypeScript  │◄──►│ iOS / Android │◄──►│                                   │
│  Component   │    │ ViewManager   │    │  ┌─────────────┐ ┌─────────────┐ │
└──────────────┘    └───────────────┘    │  │  Software    │ │  Hardware   │ │
                                         │  │  (C++ core)  │ │  (MediaCodec│ │
                                         │  │  YUVA decode │ │  OES texture│ │
                                         │  │  + GPU upload│ │  compositing│ │
                                         │  └─────────────┘ └─────────────┘ │
                                         └───────────────────────────────────┘
```

### Decode Paths

**Software (C++ core)** — Android and iOS:
1. libwebm demuxes WebM, libvpx decodes VP9 with embedded alpha, or the colour and alpha tracks of a dual-stream VP9 file
2. YUVA420P frames cached in LRU `FrameCache`
3. Uploaded to GPU as 4 textures (Y, U, V, A) per frame
4. Fragment shader converts YUVA to RGBA on Metal (iOS) / OpenGL ES (Android)

**Hardware (MediaCodec)** — Android only:
1. libwebm demuxes WebM into color packets (stream 0) and alpha packets (stream 1): the two tracks of a dual-stream file, or each block's main data and its BlockAdditional in a standard alpha WebM
2. Two `MediaCodec` instances decode in parallel, outputting to `SurfaceTexture` (OES textures)
3. One-frame-per-cycle release gate ensures color and alpha stay in sync
4. Fragment shader samples both OES textures: `gl_FragColor = vec4(color.rgb, alpha.r)`
5. Zero CPU involvement in decoding — all work is on the hardware video decoder

### Core Components

**C++ Core (`WebMAlphaPlayer`)** — software decode path
- libwebm's parser for demuxing, libvpx 1.14.0 for VP9 and dav1d 1.5.1 for AV1
- Multithreaded: decode thread, playback thread, render thread
- LRU `FrameCache` prevents re-decoding during loops
- Zero-copy pipeline from decoder to GPU

**`BufferedDualStreamPlayer`** — hardware decode path (Android)
- Manages demux loop, codec thread, playback loop, and GL thread
- PTS-gated frame release for temporal synchronization
- Handles loop transitions via `MediaCodec.flush()` + demuxer seek

**`SurfaceTextureVideoDecoder`** — codec-agnostic MediaCodec wrapper
- Supports VP9 (`video/x-vnd.on2.vp9`) and AV1 (`video/av01`)
- Opens the decoder `MediaCodecDecoders` reserved for its track (see [Choosing the Decoder](#choosing-the-decoder-android))
- Creates OES texture + SurfaceTexture per decoder instance
- One-frame-per-cycle release gate prevents output buffer flooding

**`AlphaVideoView`** — Android view
- TextureView-based (proper z-ordering, ScrollView support)
- Dedicated GL thread for EGL context and rendering
- Routes each file to MediaCodec or the C++ core (see [Decision Matrix](#decision-matrix))
- Handles view lifecycle (attach/detach/z-index reorder) gracefully

**iOS (Metal)** — software decode only
- `AlphaVideoView`: UIView that resolves the source, applies the mode and runs the global decode queue
- `AlphaVideoMetalView`: MTKView with YUVA-to-RGBA compute shader
- Triple buffering prevents tearing

**Web** — `WebPlayer.web.tsx` renders `alpha-video-web`'s React component: `<video>`, WebCodecs or WebAssembly decoding (see [Web](#web))

## Playback Modes (Software Path)

The `mode` prop controls the software decode path; the default is `stream`. The hardware path on Android always streams, whatever the mode.

```tsx
<AlphaVideoView source={{ uri: 'long-video.webm' }} mode="stream" />
<AlphaVideoView source={{ uri: 'video.webm' }} mode="preload" />
<AlphaVideoView source={{ uri: 'video.webm' }} mode="streamAndPreload" />
```

| Mode | Startup | Memory | Looping | Best For |
|------|---------|--------|---------|----------|
| `stream` (default) | Immediate | Low (cache only) | Re-decode | Long videos, memory-constrained |
| `preload` | Wait for full decode | High (all frames) | Seamless | Short loops, many simultaneous players |
| `streamAndPreload` | Immediate | High (after 1st pass) | Seamless | Fast start + smooth loops |

For http(s) sources, "Immediate" means as soon as the first frames have arrived, and `preload` also waits for the whole file ([Streaming and storage](#streaming-and-storage)).

**Key insight**: `preload` mode has **zero CPU cost during playback** since all frames are already decoded. This means 20+ preloaded videos can play simultaneously with minimal CPU impact (only GPU rendering).

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
| Hardware path (Android) | N/A | Not applicable (MediaCodec handles scheduling) |

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
       -filter_complex "[0:v]format=yuv420p[rgb]; [0:v]alphaextract,format=yuv420p[alpha]" \
       -map "[rgb]" -map "[alpha]" \
       -c:v:0 libsvtav1 -crf 28 -preset 8 \
       -c:v:1 libsvtav1 -crf 28 -preset 8 \
       -metadata:s:v:1 title=Alpha \
       output_dual_av1.webm

# VP9 dual-stream from existing YUVA WebM
ffmpeg -c:v libvpx-vp9 -i input.webm \
       -filter_complex "[0:v]format=yuv420p[rgb]; [0:v]alphaextract,format=yuv420p[alpha]" \
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
| `alphaextract,format=yuv420p` | Extract the alpha channel as a grayscale video. `yuv420p` keeps VP9 at profile 0, which hardware decoders support; `format=gray` makes libvpx encode `gbrp` (profile 1), which many don't |
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

## Codec Build

The library ships with prebuilt libvpx 1.14.0 (the VP9 decoder) and dav1d 1.5.1 (the AV1 decoder, 8- to 12-bit) static libraries. libwebm's WebM parser is in `third_party/libwebm` and compiles with the C++ core. To rebuild the libraries:

```bash
npm run build:codecs:ios          # iOS (device + simulator)
npm run build:codecs:android      # Android (arm64-v8a, armeabi-v7a, x86_64)
npm run build:codecs:all          # All platforms
```

The scripts download the sources themselves. dav1d builds with `meson` and `ninja` (`brew install meson ninja`), and the x86_64 builds (Android and the simulator) need `nasm`. The Android script uses the NDK in `ANDROID_NDK_HOME` (default `$ANDROID_HOME/ndk/27.1.12297006`).

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
scripts/build_xcframework.sh          # prebuilt/AlphaVideoCodecs.xcframework(.zip) for Package.swift
scripts/publish-android.sh local      # dev.mediacache:alpha-video into ~/.m2
scripts/publish-android.sh central    # signed, to the Maven Central Portal
scripts/package_release.sh            # the GitHub Release assets, in dist/release-v<version>/
```

`Package.swift` uses `prebuilt/AlphaVideoCodecs.xcframework` when it exists, and the release zip otherwise. Its checksum must match the zip attached to the GitHub Release, so rebuilding the xcframework means updating the checksum. The same goes for the prebuilt archive and `fetch-prebuilt.sh`, although packaging the same libraries again gives the same archive. `package_release.sh` stops or warns when either checksum is out of date. `publish-android.sh` lists the environment variables it needs for Central.

A release carries the prebuilt archive, `AlphaVideoCodecs.xcframework.zip` and the CocoaPods zip (`alpha-video-<version>-ios.zip`, the source of `alpha-video.podspec`, whose `:sha256` must match it). `pod trunk push alpha-video.podspec` once the release is public.

### Repository layout

| Path | Contents |
|---|---|
| `cpp/` | The C++ core (libwebm demux, libvpx and dav1d decode), shared by Android and iOS; the web runs its demuxer in WebAssembly (`web/wasm`) |
| `android/` | The Android library: `AlphaVideoView`, the MediaCodec dual-stream path and the JNI build |
| `ios/Sources/AlphaVideo/` | The iOS library: `AlphaVideoView` and the Metal renderer |
| `react-native/` | The React Native component (`src/`) and its thin native view managers |
| `web/` | `alpha-video-web`: the browser player, with the C++ demuxer and the decoders in WebAssembly |
| `Package.swift`, `react-native-alpha-video.podspec`, `package.json` | The SwiftPM, CocoaPods and npm packages |

## Demo and related packages

[alpha-video-test](https://github.com/MediaCacheDev/alpha-video-test) is a React Native app (Android, iOS and web) that plays the same video in each of these formats side by side, with FPS, CPU and memory readouts, so you can compare them on your own device.

- [alpha-video-gpu](https://github.com/MediaCacheDev/alpha-video-gpu): ASTC texture arrays in KTX2, drawn without decoding. The lowest CPU and battery use.
- [alpha-video-gpu-encoder](https://github.com/MediaCacheDev/alpha-video-gpu-encoder): makes the KTX2 files for alpha-video-gpu (`texenc`).
- [alpha-video-ffmpegkit](https://github.com/MediaCacheDev/alpha-video-ffmpegkit): the first attempt at WebM alpha, on FFmpegKit, kept as a reference.
- [react-native-apng-avif](https://github.com/MediaCacheDev/react-native-apng-avif): animated APNG and AVIF with alpha.
- [alpha-glow-button](https://github.com/MediaCacheDev/alpha-glow-button): buttons whose glow is a transparent video, played by alpha-video, react-native-apng-avif or a `<video>`.

## Potential future work

Ideas, not plans:

- **Drop FFmpeg on the web too.** Android and iOS decode with libvpx and dav1d directly, and the web already demuxes with libwebm. In WebAssembly libvpx is 1.2-1.9x slower than FFmpeg's own VP9 decoder, even with row multithreading, so the web's software engine keeps FFmpeg's VP9 decoder for now.
- **Hardware decoding of dual-stream files on iOS.** Two VideoToolbox sessions, one per track, composited in the Metal shader, as Android does with MediaCodec, with the C++ core as the fallback. AV1 decodes in hardware on recent Apple chips (A17 Pro, M3 and later). VP9 likely can't, since VideoToolbox's VP9 decoder isn't available to iOS apps.
- **Audio.** Decode a WebM's Opus or Vorbis track, keep video in sync with it, and drop frames when decoding falls behind.
- **More codecs.** VP8 (libvpx already has it; alpha VP8 WebM is common from older encoders) and AV2 once it's final, in software and through platform decoders where they exist.
- **DRM.** Unlikely: Widevine and FairPlay decode into protected surfaces, which the alpha shader can't read.

## License

The code in this repository is MIT (see [LICENSE](LICENSE)).

On Android and iOS the player also contains libwebm's parser and libvpx (BSD-3-Clause, with the WebM patent grant) and dav1d (BSD-2-Clause). Apps built with it must include their notices. The web player, `alpha-video-web`, contains libwebm's parser and dav1d too, and FFmpeg's VP9 decoder, which is LGPL-2.1-or-later; its notices are in [web/THIRD_PARTY_NOTICES.md](web/THIRD_PARTY_NOTICES.md). [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has the versions, build options and source links; the licence texts are in [`licenses/`](licenses).

## Credits

Built with:
- [libwebm](https://chromium.googlesource.com/webm/libwebm) - WebM demuxing
- [libvpx](https://www.webmproject.org/code/) - VP9 codec
- [FFmpeg](https://ffmpeg.org/) - VP9 decoding on the web
- [dav1d](https://code.videolan.org/videolan/dav1d) - AV1 decoder
- [React Native](https://reactnative.dev/) - Cross-platform framework
