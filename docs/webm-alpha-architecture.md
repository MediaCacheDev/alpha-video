# WebM VP9 + Alpha Player Architecture

This document maps the flow and threading model across C++ core, Android, and iOS wrappers for `react-native-webm-alpha-player`.

## High-Level Flow

```
React (JS) WebMAlphaView props
   │  source {uri|path}, cacheKey?, autoPlay, loop, paused
   │
   ├─ Android: RNWebMAlphaViewManager → WebMAlphaTextureView (TextureView)
   │      └─ download/cache (HTTP→cache dir, cacheKey-aware) → loadFromFile
   │
   ├─ iOS: RNWebMAlphaViewManager → WebMAlphaWrapper → WebMAlphaMetalPlayerView
   │      └─ download/cache (NSURLSession→Caches, cacheKey-aware) → loadFromPath
   │
   └─ C++ core (WebMAlphaPlayer)
          ├─ openStreams / initCodecs (FFmpeg)
          ├─ decode thread(s) → YUVA frames → FrameCache (LRU)
          └─ playback loop → renders into Metal/TextureView surface
```

## Components and Responsibilities

- **JS/React side**: Sends props (`source`, `cacheKey`, `autoPlay`, `loop`, `paused`, `playbackSpeed`) to native view managers. No decoding logic here.
- **Android (Java)**
  - `RNWebMAlphaViewManager` owns `WebMAlphaTextureView` and a download executor (4 threads).
  - `setSource`: HTTP URLs are downloaded to cache (honors `cacheKey` → nested cache dirs). `file://` and `asset://` are loaded directly.
  - Caching: temp file → rename to cache file; reuses cached file if present. Passes file path to native texture view.
  - Lifecycle: `createViewInstance` (TextureView), `setSource`, optional `onDropViewInstance` → `release`.
- **iOS (ObjC++)**
  - `RNWebMAlphaViewManager` creates `WebMAlphaWrapper` holding `WebMAlphaMetalPlayerView`.
  - `source` setter triggers `loadSource`:
    - HTTP URLs download via `NSURLSession`, cached to `Caches` (subdirs from `cacheKey`), then loaded.
    - `file://` and `asset://` resolved to bundle/FS paths.
  - Exposes methods via `RCT_EXPORT_METHOD` for play/pause/stop/seek.
  - Delegate callbacks bridge events (`onLoad`, `onError`, `onEnd`).
- **C++ core (cross-platform)**
  - `WebMAlphaPlayer` wraps FFmpeg (avformat/avcodec/avutil/swscale) and provides:
    - `openStreams`, `initCodecs`, `loadFromFile/loadFromMemory/loadFromUrl`.
    - `decodeAllFrames` (optional preload) and on-demand `decodeFrame`.
    - YUVA frame extraction with alpha handling (built-in YUVA420P or separate alpha track).
    - `FrameCache` (LRU, mutex-protected) for decoded frames.
    - Playback loop that clocks frames at FPS, pulls from cache/decodes, and emits to the platform renderer.
    - `PlayerConfig`: cache size, `preloadAllFrames`, playback speed, etc.


## Threading Model

```
JS thread
  └─ Calls native props/methods

Android
  ├─ UI thread: View creation, prop updates, attach/detach, forwards file paths to native view
  ├─ Download executor (fixed 4 threads): HTTP fetch → cache file → post to UI to load
  └─ Native decoder threads (C++): see core below

iOS
  ├─ Main thread: View creation, prop updates, NSURLSession callbacks dispatch to main for load
  └─ Native decoder threads (C++): see core below

C++ core (both platforms)
  ├─ Decoder thread: FFmpeg demux/decode → YUVA frames → FrameCache
  ├─ Playback/render thread: time-based loop → fetch from cache (or decode) → render to surface
  └─ Optional preload thread: `decodeAllFrames` to fill cache up-front
```


## Thread Lifecycle & Interaction


```text
JS/Main thread                 Playback thread                Decode path (same thread)        Resources
--------------                 ----------------                -------------------------        ---------
play()                     →  spawn playbackThread_      →   (none yet)                       FrameCache constructed
                                          |  playing_=true
                                          |  shouldStop_=false
                                          |
                                          |  playbackLoop start
                                          |    per frame slot:
                                          |    ├─ getFrame(idx)
                                          |    │    ├─ cache hit → emitFrame → render
                                          |    │    └─ cache miss → decodeFrame(idx)
                                          |    │          ├─ lock decodeMutex_
                                          |    │          ├─ av_seek_frame / avcodec_send/receive
                                          |    │          ├─ YUVA copy → FrameCache
                                          |    │          └─ unlock decodeMutex_
                                          |    └─ advance idx
                                          |
pause() / seekToFrame()    →  playing_=false / set currentFrame / optional inline getFrame
                                          |  (thread idles if paused)
                                          |
stop()                     →  shouldStop_=true
                                          |  playing_=false
                                          |  currentFrame_=0
                                          |  join playbackThread_
                                          |  emit stop
                                          |
unload()                   →  shouldStop_=true
                                             try_lock decodeMutex_ (wait briefly)
                                             join decodeThread_ (if ever spawned)
                                             join playbackThread_ (if running)
                                             close codecs / free formatCtx / cleanup IO
                                             FrameCache.clear(), metadata reset, loaded_=false
```

Notes
- `playbackThread_` is the only long-lived thread; it is created on first `play()` and joined on `stop()` or `unload()`.
- `decodeAllFrames()` runs on the caller thread (often JS/main during load) and holds `decodeMutex_`; there is no dedicated decode worker today.
- `decodeFrame()` also runs on the caller (typically the playback thread on cache miss, or main thread during `seekToFrame`).

## Detailed Call Order (play from cached file)

1) `WebMAlphaPlayer()` → `initializeFFmpeg()` (once per process) → create `FrameCache`.
2) `loadFromFile(path)` (caller thread)
   - Calls `unload()` (joins any threads, clears codecs/cache).
   - Alloc `formatCtx_` → `openStreams()` → `initCodecs()`; sets `loaded_=true`.
   - If `preloadAllFrames` is true, runs `decodeAllFrames()` inline on caller thread (holds `decodeMutex_`).
3) `play()` (caller thread)
   - Sets `playing_=true`, `shouldStop_=false`.
   - If `playbackThread_` is null, spawns it running `playbackLoop`.
4) `playbackLoop` (background thread)
   - Computes frame interval from `frameRate_ * playbackSpeed`.
   - Per frame slot: `getFrame(idx)` → cache hit or `decodeFrame(idx)` (holds `decodeMutex_`, seeks/decodes via FFmpeg) → `emitFrame`.
   - Increments `currentFrame_`; if at end, either loops or emits `ended` and stops.
5) Control paths
   - `pause()` sets `playing_=false` (thread stays alive, idles).
   - `seekToFrame(n)` on caller thread sets `currentFrame_`, calls `getFrame(n)` (may decode inline), then `emitFrame` and `emitEvent("seek")`.
6) Shutdown paths
   - `stop()` sets `shouldStop_=true`, `playing_=false`, resets `currentFrame_`, notifies CV, joins `playbackThread_`, emits `stop`.
   - `unload()` (called before new loads or on view release) sets `shouldStop_=true`, waits briefly on `decodeMutex_`, joins `decodeThread_` (if used) and `playbackThread_`, frees codecs/formatCtx/IO, clears cache, resets metadata, sets `loaded_=false`.

Thread lifecycle summary: `playbackThread_` is created on first `play()`, runs until `stop()` or `unload()` joins it. `decodeAllFrames()` and `decodeFrame()` execute on the calling thread (often the playback thread on cache miss), so there is no separate long-lived decode worker unless you introduce one.
```

## Data Paths

1) **Asset acquisition**
   - HTTP: downloaded to cache (cacheKey → subpath). If exists, skip download.
   - Local: `file://` or bundle asset resolved directly.
2) **Decode**
   - C++ opens file, finds color + alpha streams, initializes codecs.
   - Frames decoded to YUVA; alpha merged (built-in or secondary stream).
3) **Cache**
   - `FrameCache` stores decoded frames keyed by frame index; LRU eviction.
4) **Playback**
   - Playback loop picks frames at target FPS, emits to renderer (TextureView/Metal).
   - `paused` halts loop; `seekToFrame` jumps decode cursor.

## Lifecycle

- **Mount**: view created → source set → (download if remote) → `loadFromPath` → C++ `loadFromFile` → `onLoad` fired.
- **Play**: autoPlay or explicit play → playback thread runs until paused/stopped/end.
- **Pause/Stop**: pause halts loop; stop unloads, frees codecs and buffers.
- **Unmount**: view release cleans native resources; downloads are detached via executor cleanup on Android and session tasks on iOS.

## Cache Key Behavior

- JS `cacheKey` defines subpath under platform cache dir. Example: `videos/org/collection/asset/glows/hash-60x60.webm`.
- Android: directories auto-created; temp file uses hashed suffix; rename to final cache file.
- iOS: parent directories created; file saved atomically to final cache path.

## Error and Event Flow

- Download errors → `onError` (JS) with message.
- Decode errors → C++ delegate → platform wrapper → `onError`.
- Successful load → `onLoad` (totalFrames, width, height).
- Playback end → `onEnd`.

## Key Interactions

```text
JS sets source + cacheKey
   ↓
Platform ViewManager
   ↓ create view, set props
   ↓
if remote → download thread(s)
   ↓ cache file
   ↓ UI thread: loadFromPath on native view
   ↓
C++ player: loadFromFile → open streams → init codecs
   ↓
Decoder thread: decode frames → FrameCache
   ↓
Playback loop: schedule frames → render surface
   ↓
Events: onLoad/onError/onEnd → JS
```
