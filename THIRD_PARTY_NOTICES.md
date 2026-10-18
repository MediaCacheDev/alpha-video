# Third-party notices

The code in this repository is MIT-licensed (see `LICENSE`). The native player (Android and iOS) also contains the following, which are not. Each artifact contains only its own row:

| Component | Version | Licence | In |
|---|---|---|---|
| [libwebm](https://chromium.googlesource.com/webm/libwebm)'s WebM parser | commit `6184f448`, unmodified | BSD-3-Clause, plus the WebM patent grant | the core (`dev.mediacache:alpha-video`; on iOS every product and subspec): source in `third_party/libwebm`, compiled with the C++ core |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) | 1.14.0, unmodified | BSD-3-Clause, plus the WebM patent grant | the vpx codec module (`alpha-video-vpx`, `AlphaVideoVPX`, `alpha-video/VPX`): `libvpx.a` in `prebuilt/` |
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.1, unmodified | BSD-2-Clause | the av1 codec module (`alpha-video-av1`, `AlphaVideoAV1`, `alpha-video/AV1`): `libdav1d.a` in `prebuilt/` |

The core also compiles in the C++ reader of [alpha-video-source](https://github.com/MediaCacheDev/alpha-video-source) (`third_party/alpha-video-source`), MIT-licensed by the same author, which reads files by range and keeps them between plays.

On Android the core and each codec module have their own native library; on iOS they're linked into the app. React Native builds in the codecs the app's `package.json` selects, both by default. Licence texts: `licenses/libwebm-LICENSE.txt`, `licenses/libwebm-PATENTS.txt`, `licenses/libvpx-LICENSE.txt`, `licenses/libvpx-PATENTS.txt`, `licenses/dav1d-COPYING.txt`. An app shipping the player must reproduce the notices of what it ships, for example on an open-source licences screen.

The web player, `alpha-video-web` (in `web/`, and a dependency of the React Native package for react-native-web), decodes VP9 with FFmpeg, which is LGPL-2.1-or-later. Its own [THIRD_PARTY_NOTICES.md](web/THIRD_PARTY_NOTICES.md) covers it.

## What the build contains

The libraries are built by `scripts/build_codecs_android.sh` (arm64-v8a, armeabi-v7a, x86_64), `scripts/build_codecs_ios_device.sh` (arm64) and `scripts/build_codecs_ios_simulator.sh` (arm64 + x86_64). libvpx is built as a VP9 decoder only (`--enable-vp9-decoder --disable-vp8-encoder --disable-vp9-encoder`); its full configure line is embedded in the library. dav1d, the AV1 decoder, is built for 8-bit and 10/12-bit video, without its tools and tests (`-Dbitdepths=8,16 -Denable_tools=false -Denable_tests=false`).

## Source code

None of the libraries is modified. The source is the upstream release:

| File | SHA-256 |
|---|---|
| [`libvpx-1.14.0.tar.gz`](https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.0.tar.gz) | `5f21d2db27071c8a46f1725928a10227ae45c5cd1cad3727e4aafbe476e321fa` |
| [`dav1d-1.5.1.tar.gz`](https://github.com/videolan/dav1d/archive/refs/tags/1.5.1.tar.gz) | `fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695` |

libwebm's parser is in `third_party/libwebm`; its README gives the upstream commit.
