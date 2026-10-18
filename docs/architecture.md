# WebM Alpha Player Architecture

This document maps the flow and threading model across C++ core, Android, and iOS wrappers for `react-native-alpha-video`.

## Two Decode Paths

The player has two completely separate decode backends, selected automatically based on the file's stream layout and available hardware:

| | Software Path | Hardware Path |
|---|---|---|
| **File format** | Single VP9 stream with YUVA420P alpha, or two VP9 (8-bit) or AV1 (8- to 12-bit) streams | Two VP9 or AV1 streams (color + alpha), or a single VP9 stream with YUVA420P alpha |
| **Used for** | iOS always; Android when MediaCodec can't decode the file | Android when it can |
| **Decoder** | C++ core (libvpx for VP9, dav1d for AV1) | Android MediaCodec (hardware or software codec) |
| **Rendering** | YUVA textures → fragment shader | OES textures → fragment shader |
| **Platforms** | iOS + Android | Android only |
| **Looping** | Seamless (frames stay in cache) | Seamless |
| **Key class** | `WebMAlphaPlayer` (C++) | `BufferedDualStreamPlayer` (Java) |

Detection happens in `AlphaVideoView.loadProbedSource()`: the JNI function `nativeColorAlphaTracks()` lists the colour and alpha bitstreams with their profiles. They come from 2 VP9/AV1 tracks, or from a standard alpha WebM: one VP9 track whose alpha is a second VP9 stream in each block's BlockAdditional. The demuxer then hands each block's main data to the colour decoder and its BlockAdditional to the alpha decoder, as if they were 2 tracks.

The `decoder` prop then picks the engine. `MediaCodecDecoders.reserve()` takes a decoder for each track, of the kind asked for (hardware, software, or hardware where it can), that lists the track's profile and has an instance free under its `getMaxSupportedInstances()`; it counts the instances players hold per decoder name. Under `auto`, a file goes to MediaCodec if hardware decoders can be reserved for both tracks, else software ones, and otherwise to the C++ core. A VP9 profile MediaCodec doesn't list (usually profile 1, the 4:4:4 a `gbrp` alpha track encodes to) therefore goes to the C++ core, and so does a file once other players hold the decoders' instances (2 each). A forced value reports an error rather than fall back. The player holds its decoders until `unload()`, and `onLoad` reports them.

## High-Level Flow

```
React (JS) AlphaVideoView props
   │  source {uri, headers?, dataSource?}, cacheKey?, storage, maxStorageBytes, autoPlay, loop, paused, mode, decoder, verbose
   │
   ├─ Android: AlphaVideoViewManager → AlphaVideoView (TextureView)
   │      ├─ http(s) or dataSource → SourceReader (RangeReader over AlphaVideoHttpDataSource or the app's AlphaVideoDataSource, store) → load from the reader
   │      ├─ if colour + alpha VP9/AV1 streams + MediaCodecDecoders reserves 2 decoders → BufferedDualStreamPlayer (MediaCodec)
   │      └─ otherwise (decoder auto or software)                                    → WebMAlphaPlayer (C++)
   │
   ├─ iOS: RNAlphaVideoViewManager → RNAlphaVideoView (an AlphaVideoView) → AlphaVideoMetalView
   │      └─ http(s) or dataSource → AlphaVideoSourceReader (RangeReader over URLSession or the app's AlphaVideoDataSource, store) → loadFromReader → WebMAlphaPlayer (C++)
   │
   └─ Web: WebPlayer.web.tsx → alpha-video-web: <video> where the browser keeps the alpha,
          otherwise the C++ demuxer in WASM → WebCodecs, or WASM decoders in Workers → WebGL2
```

## Components and Responsibilities

### JS/React

- Sends props (`source` with its `headers` flattened to `[name, value, ...]` and `dataSource`, `cacheKey`, `storage`, `maxStorageBytes`, `autoPlay`, `loop`, `paused`, `playbackSpeed`, `mode`, `decoder`, `verbose`) to native view managers.
- Exposes `getDecoderStatus()`, `getDecoderInfo()`, `getMaxDecoderInstances()` for runtime capability queries (Android).
- No decoding logic; platform detection is automatic.

### Android (Java)

- **`AlphaVideoViewManager`** — Creates `AlphaVideoView` and handles prop dispatch; `source.headers` and `source.dataSource` are set before the source.
- **`SourceReader`** — Opens a C++ `avsource::RangeReader` (from alpha-video-source, vendored in `third_party/alpha-video-source`) over an `AlphaVideoDataSource` through a JNI `Upstream`, with the disk store (`cacheDir/alpha-video`), the memory store or none. The view and each player hold their own reference to it, so a new source doesn't close a reader a player is still reading.
- **`AlphaVideoHttpDataSource`** — The built-in fetcher: `HttpURLConnection` with `Range: bytes=N-`, the source's headers, redirects, and 200 (skipped to the position) and 416 answers. `AlphaVideoDataSources` holds the app's data sources by name; `alpha-video-media3` adapts a Media3 `DataSource.Factory`.
- **`AlphaVideoView`** — TextureView subclass, owns the EGL context and GL render thread. Routes to software or hardware path based on stream detection. Handles view lifecycle (attach/detach/z-index reorder) with deferred source reloading.
- **`BufferedDualStreamPlayer`** — Hardware decode orchestrator. Manages the demux loop, codec thread, playback loop, and delegates rendering to the GL thread. Handles loop transitions (flush + seek).
- **`MediaCodecDecoders`** — Picks the MediaCodec decoder for each track by kind, profile and free instances, and counts the instances players hold.
- **`SurfaceTextureVideoDecoder`** — Codec-agnostic MediaCodec wrapper around the decoder it is given. Supports VP9 and AV1. Creates OES texture + SurfaceTexture per instance. Implements one-frame-per-cycle release gate for synchronized output.
- **`AlphaVideoModule`** — Native module exposing `getDecoderInfo()`, `getDecoderStatus()`, `getMaxDecoderInstances()` to JS.

### iOS (Objective-C++)

- **`RNAlphaVideoViewManager`** — Creates `RNAlphaVideoView`, an `AlphaVideoView` (the standalone iOS view, which holds `AlphaVideoMetalView`) that turns its delegate calls into `onLoad`, `onError` and `onEnd`.
- **`AlphaVideoSourceReader`** (`AlphaVideoSource.mm`) — Opens a `RangeReader` with the disk store (`Caches/alpha-video`), the memory store or none, over a `URLSessionUpstream` (an open-ended `Range` request per read, suspended while 1 MB is waiting and resumed under 256 KB) or a `DataSourceUpstream`, which asks the app's `AlphaVideoDataSource` for 4 MB windows. The view opens it off the main thread.
- Exposes play/pause/stop/seek via `RCT_EXPORT_METHOD`. Delegate callbacks bridge events to JS.

### C++ Core (cross-platform, software path)

- **`AlphaDemuxer`** — Reads the colour and alpha bitstreams as packets with libwebm's `webm_parser` (`third_party/libwebm`): the first two VP9/AV1 tracks, or one VP9 track and its BlockAdditional alpha. Takes profile, bit depth and colour from the bitstream headers, and seeks by the Cues (loaded when a seek needs them) or by scanning. Shared with the MediaCodec path through JNI, and compiled to WebAssembly for the web, where it reads through JavaScript and stops when bytes haven't arrived yet (`ByteSource::kPending`), carrying on once they have. `tests/core/demuxer_compare` checks it against FFmpeg's Matroska demuxer (`tests/core/ffmpeg/AlphaDemuxer`, which the web used before).
- **`VideoDecoder`** — libvpx (VP9, with row multithreading) and dav1d (AV1) behind one interface: send a packet, receive pictures, drain at the end, flush on a seek.
- **`ByteSource`** — Where the demuxer reads: a file, a range of a file descriptor (an APK asset or `content://` URI), a `RangeReader` (blocking until its bytes arrive, or until the player stops), or memory.
- **`WebMAlphaPlayer`** — The player on top of them:
  - `openStreams`, `initCodecs` (a decoder per track: libvpx for VP9, dav1d for AV1), `loadFromFile/loadFromMemory/loadFromFileRange/loadFromReader`
  - dav1d holds frames back with frame threading, so every decode loop drains the decoders (an empty packet) at the end of the file
  - `decodeAllFrames` (preload) and on-demand `decodeFrame`
  - YUVA frame extraction with alpha handling (embedded YUVA420P or separate alpha track)
  - `FrameCache` (LRU, mutex-protected) for decoded frames
  - Playback loop clocking frames at target FPS
  - `PlayerConfig`: cache size, `preloadAllFrames`, playback speed
- **`avsource::RangeReader`** (`third_party/alpha-video-source/src`) — Reads a file by offset from an `Upstream`, in 256 KB blocks kept in memory (16 MB, LRU) and in the store. One fetch thread keeps one read open and moves it only for a read far from it, reads 2 MB ahead of the last read, retries a failed read 3 times, and drops a stored copy whose size or validator changed. A file the store holds whole is read without opening the upstream.

## Threading Model

```
JS thread
  └─ Calls native props/methods

Android — Software Path
  ├─ UI thread: View creation, prop updates, attach/detach
  ├─ Probe executor: opens the source reader (and fetches it whole for preload) → post to UI
  ├─ Reader fetch thread (C++, attached to the JVM): reads the data source into blocks
  ├─ GL render thread: EGL context, texture upload, YUVA→RGBA shader, present
  ├─ Native load executor: nativeLoadFromFile, nativeStop, nativeUnload (async)
  └─ C++ threads: playback loop, decode (on cache miss)

Android — Hardware (Dual-Stream) Path
  ├─ UI thread: View creation, prop updates, attach/detach
  ├─ Probe executor + reader fetch thread, as on the software path
  ├─ GL render thread: EGL context, OES texture creation, compositing shader, present
  ├─ Demux thread: AlphaDemuxer reads packets, feeds to MediaCodec input buffers
  ├─ Codec callback threads (2): MediaCodec async output callbacks (one per decoder)
  └─ Playback thread: PTS-gated frame release, timing control, render signaling

iOS
  ├─ Main thread: View creation, prop updates
  ├─ Global queue: opens the source reader (and fetches it whole for preload); a per-view serial queue loads it
  ├─ Reader fetch thread (C++) and URLSession delegate queue: fetch into blocks
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
            a. nativeCreateDemuxer (JNI) → AlphaDemuxer opens WebM, finds the colour and alpha streams
            b. nativeGetVideoInfo → [width, height, frames, fps*1000, streamCount, codec]
            c. Map codec to MIME type (VP9=0 → "video/x-vnd.on2.vp9", AV1=1 → "video/av01")
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
       4. nativeSeekToStart(demuxer) → AlphaDemuxer back to the first packet
       5. Resume demux + playback from frame 0
```

The flush happens once the last frame has been drawn, and the new pass is held until the playback loop asks for frame 0, so the last frame stays up for its full duration while the decoders refill. The loop is seamless, as on the software path.

## Software Decode Path — Detailed Flow

```
Thread Lifecycle:

JS/Main thread                 Playback thread                Decode (inline)              Resources
--------------                 ----------------               ---------------              ---------
loadFromFile(path)         →   (not started yet)              demuxer open, init codecs    FrameCache created
  if preload:                                                 decodeAllFrames (inline)
play()                     →   spawn playbackThread           
                                  playing=true
                                  per frame slot:
                                    getFrame(idx)
                                      cache hit → emit        
                                      cache miss → decode     seek + libvpx/dav1d decode
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
4. Source resolved (a local file, or a source reader opened) → auto-detect → software or hardware path
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
   - HTTP or a data source: read by range through a `RangeReader`, which keeps what it has read in the store under `cacheKey` (else the URI). In `stream`/`streamAndPreload` mode the view loads as soon as the reader is open, and the demuxer waits on it for bytes not yet fetched; `preload` fetches the whole file first.
   - Local: `file://` or bundle asset resolved directly.
2. **Decode**
   - Software: C++ opens file, finds color + alpha streams, initializes codecs. Frames decoded to YUVA.
   - Hardware: AlphaDemuxer hands out raw packets, MediaCodec decodes to SurfaceTexture.
3. **Cache**
   - Software: `FrameCache` stores decoded frames keyed by frame index; LRU eviction.
   - Hardware: No frame cache; decoded frames go directly to OES textures.
4. **Render**
   - Software: YUVA textures uploaded per frame, fragment shader converts to RGBA.
   - Hardware: OES textures sampled directly, fragment shader composites color + alpha.

## Storage

- `storage` picks the store: `disk` (one folder per app, `alpha-video` in the platform cache folder), `memory` (one per process) or `none`; `auto` is `disk` for URLs and `none` for a data source. Every view shares the store, under the cap set last (`maxStorageBytes`, default 512 MB), and the least recently used clips go first.
- On disk each clip is a sparse data file and a small meta file (size, ETag or Last-Modified, which blocks are held), named by a hash of `cacheKey`, else the URI. A block that has gone missing is fetched again.

## Error and Event Flow

- Read errors → `onError` (JS) with message: opening the source (`Loading … failed: …`), or a read that failed after the retries (`Reading the video failed: …`), also when playback had already started. The demuxer then sees an error rather than the end of the file, and the C++ decode loop stops instead of looping.
- Decode errors → C++ delegate / Java callback → `onError`.
- Successful load → `onLoad` (totalFrames, width, height).
- Playback end → `onEnd`.

## Key Interactions

```
JS sets source + cacheKey + storage
   ↓
Platform ViewManager
   ↓ create view, set props
   ↓
if remote or a data source → open a RangeReader (probe executor / global queue)
   ↓ UI thread: load from the reader
   ↓
Auto-detect: nativeColorAlphaTracks?
   ├─ YES + profiles MediaCodec lists + HW decoder available (VP9: with 2 instances free):
   │    BufferedDualStreamPlayer
   │    ├─ demux thread: AlphaDemuxer → raw packets
   │    ├─ 2× MediaCodec → OES textures
   │    └─ GL thread: composite + present
   │
   └─ otherwise:
        WebMAlphaPlayer (C++)
        ├─ decode thread: libvpx/dav1d → YUVA frames → FrameCache
        └─ playback loop → texture upload → GPU shader → present
   ↓
Events: onLoad / onError / onEnd → JS
```
