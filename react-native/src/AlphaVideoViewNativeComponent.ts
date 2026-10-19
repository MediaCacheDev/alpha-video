import type * as React from 'react';
import type { HostComponent, ViewProps } from 'react-native';
import type {
  DirectEventHandler,
  Double,
  Int32,
  WithDefault,
} from 'react-native/Libraries/Types/CodegenTypes';
import codegenNativeCommands from 'react-native/Libraries/Utilities/codegenNativeCommands';
import codegenNativeComponent from 'react-native/Libraries/Utilities/codegenNativeComponent';

/**
 * Playback mode for the WebM player.
 * 
 * - `'stream'` (default): Decode frames on-demand with a sliding window cache.
 *   Best for long videos or memory-constrained devices. Frames are evicted
 *   from cache, so looping requires re-decoding.
 * 
 * - `'preload'`: Decode all frames upfront before playback starts.
 *   Best for short loops. Zero CPU during playback, instant looping.
 * 
 * - `'streamAndPreload'`: Start playing immediately while decoding in background.
 *   After first pass completes, all frames are cached for instant looping.
 *   Best of both worlds: fast start + smooth loops.
 */
export type PlaybackMode = 'preload' | 'stream' | 'streamAndPreload';

/**
 * Decoder for files with colour and alpha as separate VP8/VP9/AV1 bitstreams (two tracks, or a
 * standard alpha WebM's BlockAdditional). Android only; iOS always uses the C++ core and web the browser.
 *
 * - `'auto'` (default): MediaCodec hardware decoders that list every track's profile, else
 *   MediaCodec's software decoders, else the C++ core.
 * - `'mediacodec'`: per track, a hardware decoder if one lists the profile and has an instance
 *   free, else a software one.
 * - `'mediacodec-hw'` / `'mediacodec-sw'`: hardware / software decoders only, for both tracks.
 * - `'software'`: the C++ core, with the codecs built in (libvpx for VP8 and 8-bit VP9, dav1d for 8- to
 *   12-bit AV1).
 *
 * A forced engine never falls back: a file it can't play calls `onError`. One that needs a codec
 * left out of the build reports the code `'NO_DECODER'`.
 */
export type Decoder = 'auto' | 'mediacodec' | 'mediacodec-hw' | 'mediacodec-sw' | 'software';

type LoadEvent = Readonly<{
  totalFrames: Int32;
  width: Int32;
  height: Int32;
  /** The engine and decoders playing the file, e.g. `'mediacodec-hw (c2.qti.vp9.decoder)'`. */
  decoder: string;
}>;

type ErrorEvent = Readonly<{
  error: string;
  /** `'NO_DECODER'` when the file needs a software decoder that isn't built in */
  code?: string;
}>;

/** Android only: every frame is decoded (preload and streamAndPreload modes). */
type PreloadCompleteEvent = Readonly<{
  totalFrames: Int32;
  timeMs: Double;
}>;

export interface NativeProps extends ViewProps {
  source?: Readonly<{
    uri?: string;
    /** Request headers as name, value, name, value... (codegen has no string maps) */
    headers?: ReadonlyArray<string>;
    /** A data source registered in native code under this name */
    dataSource?: string;
  }>;
  autoPlay?: WithDefault<boolean, true>;
  loop?: WithDefault<boolean, true>;
  paused?: WithDefault<boolean, false>;
  playbackSpeed?: WithDefault<Double, 1.0>;
  /** What the store keeps the source under, instead of its URL */
  cacheKey?: string;
  /** Where a URL's or data source's bytes are kept between plays */
  storage?: WithDefault<'auto' | 'disk' | 'memory' | 'none', 'auto'>;
  /** The store's cap in bytes; 0 for the default (512 MB) */
  maxStorageBytes?: WithDefault<Double, 0>;

  /** See {@link PlaybackMode}. */
  mode?: WithDefault<'preload' | 'stream' | 'streamAndPreload', 'stream'>;

  /** See {@link Decoder}. */
  decoder?: WithDefault<'auto' | 'mediacodec' | 'mediacodec-hw' | 'mediacodec-sw' | 'software', 'auto'>;

  /**
   * Number of frames to keep in cache when streaming.
   * Default: 30 frames (~1 second at 30fps)
   * Only used when mode='stream'
   */
  streamCacheSize?: WithDefault<Int32, 30>;

  /**
   * Number of frames to prefetch ahead of playback position when streaming.
   * Default: 15 frames (~0.5 seconds at 30fps)
   * Only used when mode='stream' or mode='streamAndPreload'
   */
  streamPrefetchAhead?: WithDefault<Int32, 15>;

  /**
   * Enable global decode queue for non-stream mode.
   * When true (default): decoding is serialized across all players, preventing CPU saturation
   * When false: each player decodes independently (can freeze with many)
   * Has no effect when mode='stream'.
   */
  useDecodeQueue?: WithDefault<boolean, true>;

  /**
   * Enable verbose per-frame logging for debugging.
   * When true: logs every frame queue, release, and render event.
   * When false (default): only logs lifecycle events and periodic performance stats.
   */
  verbose?: WithDefault<boolean, false>;

  onLoad?: DirectEventHandler<LoadEvent>;
  onError?: DirectEventHandler<ErrorEvent>;
  onEnd?: DirectEventHandler<Readonly<{}>>;
  onPreloadComplete?: DirectEventHandler<PreloadCompleteEvent>;
}

type NativeComponentType = HostComponent<NativeProps>;

interface NativeCommands {
  play: (viewRef: React.ElementRef<NativeComponentType>) => void;
  pause: (viewRef: React.ElementRef<NativeComponentType>) => void;
  stop: (viewRef: React.ElementRef<NativeComponentType>) => void;
  seekToFrame: (viewRef: React.ElementRef<NativeComponentType>, frame: Int32) => void;
  seekToTime: (viewRef: React.ElementRef<NativeComponentType>, seconds: Double) => void;
}

export const Commands: NativeCommands = codegenNativeCommands<NativeCommands>({
  supportedCommands: ['play', 'pause', 'stop', 'seekToFrame', 'seekToTime'],
});

export default codegenNativeComponent<NativeProps>('RNAlphaVideoView') as NativeComponentType;
