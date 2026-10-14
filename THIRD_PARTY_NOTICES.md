# Third-party notices

The code in this repository is MIT-licensed (see `LICENSE`). The prebuilt libraries in `prebuilt/`, which are linked into the player's native library on Android and into the app on iOS, are not:

| Component | Version | Licence | Libraries |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) | 6.1, unmodified | LGPL-2.1-or-later | `libavformat.a`, `libavcodec.a`, `libavutil.a` |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) | 1.14.0, unmodified | BSD-3-Clause, plus the WebM patent grant | `libvpx.a` |
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.1, unmodified | BSD-2-Clause | `libdav1d.a` |

Licence texts: `licenses/LGPL-2.1.txt`, `licenses/FFmpeg-LICENSE.md`, `licenses/libvpx-LICENSE.txt`, `licenses/libvpx-PATENTS.txt`, `licenses/dav1d-COPYING.txt`.

## What the build contains

The libraries are built by `scripts/build_ffmpeg_android.sh` (arm64-v8a, armeabi-v7a, x86_64), `scripts/build_ffmpeg_ios_device.sh` (arm64) and `scripts/build_ffmpeg_ios_simulator.sh` (arm64 + x86_64). FFmpeg is configured with only what the player uses, and no GPL or LGPL-3.0 code (no `--enable-gpl`, no `--enable-version3`):

```
--disable-gpl --disable-everything --disable-autodetect --enable-zlib --enable-pthreads --disable-network --disable-programs
--disable-avdevice --disable-avfilter --disable-swresample --disable-swscale
--enable-demuxer=matroska,webm --enable-decoder=vp9,libvpx_vp9,libdav1d --enable-parser=vp9
--enable-protocol=file --enable-libvpx --enable-libdav1d --enable-static --disable-shared --enable-pic --disable-asm
```

The full configure line of each build is embedded in the libraries; `strings libavutil.a | grep -- --prefix` prints it. libvpx is built as a VP9 decoder only (`--enable-vp9-decoder --disable-vp8-encoder --disable-vp9-encoder`). dav1d, the AV1 decoder, is built for 8-bit and 10/12-bit video, without its tools and tests (`-Dbitdepths=8,16 -Denable_tools=false -Denable_tests=false`); its assembly is on, unlike FFmpeg's.

## Source code

None of the libraries is modified. The corresponding source is the upstream release:

| File | SHA-256 |
|---|---|
| [`ffmpeg-6.1.tar.xz`](https://ffmpeg.org/releases/ffmpeg-6.1.tar.xz) | `488c76e57dd9b3bee901f71d5c95eaf1db4a5a31fe46a28654e837144207c270` |
| [`libvpx-1.14.0.tar.gz`](https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.0.tar.gz) | `5f21d2db27071c8a46f1725928a10227ae45c5cd1cad3727e4aafbe476e321fa` |
| [`dav1d-1.5.1.tar.gz`](https://github.com/videolan/dav1d/archive/refs/tags/1.5.1.tar.gz) | `fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695` |

Copies of FFmpeg's and libvpx's are also attached to the GitHub Release of this repository that carries the prebuilt libraries.

## Replacing FFmpeg

The build scripts are in this repository, so you can rebuild the player against your own FFmpeg. On Android, FFmpeg is linked statically into `libalphavideo.so`; on iOS, it is linked statically into the app. If you ship a closed-source app, LGPL-2.1 asks you to let users relink it against a modified FFmpeg, for example by providing your app's object files or by building FFmpeg as a dynamic library.
