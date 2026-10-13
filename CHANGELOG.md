# Changelog

The packages built from this repository share one version: `react-native-alpha-video` on npm, `dev.mediacache:alpha-video` on Maven Central, and the `AlphaVideo` Swift package and `alpha-video` pod.

## 1.0.0 - Unreleased

First public release.

- Plays single-stream VP9 WebM with alpha (YUVA) on Android, iOS and the web, decoded by a shared C++ core (FFmpeg 6.1 and libvpx 1.14.0). iOS renders with Metal, Android with OpenGL ES on a `TextureView`.
- On Android, dual-stream WebM (colour and alpha in separate VP9 or AV1 tracks) is decoded by MediaCodec in hardware. The view picks the path from the file. Elsewhere the C++ core decodes dual-stream VP9 in every playback mode, and expands a limited-range alpha track to the full 0-255.
- Colour is converted with the clip's range and matrix (limited or full; BT.601, BT.709 or BT.2020), matching FFmpeg's own conversion. An untagged clip is taken as limited range, BT.601 up to 720x576 and BT.709 above. Odd frame sizes keep their last chroma column and row.
- Playback modes for the software path: `preload` (decode every frame once), `stream` (decode ahead, bounded cache) and `streamAndPreload`. A global decode queue (`useDecodeQueue`) keeps many players from saturating the CPU.
- Sources: `https://`, `file://`, absolute paths, `asset://` (Android assets, the iOS bundle, the web root), `res/raw` names and `content://` on Android. http(s) files are cached under `cacheKey`, and the stream modes play them while they download.
- Playback control: `play`, `pause`, `stop`, `seekToFrame` and `seekToTime`, plus `paused`, `autoPlay`, `loop` and `playbackSpeed`. Events: `onLoad`, `onError` and `onEnd`.
- Android decoder status: `getDecoderStatus`, `getDecoderInfo`, `getMaxDecoderInstances` and `getActiveDecoderCount`.
- React Native: `AlphaVideoView` on both the new architecture (a Fabric view, and a TurboModule for the decoder status) and the old one. On react-native-web it renders a `<video>`.
- Standalone Android and iOS libraries with no React Native dependency.
- MIT, with the bundled FFmpeg under LGPL-2.1-or-later and libvpx under BSD-3-Clause. Their notices are in `THIRD_PARTY_NOTICES.md`, and their source is attached to the GitHub release.
- Android libraries are linked for 16 KB pages.
