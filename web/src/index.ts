/**
 * alpha-video-web: transparent WebM (VP9 or AV1, alpha in BlockAdditional or a second track) in
 * every browser that can show it, by the best path it has:
 *
 * 1. a `<video>`, where a pixel probe shows the browser keeps WebM alpha (Chromium, Firefox);
 * 2. WebCodecs: the C++ demuxer of react-native-alpha-video in WebAssembly feeding two
 *    VideoDecoders, drawn with WebGL2 and the native players' colour conversion (Safari, and
 *    dual-stream files everywhere).
 *
 *   import { createAlphaVideo } from 'alpha-video-web';
 *
 *   const player = createAlphaVideo(container, { src: '/glow.webm', onLoad: (e) => console.log(e.decoder) });
 *
 * The WebAssembly demuxer is inlined in its own chunk, loaded only when `<video>` can't play the
 * file. React: `alpha-video-web/react`.
 */

export { createAlphaVideo } from './player';
export { videoElementKeepsWebMAlpha } from './probe';
export { sniffWebM, type WebMLayout, type WebMSniff } from './sniff';
export { AlphaVideoError, type AlphaVideoErrorCode } from './errors';
export type {
  AlphaVideoLoadEvent,
  AlphaVideoOptions,
  AlphaVideoPlayer,
  AlphaVideoSource,
  AlphaVideoStorage,
  Decoder,
  ObjectFit,
} from './types';
export { STORAGE_NAMESPACE } from './input';
export {
  blobSource,
  createStore,
  httpSource,
  memorySource,
  HttpSourceError,
  SourceChangedError,
} from 'alpha-video-source';
export type { ByteSource, ByteStore, SourceInfo, StorageKind, HttpSourceOptions } from 'alpha-video-source';
