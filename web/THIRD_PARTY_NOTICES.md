# Third-party notices

The code of alpha-video-web is MIT-licensed (see `LICENSE`). Its WebAssembly modules are inlined in `dist/demuxer-*.{js,mjs}` and in the decode Workers, one per codec module: `dist/decode-worker-vpx-*.{js,mjs}` (VP8 and VP9) and `dist/decode-worker-av1-*.{js,mjs}` (single-threaded), and `dist/decode-worker-vpx-mt-*` and `dist/decode-worker-av1-mt-*` (threaded). A page loads a module's Worker only to software-decode a stream in its codecs. Besides the repository's C++ core (MIT), they contain:

| Component | Version | Licence | In |
|---|---|---|---|
| [libwebm](https://chromium.googlesource.com/webm/libwebm)'s WebM parser | commit `6184f448`, unmodified | BSD-3-Clause, plus the WebM patent grant | the demuxer, with `cpp/AlphaDemuxer`, `cpp/Av1SequenceHeader` and `cpp/YuvToRgb` |
| [FFmpeg](https://ffmpeg.org) | 6.1, unmodified | LGPL-2.1-or-later | the vpx Workers only: libavcodec, libavutil |
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.1, source unmodified | BSD-2-Clause | the AV1 Workers only, with the av1 codec module's adapter (`codecs/av1`) |

Licence texts: `licenses/libwebm-LICENSE.txt`, `licenses/libwebm-PATENTS.txt`, `licenses/LGPL-2.1.txt`, `licenses/FFmpeg-LICENSE.md`, `licenses/dav1d-COPYING.txt`.

The JavaScript also bundles [alpha-video-source](https://github.com/MediaCacheDev/alpha-video-source) (MIT, by the same author), which reads files by range and keeps them in the browser's storage.

## What the build contains

`scripts/build_wasm.sh` in the [repository](https://github.com/MediaCacheDev/alpha-video) builds the modules with Emscripten. The demuxer is the C++ core and libwebm's parser, from the repository's `cpp/` and `third_party/libwebm`. FFmpeg, in the vpx modules only, is configured with only what the player uses, and with no GPL or LGPL-3.0 code (no `--enable-gpl`, no `--enable-version3`):

```
--target-os=none --arch=x86_32 --enable-cross-compile --disable-asm --disable-inline-asm --disable-x86asm
--disable-programs --disable-doc --disable-debug --disable-network --disable-gpl --disable-everything
--disable-autodetect --disable-iconv --disable-pthreads --disable-avdevice --disable-avfilter
--disable-swresample --disable-swscale --enable-demuxer=matroska,webm --enable-static --disable-shared
```

plus `--disable-avformat --enable-decoder=vp8,vp9`, and for the threaded module `--enable-pthreads --extra-cflags=-pthread --extra-ldflags=-pthread`. dav1d, in the AV1 modules only, is built for 8-bit and 10/12-bit video without assembly, tools or tests (`-Denable_asm=false -Dbitdepths=8,16 -Denable_tools=false -Denable_tests=false`), and with `-pthread` for the threaded module. The script edits one line of dav1d's `meson.build` so that Meson doesn't add `-pthread` to the single-threaded build.

## Source code

libwebm's parser is in the repository's `third_party/libwebm`, whose README gives the upstream commit. The others are the upstream releases:

| File | SHA-256 |
|---|---|
| [`ffmpeg-6.1.tar.xz`](https://ffmpeg.org/releases/ffmpeg-6.1.tar.xz) | `488c76e57dd9b3bee901f71d5c95eaf1db4a5a31fe46a28654e837144207c270` |
| [`dav1d-1.5.1.tar.gz`](https://github.com/videolan/dav1d/archive/refs/tags/1.5.1.tar.gz) | `fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695` |

## Replacing FFmpeg

The vpx decoder modules are separate files in `dist/`, loaded at run time by dynamic `import()`. To use a modified FFmpeg, as LGPL-2.1 allows, rebuild them with `scripts/build_wasm.sh` against your FFmpeg, rebuild the package (`npm run build`, which bundles the decoder modules into the Worker files), and replace the `decode-worker-vpx` files.
