import type { ByteSource, ByteStore, StorageKind } from 'alpha-video-source';
import type { AlphaVideoError } from './errors';

/**
 * Which engine plays the file. `'auto'` (default) picks the best one this browser has for it:
 * - `'browser'`: a `<video>` element, when the probe shows it keeps WebM alpha (Chromium,
 *   Firefox) and the file is a single VP8 or VP9 track with alpha, or has no alpha at all.
 * - `'webcodecs'`: the C++ demuxer in WebAssembly and two WebCodecs decoders, drawn with WebGL2.
 *   For dual-stream VP8, VP9 and AV1, and for any alpha WebM where `<video>` drops alpha (Safari).
 *   `'webcodecs-hw'` and `'webcodecs-sw'` ask the browser for a hardware or software decoder.
 * - `'software'`: the same demuxer with FFmpeg's VP8 and VP9 decoders and dav1d (AV1) in WebAssembly, on
 *   the page's thread, for streams the browser's WebCodecs can't decode (VP9 profile 1 alpha in
 *   Safari and Firefox). Slower than the others, and a 1.2 MB module, loaded only when used.
 *
 * Naming one forces it: if it can't play the file, `onError` reports why and nothing else is tried.
 */
export type Decoder = 'auto' | 'browser' | 'webcodecs' | 'webcodecs-hw' | 'webcodecs-sw' | 'software';

/**
 * A URL (fetched with CORS when it isn't same-origin, unless `<video>` plays it), the file itself
 * (a Blob or File, or its bytes), or a ByteSource that reads it by offset (an app's own fetcher
 * or cache). The canvas engines read it by range as they play, so a long clip starts at once and
 * a seek fetches only what it needs; `<video>` loads a URL itself.
 */
export type AlphaVideoSource = string | Blob | ArrayBuffer | Uint8Array | ByteSource;

/**
 * Where the canvas engines keep a URL's bytes between plays: 'auto' (OPFS, else IndexedDB),
 * 'opfs', 'indexeddb', 'cache' (the Cache API), 'memory' (this page only), 'none', or your own
 * store.
 */
export type AlphaVideoStorage = StorageKind | ByteStore;

export type ObjectFit = 'contain' | 'cover' | 'fill';

export interface AlphaVideoLoadEvent {
  totalFrames: number;
  width: number;
  height: number;
  /**
   * `'browser'`, `'webcodecs (vp09.00.10.08)'` with the decoders' codec strings, or
   * `'software (vp9)'` / `'software (dav1d)'`
   */
  decoder: string;
}

export interface AlphaVideoOptions {
  src: AlphaVideoSource;
  /** Start playing once loaded (default true) */
  autoPlay?: boolean;
  /** Loop (default true) */
  loop?: boolean;
  /** Playback speed multiplier (default 1) */
  playbackSpeed?: number;
  /** default `'auto'` */
  decoder?: Decoder;
  /** How the video fills the container (default `'contain'`) */
  objectFit?: ObjectFit;
  /** Passed to fetch() when the engine needs the file's bytes, e.g. `{ credentials: 'include' }` */
  fetchInit?: RequestInit;
  /**
   * Sent with every request for a URL, e.g. Authorization (merged over `fetchInit.headers`).
   * `<video>` can't send headers, so with them it plays the file from memory once fetched.
   */
  headers?: Record<string, string>;
  /**
   * Names the stored copy, for URLs that change while the file doesn't (presigned URLs).
   * Defaults to the URL. A Blob or ByteSource is stored only with storage set and a cacheKey.
   */
  cacheKey?: string;
  /**
   * Where the canvas engines keep a URL's bytes between plays, read back by range (`<video>`
   * uses the browser's HTTP cache). Default: 'auto' for URLs, 'none' for the rest. With
   * 'none', a clip under 16 MB still stays in memory for loops.
   */
  storage?: AlphaVideoStorage;
  /** Cap on the bytes stored across every clip, evicting the least recently played. Default: 512 MB */
  maxStorageBytes?: number;
  onLoad?: (event: AlphaVideoLoadEvent) => void;
  onError?: (error: AlphaVideoError) => void;
  /** The end of a clip that doesn't loop */
  onEnd?: () => void;
}

export interface AlphaVideoPlayer {
  /** The `<video>` or `<canvas>` in the container, once loading has picked an engine */
  readonly element: HTMLVideoElement | HTMLCanvasElement | null;
  /** Resolves with the onLoad event; rejects with the onError error */
  readonly loaded: Promise<AlphaVideoLoadEvent>;
  play(): void;
  pause(): void;
  /** Pauses on the first frame */
  stop(): void;
  /** Shows that frame (clamped to the clip) and keeps playing or stays paused */
  seekToFrame(frame: number): void;
  /** Seeks to the frame showing at that time, in seconds */
  seekToTime(seconds: number): void;
  setPlaybackSpeed(speed: number): void;
  setLoop(loop: boolean): void;
  /** Stops decoding, frees the decoders and removes the element */
  destroy(): void;
}
