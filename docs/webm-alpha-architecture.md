# WebM Alpha Player Architecture

This document maps the flow and threading model across C++ core, Android, and iOS wrappers for `react-native-webm-alpha-player`.

## Two Decode Paths

The player has two completely separate decode backends, selected automatically based on the file's stream layout and available hardware:

| | Software Path | Hardware Path |
|---|---|---|
| **File format** | Single VP9 stream with YUVA420P alpha | Two VP9 or AV1 streams (color + alpha) |
| **Decoder** | FFmpeg (C++, libvpx) | Android MediaCodec (hardware or software codec) |
| **Rendering** | YUVA textures → fragment shader | OES textures → fragment shader |
| **Platforms** | iOS + Android | Android only |
| **Looping** | Seamless (frames stay in cache) | Tiny stall (MediaCodec flush + refill) |
| **Key class** | `WebMAlphaPlayer` (C++) | `BufferedDualStreamPlayer` (Java) |

Detection happens in `WebMAlphaTextureView.loadFromFile()`: the JNI function `nativeIsDualStream()` counts video streams in the WebM container. If there are 2 video streams and a hardware decoder is available, the dual-stream path is used.

## High-Level Flow

```
React (JS) WebMAlphaView props
   │  source {uri|path}, cacheKey?, autoPlay, loop, paused, mode, verbose
   │
   ├─ Android: RNWebMAlphaViewManager → WebMAlphaTextureView (TextureView)
   │      ├─ download/cache (HTTP→cache dir, cacheKey-aware) → loadFromFile
   │      ├─ if dual-stream + HW decoder → BufferedDualStreamPlayer (MediaCodec)
   │      └─ if single-stream or no HW   → WebMAlphaPlayer (C++/FFmpeg)
   │
   ├─ iOS: RNWebMAlphaViewManager → WebMAlphaWrapper → WebMAlphaMetalPlayerView
   │      └─ download/cache (NSURLSession→Caches) → loadFromPath → WebMAlphaPlayer (C++)
   │
   └─ Web: HTML5 <video> element fallback
```

## Components and Responsibilities

### JS/React

- Sends props (`source`, `cacheKey`, `autoPlay`, `loop`, `paused`, `playbackSpeed`, `mode`, `verbose`) to native view managers.
- Exposes `getDecoderStatus()`, `getDecoderInfo()`, `getMaxDecoderInstances()` for runtime capability queries (Android).
- No decoding logic; platform detection is automatic.

### Android (Java)

- **`RNWebMAlphaViewManager`** — Creates `WebMAlphaTextureView`, handles prop dispatch, manages download executor (4 threads).
- **`WebMAlphaTextureView`** — TextureView subclass, owns the EGL context and GL render thread. Routes to software or hardware path based on stream detection. Handles view lifecycle (attach/detach/z-index reorder) with deferred source reloading.
- **`BufferedDualStreamPlayer`** — Hardware decode orchestrator. Manages the demux loop, codec thread, playback loop, and delegates rendering to the GL thread. Handles loop transitions (flush + seek).
- **`SurfaceTextureVideoDecoder`** — Codec-agnostic MediaCodec wrapper. Supports VP9 and AV1. Creates OES texture + SurfaceTexture per instance. Implements one-frame-per-cycle release gate for synchronized output.
- **`RNWebMAlphaPlayerModule`** — Native module exposing `getDecoderInfo()`, `getDecoderStatus()`, `getMaxDecoderInstances()` to JS.

### iOS (Objective-C++)

- **`RNWebMAlphaViewManager`** — Creates `WebMAlphaWrapper` holding `WebMAlphaMetalPlayerView`.
- Source download via `NSURLSession`, cached to `Caches` (subdirs from `cacheKey`).
- Exposes play/pause/stop/seek via `RCT_EXPORT_METHOD`. Delegate callbacks bridge events to JS.

### C++ Core (cross-platform, software path)

- **`WebMAlphaPlayer`** — FFmpeg (avformat/avcodec/avutil/swscale) wrapper:
  - `openStreams`, `initCodecs`, `loadFromFile/loadFromMemory/loadFromUrl`
  - `decodeAllFrames` (preload) and on-demand `decodeFrame`
  - YUVA frame extraction with alpha handling (embedded YUVA420P or separate alpha track)
  - `FrameCache` (LRU, mutex-protected) for decoded frames
  - Playback loop clocking frames at target FPS
  - `PlayerConfig`: cache size, `preloadAllFrames`, playback speed

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

This causes a brief stall (~10-20ms) as the MediaCodec pipelines refill. Inherent to hardware decode; the software path avoids this since frames stay in the LRU cache.

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
1. `RNWebMAlphaViewManager.createViewInstance()` → creates `WebMAlphaTextureView`
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
   - HTTP: downloaded to cache (cacheKey → subpath). Reuses cached file if present.
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
- Android: directories auto-created; temp file uses hashed suffix; rename to final cache file.
- iOS: parent directories created; file saved atomically to final cache path.

## Error and Event Flow

- Download errors → `onError` (JS) with message.
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
Auto-detect: nativeIsDualStream?
   ├─ YES + HW decoder available:
   │    BufferedDualStreamPlayer
   │    ├─ demux thread: FFmpeg → raw packets
   │    ├─ 2× MediaCodec → OES textures
   │    └─ GL thread: composite + present
   │
   └─ NO (or no HW decoder):
        WebMAlphaPlayer (C++)
        ├─ decode thread: FFmpeg → YUVA frames → FrameCache
        └─ playback loop → texture upload → GPU shader → present
   ↓
Events: onLoad / onError / onEnd → JS
```
