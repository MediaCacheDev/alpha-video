# alpha-video-web

Plays transparent WebM video in every modern browser, including those that can't show transparent video themselves: Safari drops WebM's alpha channel, and no browser shows the alpha track of a dual-stream file. It plays VP8, VP9 or AV1, with alpha in each block's BlockAdditional (what FFmpeg and libvpx write for `yuva420p`) or in a second track. Plain JS/TS, with an optional React component.

It's the web half of [react-native-alpha-video](https://github.com/MediaCacheDev/alpha-video), whose `AlphaVideoView` plays through it on react-native-web, and it reuses that package's C++ core: the demuxer that splits a file into its colour and alpha bitstreams (`cpp/AlphaDemuxer`, on libwebm's parser, the same one Android and iOS run) and the YUV to RGB conversion of the iOS and Android players (`cpp/YuvToRgb`), compiled to WebAssembly. Files stream in by HTTP range requests as they play, and can be kept in the browser's storage between plays.

```bash
npm install alpha-video-web
```

## How it picks an engine

`<video>` is the fastest way to play a WebM, but it doesn't always keep the alpha, and `canPlayType` can't tell: every browser answers `"probably"`, including Safari, which drops it. So the first time a page plays a file, a 599-byte clip is drawn into a canvas and two of its pixels are read (about 25 ms in Chromium, 5 ms in Firefox, once per page). Then, best first:

1. **`browser`**: a `<video>` element, when the probe shows it keeps WebM alpha (Chromium, Firefox) and the file is a single VP8 or VP9 track with alpha. The first 64 KB of the file tell which kind it is, so nothing else is downloaded.
2. **`webcodecs`**: the C++ demuxer in WebAssembly reads the file by range and feeds two WebCodecs `VideoDecoder`s, one for colour and one for alpha, and WebGL2 draws the pair with the native players' colour matrix. For dual-stream files, and for every alpha WebM in Safari.
3. **`software`**: the same demuxer feeding FFmpeg's VP8 and VP9 decoders and dav1d (AV1) in WebAssembly, for streams WebCodecs can't decode: VP9 profile 1 alpha, the RGB-coded kind, in Safari (no decoder) and Firefox (converts it as if it were YUV). Colour and alpha each decode in their own Web Worker, in parallel and off the page's thread. On a [cross-origin isolated](#threads) page each decoder also runs threads of its own.

The WebAssembly modules are inlined in the package, in their own chunks, and loaded by dynamic `import()` only when an engine needs them: the demuxer (183 KB, 63 KB gzipped) for `webcodecs` and `software`, and for `software` one decode Worker per codec module the file uses, vpx for VP8 and VP9 (1.2 MB, 403 KB gzipped) or AV1 (480 KB, 211 KB gzipped). Cross-origin isolated pages load their threaded builds (1.3 MB and 518 KB).

| | Chromium | Firefox | Safari |
|---|---|---|---|
| VP8 or VP9, alpha in BlockAdditional (`yuva420p`) | `browser` | `browser` | `webcodecs` |
| VP8 or VP9, two tracks | `webcodecs` | `webcodecs` | `webcodecs` |
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

The player adds a `<video>` or `<canvas>` to the container and sizes it to fill it (`objectFit`, default `'contain'`); give the container a size. `src` is a URL, the file itself, or a byte source (see [Sources and storage](#sources-and-storage)). A seek shows the frame, clamped to the clip, and playback carries on from it or stays paused. `stop()` pauses on the first frame. Calls made while the file loads are applied once it has.

Options: `src`, `autoPlay`, `loop`, `playbackSpeed`, `decoder`, `objectFit`, `headers`, `cacheKey`, `storage`, `maxStorageBytes`, `fetchInit` (other `fetch()` options, e.g. `{ credentials: 'include' }`), `onLoad`, `onError`, `onEnd`. `player.loaded` is a promise of the `onLoad` event.

`decoder` is `'auto'` by default. Naming an engine (`'browser'`, `'webcodecs'`, `'webcodecs-hw'`, `'webcodecs-sw'`, `'software'`) forces it: if it can't play the file, `onError` says why and nothing else is tried. `'webcodecs-hw'` and `'webcodecs-sw'` ask the browser for a hardware or a software decoder. `onLoad`'s `decoder` names the engine used: `'browser'`, `'webcodecs (vp09.00.20.08)'` with the decoders' codec strings (`vp8` for VP8), or `'software (vp9)'` (`'software (vp8)'`).

`videoElementKeepsWebMAlpha()` runs the probe on its own (a cached promise), and `sniffWebM(bytes)` reads the layout from the start of a file, for pages that want to decide for themselves.

## Sources and storage

`src` takes:

- **A URL.** `<video>` loads it itself, with the browser's HTTP cache. The canvas engines read it with HTTP range requests: playback starts once the headers and the first frames are in, the rest follows as it plays (a megabyte ahead), and a seek fetches the Cues and the cluster it lands in, not the bytes before. `headers` (for example `Authorization`) go with every request; `<video>` can't send headers, so with them a file `<video>` plays is fetched whole first and played from memory. A server without range support works too, read from the start.
- **A `Blob` or `File`,** read with `slice`.
- **An `ArrayBuffer` or `Uint8Array`.**
- **A byte source** of your own: `{ size, read(offset, length, signal) }`, returning a `Promise<Uint8Array>`, for apps that fetch, decrypt or cache in their own code. See [alpha-video-source](https://github.com/MediaCacheDev/alpha-video-source/tree/main/web#sources) for the full interface; `httpSource`, `blobSource` and `memorySource` are re-exported here. The canvas engines read it by range; for `<video>` it's read whole into a `Blob`.

What the canvas engines keep a URL's bytes in between plays is the `storage` option:

- `'auto'` (the default for URLs) is OPFS, falling back to IndexedDB where OPFS isn't available.
- `'opfs'`, `'indexeddb'`, `'cache'` (the Cache API) or `'memory'` (this page only) pick one. Where it isn't available, the player falls back to IndexedDB, then memory.
- `'none'` stores nothing. Clips up to 16 MB still stay in memory, so loops don't fetch again.
- A `ByteStore` of your own (from `createStore`, or written against alpha-video-source's interface).

Storage is partial: it keeps the ranges played so far, and a clip held whole plays again without the network, offline too. `maxStorageBytes` (default 512 MB) caps what this player keeps across every clip, evicting the least recently played; the store is this player's own (namespace `'alpha-video'`), apart from alpha-video-gpu-web's. Stored clips are named by `cacheKey`, else the URL, so a presigned URL that changes on each visit can keep the same copy. A copy is dropped when the server's `ETag`, `Last-Modified` or length no longer matches it. A clip held whole isn't checked again; give a changed file a new `cacheKey` or URL.

Blobs and byte sources default to `'none'`, as the data is local already, or the source does its own caching. With `storage` and a `cacheKey` they are stored too.

A Service Worker controlling the page sees the player's range requests as plain `fetch`es, so it can answer them from its own cache.

Cross-origin URLs need CORS for the canvas engines (under `auto`, one without it plays in `<video>`, as far as `<video>` can). To check versions, the server must expose `ETag` (or `Last-Modified`) and `Content-Range` with `Access-Control-Expose-Headers`; without them, copies aren't checked.

## React

```tsx
import { AlphaVideo } from 'alpha-video-web/react';

<AlphaVideo src="/glow.webm" style={{ width: 300, height: 300 }} onError={(e) => console.error(e)} />
```

Props: the options above, plus `paused`, `className` and `style` for the container. A ref has `play()`, `pause()`, `stop()`, `seekToFrame(frame)` and `seekToTime(seconds)`. Changing `src`, `decoder`, `objectFit`, `headers` (by value), `cacheKey`, `storage` or `maxStorageBytes` starts a new player; the other props apply to the running one. React 16.8 or later is an optional peer dependency.

## Errors

`onError` gets an `AlphaVideoError` with a `code`:

- `'LOAD_FAILED'`: the file couldn't be fetched, or reading it failed while playing (the connection dropped, the file changed on the server). The canvas engines read the file with `fetch()`, so a file on another origin needs CORS headers; under `auto`, one without them plays in `<video>`, as far as `<video>` can.
- `'UNSUPPORTED_FILE'`: not a WebM with a VP8, VP9 or AV1 track.
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
- The canvas engines seek by the file's Cues. A file without them (some live recordings) is scanned from the start for the keyframe, which on a long file means fetching what lies before it.
- `software` costs CPU. On an M4 Pro, in Chromium and Safari's engine, a 30 fps 1080p clip with alpha decodes at about 5× real time as VP9 and 3× as AV1 without threads, and 11× and 7× with them. Phones are several times slower; keep software-decoded clips small, or offer a file WebCodecs can decode.
- WebAssembly memory is set when the modules are built (`../scripts/build_wasm.sh`), not at run time, and grows as it's needed rather than being allocated up front. The demuxer, one per page shared by every player, starts at 16 MB. Each `software` decode Worker has its own decoder module, starting at 32 MB, so a software-decoded video adds two (colour and alpha); WebCodecs videos add none. The single-threaded decoders can grow to 2 GB. The threaded ones are capped at 1 GB: their memory is shared between threads and can't move, so the browser reserves address space for the whole cap when the module starts, though it uses RAM only for what's touched. At the default 2 GB, WebKit ran out of address space after a few of them.
- Frame rate: `browser` doesn't know it and assumes 30 fps for `totalFrames` and frame seeks, like the React Native component on the web; the canvas engines read it from the file.
- Players per page: browsers keep about 16 WebGL contexts on a page and lose the oldest past that, so the canvas engines share one. Each player draws into it in turn and copies the picture into its own 2D canvas, which costs next to nothing where the page is GPU-accelerated (0.03 ms per 1080p frame in Chromium and Firefox on an M4 Pro). Headless browsers without GPU compositing read each copy back, several milliseconds a frame.

## Potential future work

- **The native players' buffering options.** On Android and iOS, `AlphaVideoView` takes `mode` (`stream`, `preload` or `streamAndPreload`), `streamCacheSize`, `streamPrefetchAhead` and `useDecodeQueue`; on the web they're ignored. The canvas engines only stream: decoding stays a fixed number of pairs ahead of the playhead (4 for `software`, 6 for `webcodecs`), and a frame's buffer goes back to its decoder once drawn, so each loop decodes the clip again. Carrying the options over would mean:
  - a decode-ahead option in place of those constants. WebCodecs hardware decoders have few output frames, so holding many undrawn ones would need a cap or a copy out of the decoder;
  - a cache of decoded pairs, which `preload` and `streamAndPreload` fill with the whole clip, so loops play from memory and decoding stops. That suits short loops in `software`, which otherwise keeps its two Workers busy for as long as the clip plays. A 400×400 frame with alpha takes about 400 KB, so a 6-second glow loop about 72 MB, and 1080p about 5 MB a frame. The pairs could be kept as textures in the shared WebGL2 context, which also saves uploading them again on each loop;
  - with preloading, a page-wide queue so that many players starting at once don't all decode at the same time.

## Development

```bash
npm install
npm run build            # dist/
npm test                 # the WebAssembly demuxer on the test cards in ../tests/fixtures, under node
npm run test:browsers    # Chromium, Firefox and WebKit through Playwright: every engine on every test card
npm run test:browsers -- --isolated   # the same, cross-origin isolated: software with threads
npm run test:streaming   # range requests, a server without them, and replay from storage
```

`npm test` also reads every card through a reader that hands over one block per fetch, so the demuxer stops for bytes mid-header and mid-frame, and checks it gives the same packets and seeks, gives up a wait when interrupted, and reports a failing source. `npm run test:browsers` serves the page from http://localhost and checks, per browser, engine and card: the frame codes after seeks (colour and alpha separately), the alpha ramp and the colour swatches, against the tolerance of the C++ host tests (6 levels); then plays clips through a loop. `npm run test:streaming` makes a 10 s, 4 MB dual-stream clip with ffmpeg on first run, and checks that a seek near its end fetches from near the end, that a server without range support plays it, and that once played it plays again with no requests. More tools, each taking a directory of `.webm` files:

- `node test/browsers/speed.mjs <dir>`: how close to real time each engine plays them, and how many frames it shows;
- `node test/browsers/throughput.mjs <dir> [--isolated]`: how many colour and alpha pairs per second `software` decodes when nothing waits for the clock;
- `node test/browsers/formats.mjs`: which frame formats each browser's WebCodecs decoders output.

The WebAssembly modules in `src/wasm/` are generated by `../scripts/build_wasm.sh` (Emscripten, meson and ninja: `brew install emscripten meson ninja`) and committed, so building the package doesn't need Emscripten. The decode Workers are bundled into strings at build time (`scripts/inline-worker.mjs`), WebAssembly included, so there's no Worker file for apps to host.

Third-party code: see `THIRD_PARTY_NOTICES.md` (libwebm's parser, BSD-3-Clause; FFmpeg, LGPL-2.1-or-later; dav1d, BSD-2-Clause).
