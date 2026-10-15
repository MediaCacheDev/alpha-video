# alpha-video-web

Plays transparent WebM video in every browser that can show it: VP9 or AV1, with alpha in each block's BlockAdditional (what FFmpeg and libvpx write for `yuva420p`) or in a second track. Plain JS/TS, with an optional React component.

It's the web half of [react-native-alpha-video](https://github.com/MediaCacheDev/alpha-video), and it reuses that package's C++ core: the demuxer that splits a file into its colour and alpha bitstreams (`cpp/AlphaDemuxer`) and the YUV to RGB conversion of the iOS and Android players (`cpp/YuvToRgb`), compiled to WebAssembly.

```bash
npm install alpha-video-web
```

## How it picks an engine

`<video>` is the fastest way to play a WebM, but it doesn't always keep the alpha, and `canPlayType` can't tell: every browser answers `"probably"`, including Safari, which drops it. So the first time a page plays a file, a 599-byte clip is drawn into a canvas and two of its pixels are read (about 25 ms in Chromium, 5 ms in Firefox, once per page). Then, best first:

1. **`browser`**: a `<video>` element, when the probe shows it keeps WebM alpha (Chromium, Firefox) and the file is a single VP9 track with alpha. The first 64 KB of the file tell which kind it is, so nothing else is downloaded.
2. **`webcodecs`**: the C++ demuxer in WebAssembly feeds two WebCodecs `VideoDecoder`s, one for colour and one for alpha, and WebGL2 draws the pair with the native players' colour matrix. For dual-stream files, and for every alpha WebM in Safari.
3. **`software`**: the same demuxer feeding FFmpeg's VP9 decoder and dav1d (AV1) in WebAssembly, for streams WebCodecs can't decode: VP9 profile 1 alpha, the RGB-coded kind, in Safari (no decoder) and Firefox (converts it as if it were YUV). Colour and alpha each decode in their own Web Worker, in parallel and off the page's thread. On a [cross-origin isolated](#threads) page each decoder also runs threads of its own.

The WebAssembly modules are inlined in the package, in their own chunks, and loaded by dynamic `import()` only when an engine needs them: the demuxer (552 KB, 243 KB gzipped) for `webcodecs` and `software`, and for `software` the decode Worker (1.4 MB), or its threaded build (1.6 MB) on cross-origin isolated pages.

| | Chromium | Firefox | Safari |
|---|---|---|---|
| VP9, alpha in BlockAdditional (`yuva420p`) | `browser` | `browser` | `webcodecs` |
| VP9, two tracks | `webcodecs` | `webcodecs` | `webcodecs` |
| VP9 profile 1 (RGB) alpha track | `webcodecs` | `software` | `software` |
| AV1, two tracks (8- and 10-bit) | `webcodecs` | `webcodecs` | `webcodecs` |

## Plain JS/TS

```ts
import { createAlphaVideo } from 'alpha-video-web';

const player = createAlphaVideo(document.querySelector('#glow')!, {
  src: '/glow.webm',
  autoPlay: true,
  loop: true,
  onLoad: ({ totalFrames, width, height, decoder }) => console.log(decoder),
  onError: (error) => console.error(error.code, error.message),
  onEnd: () => {},
});

player.pause();
player.play();
player.seekToFrame(0);
player.seekToTime(1.5);
player.setPlaybackSpeed(2);
player.destroy();
```

The player adds a `<video>` or `<canvas>` to the container and sizes it to fill it (`objectFit`, default `'contain'`); give the container a size. `src` is a URL or the file itself (`Blob`, `ArrayBuffer`, `Uint8Array`). A seek shows the frame, clamped to the clip, and playback carries on from it or stays paused. `stop()` pauses on the first frame. Calls made while the file loads are applied once it has.

Options: `src`, `autoPlay`, `loop`, `playbackSpeed`, `decoder`, `objectFit`, `fetchInit` (passed to `fetch()`, e.g. `{ credentials: 'include' }`), `onLoad`, `onError`, `onEnd`. `player.loaded` is a promise of the `onLoad` event.

`decoder` is `'auto'` by default. Naming an engine (`'browser'`, `'webcodecs'`, `'webcodecs-hw'`, `'webcodecs-sw'`, `'software'`) forces it: if it can't play the file, `onError` says why and nothing else is tried. `'webcodecs-hw'` and `'webcodecs-sw'` ask the browser for a hardware or a software decoder. `onLoad`'s `decoder` names the engine used: `'browser'`, `'webcodecs (vp09.00.20.08)'` with the decoders' codec strings, or `'software (vp9)'`.

`videoElementKeepsWebMAlpha()` runs the probe on its own (a cached promise), and `sniffWebM(bytes)` reads the layout from the start of a file, for pages that want to decide for themselves.

## React

```tsx
import { AlphaVideo } from 'alpha-video-web/react';

<AlphaVideo src="/glow.webm" style={{ width: 300, height: 300 }} onError={(e) => console.error(e)} />
```

Props: the options above, plus `paused`, `className` and `style` for the container. A ref has `play()`, `pause()`, `stop()`, `seekToFrame(frame)` and `seekToTime(seconds)`. Changing `src`, `decoder` or `objectFit` starts a new player; the other props apply to the running one. React 16.8 or later is an optional peer dependency.

## Errors

`onError` gets an `AlphaVideoError` with a `code`:

- `'LOAD_FAILED'`: the file couldn't be fetched. The canvas engines read the file with `fetch()`, so a file on another origin needs CORS headers; under `auto`, one without them plays in `<video>`, as far as `<video>` can.
- `'UNSUPPORTED_FILE'`: not a WebM with a VP9 or AV1 track.
- `'NO_ALPHA_PATH'`: no engine in this browser can show the file with its alpha (no WebCodecs and no WebGL2, say).
- `'DECODER_UNSUPPORTED'`: a forced engine has no decoder for the file.
- `'DECODE_FAILED'`: a decoder failed while playing.
- `'WEBGL_UNSUPPORTED'`: no WebGL2 context for the canvas engines.

## Threads

WebAssembly threads need `SharedArrayBuffer`, which browsers give only to cross-origin isolated pages. Serve the page with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

(`credentialless` instead of `require-corp` works in Chromium and Firefox.) Then `crossOriginIsolated` is true, and `software` loads its threaded build: up to 4 threads for the colour decoder and 2 for the alpha. Threads are shared out per page from `navigator.hardwareConcurrency`: each video takes some while it's open, and once fewer than 4 cores are left, further videos decode single-threaded. `onLoad` reports the threads, e.g. `'software (vp9, 4+2 threads)'`. Each software-decoded video runs 2 Workers, plus one per decoder thread and a spare per decoder when threaded. Isolation has costs of its own: every cross-origin resource the page embeds must allow it (CORS or `Cross-Origin-Resource-Policy`), and popups lose `window.opener`. Without it, `software` still decodes colour and alpha in parallel, one Worker each. If the threaded build can't start, the single-threaded one is used.

## Requirements and limits

- WebCodecs needs a secure context: https, or http://localhost.
- `software` starts its Workers from `blob:` URLs, so a Content-Security-Policy must allow `worker-src blob:` (or `script-src blob:` without a `worker-src`). Without it, `software` reports `DECODER_UNSUPPORTED`.
- The canvas engines download the whole file before playing it. That suits UI animations and short clips; for long videos, prefer files `<video>` can play.
- `software` costs CPU. On an M4 Pro, in Chromium and Safari's engine, a 30 fps 1080p clip with alpha decodes at about 5× real time as VP9 and 3× as AV1 without threads, and 11× and 7× with them. Phones are several times slower; keep software-decoded clips small, or offer a file WebCodecs can decode.
- Frame rate: `browser` doesn't know it and assumes 30 fps for `totalFrames` and frame seeks, like the React Native component on the web; the canvas engines read it from the file.

## Development

```bash
npm install
npm run build            # dist/
npm test                 # the WebAssembly demuxer on the test cards in ../tests/fixtures, under node
npm run test:browsers    # Chromium, Firefox and WebKit through Playwright: every engine on every test card
npm run test:browsers -- --isolated   # the same, cross-origin isolated: software with threads
```

`npm run test:browsers` serves the page from http://localhost and checks, per browser, engine and card: the frame codes after seeks (colour and alpha separately), the alpha ramp and the colour swatches, against the tolerance of the C++ host tests (6 levels); then plays clips through a loop. More tools, each taking a directory of `.webm` files:

- `node test/browsers/speed.mjs <dir>`: how close to real time each engine plays them, and how many frames it shows;
- `node test/browsers/throughput.mjs <dir> [--isolated]`: how many colour and alpha pairs per second `software` decodes when nothing waits for the clock;
- `node test/browsers/formats.mjs`: which frame formats each browser's WebCodecs decoders output.

The WebAssembly modules in `src/wasm/` are generated by `../scripts/build_wasm.sh` (Emscripten, meson and ninja: `brew install emscripten meson ninja`) and committed, so building the package doesn't need Emscripten. The decode Workers are bundled into strings at build time (`scripts/inline-worker.mjs`), WebAssembly included, so there's no Worker file for apps to host.

Third-party code: see `THIRD_PARTY_NOTICES.md` (FFmpeg, LGPL-2.1-or-later; dav1d, BSD-2-Clause).
