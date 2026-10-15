# Third-party notices

The code of alpha-video-web is MIT-licensed (see `LICENSE`). Its WebAssembly modules, inlined in `dist/demuxer-*.{js,mjs}` and in the decode Workers `dist/decode-worker-*.{js,mjs}` (single-threaded) and `dist/decode-worker-mt-*.{js,mjs}` (threaded), also contain:

| Component | Version | Licence | In |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) | 6.1, unmodified | LGPL-2.1-or-later | the demuxer: libavformat, libavcodec, libavutil; the decoders: libavcodec, libavutil |
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.1, source unmodified | BSD-2-Clause | the decoders |

Licence texts: `licenses/LGPL-2.1.txt`, `licenses/FFmpeg-LICENSE.md`, `licenses/dav1d-COPYING.txt`.

## What the build contains

`scripts/build_wasm.sh` in the [repository](https://github.com/MediaCacheDev/alpha-video) builds the modules with Emscripten. FFmpeg is configured with only what the player uses, and with no GPL or LGPL-3.0 code (no `--enable-gpl`, no `--enable-version3`):

```
--target-os=none --arch=x86_32 --enable-cross-compile --disable-asm --disable-inline-asm --disable-x86asm
--disable-programs --disable-doc --disable-debug --disable-network --disable-gpl --disable-everything
--disable-autodetect --disable-iconv --disable-pthreads --disable-avdevice --disable-avfilter
--disable-swresample --disable-swscale --enable-demuxer=matroska,webm --enable-static --disable-shared
```

The decoder modules add `--disable-avformat --enable-decoder=vp9,libdav1d --enable-libdav1d`, and the threaded one `--enable-pthreads --extra-cflags=-pthread --extra-ldflags=-pthread`. dav1d is built for 8-bit and 10/12-bit video without assembly, tools or tests (`-Denable_asm=false -Dbitdepths=8,16 -Denable_tools=false -Denable_tests=false`), and with `-pthread` for the threaded module. The script edits one line of dav1d's `meson.build` so that Meson doesn't add `-pthread` to the single-threaded build.

## Source code

The corresponding source is the upstream release:

| File | SHA-256 |
|---|---|
| [`ffmpeg-6.1.tar.xz`](https://ffmpeg.org/releases/ffmpeg-6.1.tar.xz) | `488c76e57dd9b3bee901f71d5c95eaf1db4a5a31fe46a28654e837144207c270` |
| [`dav1d-1.5.1.tar.gz`](https://github.com/videolan/dav1d/archive/refs/tags/1.5.1.tar.gz) | `fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695` |

## Replacing FFmpeg

The modules are separate files in `dist/`, loaded at run time by dynamic `import()`. To use a modified FFmpeg, as LGPL-2.1 allows, rebuild them with `scripts/build_wasm.sh` against your FFmpeg, rebuild the package (`npm run build`, which bundles the decoder modules into the Worker files), and replace those files.
