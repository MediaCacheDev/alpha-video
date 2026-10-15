# WebM Alpha Player Architecture

This document maps the flow and threading model across C++ core, Android, and iOS wrappers for `react-native-alpha-video`.

## Two Decode Paths

The player has two completely separate decode backends, selected automatically based on the file's stream layout and available hardware:

| | Software Path | Hardware Path |
|---|---|---|
| **File format** | Single VP9 stream with YUVA420P alpha, or two VP9 (8-bit) or AV1 (8- to 12-bit) streams | Two VP9 or AV1 streams (color + alpha), or a single VP9 stream with YUVA420P alpha |
| **Used for** | iOS always; Android when MediaCodec can't decode the file | Android when it can |
| **Decoder** | FFmpeg (C++; libvpx for VP9, dav1d for AV1) | Android MediaCodec (hardware or software codec) |
| **Rendering** | YUVA textures → fragment shader | OES textures → fragment shader |
| **Platforms** | iOS + Android | Android only |
| **Looping** | Seamless (frames stay in cache) | Seamless |
| **Key class** | `WebMAlphaPlayer` (C++) | `BufferedDualStreamPlayer` (Java) |

Detection happens in `AlphaVideoView.loadProbedSource()`: the JNI function `nativeColorAlphaTracks()` lists the colour and alpha bitstreams with their profiles. They come from 2 VP9/AV1 tracks, or from a standard alpha WebM: one VP9 track whose alpha is a second VP9 stream in each block's BlockAdditional. The demuxer then hands each block's main data to the colour decoder and its BlockAdditional to the alpha decoder, as if they were 2 tracks.

The `decoder` prop then picks the engine. `MediaCodecDecoders.reserve()` takes a decoder for each track, of the kind asked for (hardware, software, or hardware where it can), that lists the track's profile and has an instance free under its `getMaxSupportedInstances()`; it counts the instances players hold per decoder name. Under `auto`, a file goes to MediaCodec if hardware decoders can be reserved for both tracks, else software ones, and otherwise to the C++ core. A VP9 profile MediaCodec doesn't list (usually profile 1, the 4:4:4 a `gbrp` alpha track encodes to) therefore goes to the C++ core, and so does a file once other players hold the decoders' instances (2 each). A forced value reports an error rather than fall back. The player holds its decoders until `unload()`, and `onLoad` reports them.

## High-Level Flow

```
React (JS) AlphaVideoView props
   │  source {uri|path}, cacheKey?, autoPlay, loop, paused, mode, decoder, verbose
   │
   ├─ Android: AlphaVideoViewManager → AlphaVideoView (TextureView)
   │      ├─ download/cache (WebMDownloader, cacheKey-aware) → load, or loadFromDownload while it runs
   │      ├─ if colour + alpha VP9/AV1 streams + MediaCodecDecoders reserves 2 decoders → BufferedDualStreamPlayer (MediaCodec)
   │      └─ otherwise (decoder auto or ffmpeg)                                      → WebMAlphaPlayer (C++/FFmpeg)
   │
   ├─ iOS: RNAlphaVideoViewManager → RNAlphaVideoView (an AlphaVideoView) → AlphaVideoMetalView
   │      └─ download/cache (AlphaVideoDownload, NSURLSession→Caches) → loadFromPath / loadFromDownloadPath → WebMAlphaPlayer (C++)
   │
   └─ Web: WebPlayer.web.tsx → alpha-video-web: <video> where the browser keeps the alpha,
          otherwise the C++ demuxer in WASM → WebCodecs, or WASM decoders in Workers → WebGL2
```

## Components and Responsibilities

### JS/React

- Sends props (`source`, `cacheKey`, `autoPlay`, `loop`, `paused`, `playbackSpeed`, `mode`, `decoder`, `verbose`) to native view managers.
- Exposes `getDecoderStatus()`, `getDecoderInfo()`, `getMaxDecoderInstances()` for runtime capability queries (Android).
- No decoding logic; platform detection is automatic.

### Android (Java)

- **`AlphaVideoViewManager`** — Creates `AlphaVideoView`, handles prop dispatch, manages download executor (4 threads).
- **`AlphaVideoView`** — TextureView subclass, owns the EGL context and GL render thread. Routes to software or hardware path based on stream detection. Handles view lifecycle (attach/detach/z-index reorder) with deferred source reloading.
- **`BufferedDualStreamPlayer`** — Hardware decode orchestrator. Manages the demux loop, codec thread, playback loop, and delegates rendering to the GL thread. Handles loop transitions (flush + seek).
- **`MediaCodecDecoders`** — Picks the MediaCodec decoder for each track by kind, profile and free instances, and counts the instances players hold.
- **`SurfaceTextureVideoDecoder`** — Codec-agnostic MediaCodec wrapper around the decoder it is given. Supports VP9 and AV1. Creates OES texture + SurfaceTexture per instance. Implements one-frame-per-cycle release gate for synchronized output.
- **`AlphaVideoModule`** — Native module exposing `getDecoderInfo()`, `getDecoderStatus()`, `getMaxDecoderInstances()` to JS.

### iOS (Objective-C++)

- **`RNAlphaVideoViewManager`** — Creates `RNAlphaVideoView`, an `AlphaVideoView` (the standalone iOS view, which holds `AlphaVideoMetalView`) that turns its delegate calls into `onLoad`, `onError` and `onEnd`.
- Source download via `NSURLSession` (`AlphaVideoDownload` in `AlphaVideoPlayer.mm`), cached to `Caches` (subdirs from `cacheKey`).
- Exposes play/pause/stop/seek via `RCT_EXPORT_METHOD`. Delegate callbacks bridge events to JS.

### C++ Core (cross-platform, software path)

- **`WebMAlphaPlayer`** — FFmpeg (avformat/avcodec/avutil) wrapper:
  - `openStreams`, `initCodecs` (a decoder per track: libvpx-vp9 for VP9, libdav1d for AV1), `loadFromFile/loadFromMemory/loadFromFileRange/loadFromDownload`
  - dav1d holds frames back with frame threading, so every decode loop drains the decoders (an empty packet) at the end of the file
  - `decodeAllFrames` (preload) and on-demand `decodeFrame`
  - YUVA frame extraction with alpha handling (embedded YUVA420P or separate alpha track)
  - `FrameCache` (LRU, mutex-protected) for decoded frames
  - Playback loop clocking frames at target FPS
  - `PlayerConfig`: cache size, `preloadAllFrames`, playback speed
- **`GrowingFile`** — A download in progress, registered under its cache path. The platform downloader reports each chunk it writes to a part file; `openDownloadIO` (in `FileRangeIO`) gives FFmpeg an AVIO context that waits for bytes not yet written, instead of hitting EOF.

## Threading Model

```
JS thread
  └─ Calls native props/methods

Android — Software Path
  ├─ UI thread: View creation, prop updates, attach/detach
  ├─ Download executor (4 threads): HTTP fetch → cache file → post to UI
  ├─ GL render thread: EGL context, texture upload, YUVA→RGBA shader, present
  ├─ Native load executor: nativeLoadFromFile, nativeStop, nativeUnload (async)
  └─ C++ threads: playback loop, decode (on cache miss)

Android — Hardware (Dual-Stream) Path
  ├─ UI thread: View creation, prop updates, attach/detach
  ├─ Download executor (4 threads): HTTP fetch → cache file
  ├─ GL render thread: EGL context, OES texture creation, compositing shader, present
  ├─ Demux thread: FFmpeg reads packets, feeds to MediaCodec input buffers
  ├─ Codec callback threads (2): MediaCodec async output callbacks (one per decoder)
  └─ Playback thread: PTS-gated frame release, timing control, render signaling

iOS
  ├─ Main thread: View creation, prop updates, NSURLSession callbacks
  ├─ Metal render thread: MTKView delegate, triple-buffered YUVA→RGBA
  └─ C++ threads: playback loop, decode

Web
  └─ Main thread: <video> element, standard browser media pipeline
```

## Hardware Decode Path — Detailed Flow

### Initialization

```
loadDualStreamFile(path)
  └─ renderThread.execute:
       1. Create BufferedDualStreamPlayer
       2. Set loop, verbose, GL executor, callbacks
       3. bufferedPlayer.loadFromFile(path):
            a. nativeCreateDemuxer (JNI) → FFmpeg opens WebM, finds 2 video streams
            b. nativeGetVideoInfo → [width, height, frames, fps*1000, streamCount, codecId]
            c. Map codecId to MIME type (VP9=167 → "video/x-vnd.on2.vp9", AV1=226 → "video/av01")
            d. Create 2 SurfaceTextureVideoDecoder instances (color + alpha)
                 - Each creates: OES texture → SurfaceTexture → Surface → MediaCodec
                 - MediaCodec configured with Surface output (GPU-accelerated)
            e. Start both decoders
       4. bufferedPlayer.play() → starts demux + playback threads
```

### Playback Loop

```
Demux Thread                     Codec Callbacks              Playback Thread              GL Thread
------------                     ---------------              ---------------              ---------
nativeReadPacket()               onOutputBufferAvailable      (waits for initial frames)
  ├─ color pkt → colorDecoder      ├─ hold buffer until       per frame interval:
  └─ alpha pkt → alphaDecoder      │   PTS <= targetTime        ├─ advance targetPTS
                                    └─ release to Surface        ├─ checkHeldBuffer (color)
loop until EOF or stopped             (one per cycle)            ├─ checkHeldBuffer (alpha)
                                                                 ├─ if both have new frame:
                                                                 │    signal GL thread
                                                                 └─ sleep to next frame
                                                                                            consumeFrames():
                                                                                              colorTex.updateTexImage()
                                                                                              alphaTex.updateTexImage()
                                                                                              getTransformMatrix (both)
                                                                                              draw with OES shader:
                                                                                                color.rgb + alpha.r → RGBA
                                                                                              eglSwapBuffers()
```

### Synchronization Mechanisms

1. **PTS Gating** — `checkHeldBuffer()` holds each MediaCodec output buffer until its presentation timestamp is at or before the current playback target. Prevents frame skipping.

2. **One-Frame-Per-Cycle Release** — Each decoder releases at most one frame to its SurfaceTexture per render cycle. Prevents faster decoders from flooding the output.

3. **Paired Consumption** — `consumeFrames()` only calls `updateTexImage()` on both textures when *both* decoders have confirmed a new frame release. Guarantees temporal alignment between color and alpha.

### Loop Transition

```
Demux thread detects EOF
  └─ handleLoopTransition():
       1. Pause demux + playback
       2. colorDecoder.flush() → MediaCodec reset, buffers cleared
       3. alphaDecoder.flush() → MediaCodec reset, buffers cleared
       4. nativeSeekToStart(demuxer) → FFmpeg av_seek_frame to 0
       5. Resume demux + playback from frame 0
```

The flush happens once the last frame has been drawn, and the new pass is held until the playback loop asks for frame 0, so the last frame stays up for its full duration while the decoders refill. The loop is seamless, as on the software path.

## Software Decode Path — Detailed Flow

```
Thread Lifecycle:

JS/Main thread                 Playback thread                Decode (inline)              Resources
--------------                 ----------------               ---------------              ---------
loadFromFile(path)         →   (not started yet)              FFmpeg open, init codecs     FrameCache created
  if preload:                                                 decodeAllFrames (inline)
play()                     →   spawn playbackThread           
                                  playing=true
                                  per frame slot:
                                    getFrame(idx)
                                      cache hit → emit        
                                      cache miss → decode     av_seek + avcodec decode
                                    advance idx               copy YUVA → FrameCache

pause()                    →   playing=false (thread idles)
seekToFrame(n)             →   set currentFrame, inline decode if needed
stop()                     →   shouldStop=true, join thread
unload()                   →   shouldStop=true, join threads, free codecs, clear cache
```

## View Lifecycle (Android)

### Normal Flow
1. `AlphaVideoViewManager.createViewInstance()` → creates `AlphaVideoView`
2. Props set (`setSource`, `setMode`, `setVerbose`, etc.)
3. `onSurfaceTextureAvailable` → create render thread → init EGL
4. Source downloaded/cached → `loadFromFile()` → auto-detect → software or hardware path
5. `onLoad` event dispatched to JS with video dimensions
6. Playback starts (if `autoPlay`)

### Z-Index Reorder (detach → re-attach)
React Native may detach and re-attach the view during z-index changes:
1. `onDetachedFromWindow` → `softRelease()` stops player, clears state
2. `onSurfaceTextureDestroyed` → EGL context destroyed
3. `onAttachedToWindow` → resets `isReleased` flag
4. `onSurfaceTextureAvailable` → new EGL context → `loadFromFile()` re-detects stream type → full reload
5. Playback resumes (~20-40ms reload, imperceptible)

### Unmount
1. `onDetachedFromWindow` → `softRelease()`
2. `onDropViewInstance` → `release()` → destroy native player, free all resources

## Data Paths

1. **Asset acquisition**
   - HTTP: downloaded to cache (cacheKey → subpath). Reuses cached file if present. One download per cache path, shared by every view asking for it. In `stream`/`streamAndPreload` mode the view loads as soon as the first bytes are written and reads the part file through `GrowingFile`; `preload` loads the finished file.
   - Local: `file://` or bundle asset resolved directly.
2. **Decode**
   - Software: C++ opens file, finds color + alpha streams, initializes codecs. Frames decoded to YUVA.
   - Hardware: FFmpeg demuxes raw packets, MediaCodec decodes to SurfaceTexture.
3. **Cache**
   - Software: `FrameCache` stores decoded frames keyed by frame index; LRU eviction.
   - Hardware: No frame cache; decoded frames go directly to OES textures.
4. **Render**
   - Software: YUVA textures uploaded per frame, fragment shader converts to RGBA.
   - Hardware: OES textures sampled directly, fragment shader composites color + alpha.

## Cache Key Behavior

- JS `cacheKey` defines subpath under platform cache dir. Example: `videos/org/collection/asset/glows/hash-60x60.webm`.
- Both platforms: directories auto-created; the download goes to a `.part` file in the same directory, renamed to the final cache path when complete and deleted if it fails. Readers hold their own fd, so the rename doesn't disturb a view that is already playing it.

## Error and Event Flow

- Download errors → `onError` (JS) with message, also when playback had already started. Reads past the last byte of a failed download return `EIO`, and the C++ decode loop stops instead of looping over the partial file.
- Decode errors → C++ delegate / Java callback → `onError`.
- Successful load → `onLoad` (totalFrames, width, height).
- Playback end → `onEnd`.

## Key Interactions

```
JS sets source + cacheKey
   ↓
Platform ViewManager
   ↓ create view, set props
   ↓
if remote → download thread(s)
   ↓ cache file
   ↓ UI thread: loadFromFile
   ↓
Auto-detect: nativeColorAlphaTracks?
   ├─ YES + profiles MediaCodec lists + HW decoder available (VP9: with 2 instances free):
   │    BufferedDualStreamPlayer
   │    ├─ demux thread: FFmpeg → raw packets
   │    ├─ 2× MediaCodec → OES textures
   │    └─ GL thread: composite + present
   │
   └─ otherwise:
        WebMAlphaPlayer (C++)
        ├─ decode thread: FFmpeg → YUVA frames → FrameCache
        └─ playback loop → texture upload → GPU shader → present
   ↓
Events: onLoad / onError / onEnd → JS
```
