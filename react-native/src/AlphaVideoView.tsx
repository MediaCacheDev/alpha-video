/**
 * The player view, split out from the package barrel.
 *
 * Separate from `index.tsx` so that anything built ON the player can import it without the barrel
 * importing that code back. A cycle through the entry point happens to work while every reference
 * is inside a render, which makes it a trap rather than a design.
 */

import React, { useRef, forwardRef, useImperativeHandle, useMemo } from 'react';
import {
  Platform,
  ViewProps,
  StyleProp,
  ViewStyle,
} from 'react-native';
import NativeAlphaVideoView, { Commands, type Decoder } from './AlphaVideoViewNativeComponent';
import { WebPlayer } from './WebPlayer';

export type { Decoder };

/**
 * The web engine (default `'auto'`): `'browser'` (`<video>`), `'webcodecs'`, `'webcodecs-hw'`,
 * `'webcodecs-sw'` or `'software'` (WebAssembly). `'auto'` tries them in that order and uses the
 * first that shows the alpha; a forced engine never falls back.
 */
export type WebDecoder = 'auto' | 'browser' | 'webcodecs' | 'webcodecs-hw' | 'webcodecs-sw' | 'software';

/**
 * Source types for WebM video
 */
export interface AlphaVideoSource {
  /**
   * https:// (read by range as it plays), file://, an absolute path, asset://relative/path
   * (app bundle / APK assets; the site root on web), content:// (Android), or
   * Image.resolveAssetSource(require(...)).uri. With `dataSource`, any URI that source reads.
   */
  uri: string;
  /** Names the stored copy, for URLs that change while the file doesn't (presigned URLs); same as the `cacheKey` prop */
  cacheKey?: string;
  /** Sent with every request for the URL, e.g. Authorization; given to a `dataSource` too */
  headers?: Record<string, string>;
  /**
   * iOS and Android: read the file through the data source the app registered under this name
   * (`AlphaVideoDataSources.register` on Android, `AlphaVideoDataSources.register(_:name:)`
   * on iOS) instead of the built-in fetcher.
   */
  dataSource?: string;
}

/**
 * Where a URL's bytes are kept between plays, read back by range: `'disk'` (the cache folder;
 * OPFS, else IndexedDB, on the web), `'memory'` (for the app's lifetime) or `'none'`. `'auto'`,
 * the default, is `'disk'` for URLs and `'none'` for a `dataSource` and local files.
 */
export type AlphaVideoStorage = 'auto' | 'disk' | 'memory' | 'none';

/**
 * Props for the AlphaVideoView component
 */
/**
 * Playback mode for the WebM player.
 */
export type PlaybackMode = 'preload' | 'stream' | 'streamAndPreload';

export interface AlphaVideoLoadEvent {
  totalFrames: number;
  width: number;
  height: number;
  /**
   * The engine and decoders playing the file: `'software (libvpx)'`, `'software (dav1d)'` (AV1),
   * `'mediacodec-hw (c2.qti.vp9.decoder)'`, `'mediacodec-sw (...)'`, `'mediacodec (hardware, software)'`
   * for one of each. On the web: `'browser'`, `'webcodecs (vp09.00.41.08)'` (with the alpha track's
   * codec when it differs), `'software (vp9)'`, `'software (vp9, 4+2 threads)'` or `'software (libdav1d)'`.
   */
  decoder: string;
}

export interface AlphaVideoViewProps extends ViewProps {
  /** Video source URL or path */
  source: AlphaVideoSource;
  /** Auto-play video when loaded (default: true) */
  autoPlay?: boolean;
  /** Loop video playback (default: true) */
  loop?: boolean;
  /** Pause video playback (default: false) */
  paused?: boolean;
  /** Playback speed multiplier (default: 1.0) */
  playbackSpeed?: number;
  /**
   * Names the stored copy instead of the URL, for URLs that change while the file doesn't
   * (presigned URLs). `source.cacheKey` wins over it.
   */
  cacheKey?: string;

  /** See {@link AlphaVideoStorage}. */
  storage?: AlphaVideoStorage;

  /**
   * Cap on the bytes this player stores across every clip, evicting the least recently played
   * (default 512 MB).
   */
  maxStorageBytes?: number;
  
  /**
   * Playback mode for the player.
   * - 'stream' (default): Decode frames on-demand with a sliding window cache.
   * - 'preload': Decode all frames upfront before playback starts.
   * - 'streamAndPreload': Start playing immediately while decoding in background.
   *
   * Applies to the software decoders only: a file MediaCodec plays on Android (see `decoder`) decodes as it is read.
   */
  mode?: PlaybackMode;

  /**
   * Android: the decoder for files with colour and alpha as separate VP9/AV1 bitstreams
   * (default `'auto'`; see {@link Decoder}). `onLoad` reports the one used. iOS always uses
   * the C++ core; the web has `webDecoder`.
   */
  decoder?: Decoder;

  /** Web: the engine (default `'auto'`; see {@link WebDecoder}). `onLoad` reports the one used. */
  webDecoder?: WebDecoder;
  
  /**
   * @deprecated Use `mode` instead.
   * Enable streaming mode for low RAM usage.
   * When true: decode on-demand with sliding window cache (good for long videos like YouTube)
   * When false: preload all frames to RAM (good for short loops like glow effects)
   * When omitted, `mode` (default 'stream') applies.
   */
  stream?: boolean;
  
  /**
   * Number of frames to keep in cache when streaming.
   * Default: 30 frames (~1 second at 30fps)
   * Only used when mode='stream'
   */
  streamCacheSize?: number;
  
  /**
   * Number of frames to prefetch ahead of playback position when streaming.
   * Default: 15 frames (~0.5 seconds at 30fps)
   * Only used when mode='stream'
   */
  streamPrefetchAhead?: number;
  
  /**
   * Enable global decode queue for non-stream mode.
   * When true (default): decoding is serialized across all players, preventing CPU saturation
   * When false: each player decodes independently (can freeze with many)
   * Has no effect when mode='stream'.
   */
  useDecodeQueue?: boolean;
  
  /**
   * Enable verbose per-frame logging for debugging.
   * When true: logs every frame queue, release, and render event.
   * When false (default): only logs lifecycle events and periodic performance stats.
   */
  verbose?: boolean;
  
  /** Container style */
  style?: StyleProp<ViewStyle>;
  /** Callback when video loads */
  onLoad?: (event: AlphaVideoLoadEvent) => void;
  /**
   * Callback when error occurs. On the web, `code` tells the errors apart: `'LOAD_FAILED'`,
   * `'UNSUPPORTED_FILE'`, `'NO_ALPHA_PATH'`, `'DECODER_UNSUPPORTED'`, `'DECODE_FAILED'` or `'WEBGL_UNSUPPORTED'`.
   */
  onError?: (event: { error: string; code?: string }) => void;
  /** Callback when video ends */
  onEnd?: (event: {}) => void;
}

/**
 * Methods exposed by the AlphaVideoView ref
 */
export interface AlphaVideoViewRef {
  play: () => void;
  pause: () => void;
  stop: () => void;
  /** Shows that frame (clamped to the clip) and keeps playing or stays paused. */
  seekToFrame: (frame: number) => void;
  /** Seeks to the frame showing at that time, in seconds. */
  seekToTime: (seconds: number) => void;
}

// Native component for iOS and Android, from the codegen spec: a Fabric component on the new
// architecture, a view manager on the old one.
const NativeAlphaVideoViewOrNull = Platform.OS !== 'web' ? NativeAlphaVideoView : null;

/** `mode` wins over the deprecated `stream`. */
function resolveMode(mode: PlaybackMode | undefined, stream: boolean | undefined): PlaybackMode | undefined {
  if (mode) return mode;
  if (stream == null) return undefined;
  return stream ? 'stream' : 'preload';
}

/**
 * Native implementation for iOS and Android
 */
const NativePlayer = forwardRef<AlphaVideoViewRef, AlphaVideoViewProps>(
  (props, ref) => {
    const nativeRef = useRef<React.ElementRef<typeof NativeAlphaVideoView>>(null);

    const withView = (command: (view: React.ElementRef<typeof NativeAlphaVideoView>) => void) => {
      if (nativeRef.current) command(nativeRef.current);
    };

    useImperativeHandle(ref, () => ({
      play: () => withView(view => Commands.play(view)),
      pause: () => withView(view => Commands.pause(view)),
      stop: () => withView(view => Commands.stop(view)),
      seekToFrame: (frame: number) => withView(view => Commands.seekToFrame(view, frame)),
      seekToTime: (seconds: number) => withView(view => Commands.seekToTime(view, seconds)),
    }));

    const { uri, headers, dataSource } = props.source ?? {};
    const headersKey = headers ? JSON.stringify(headers) : '';
    // Rebuilt only when a value changes, so a new source object each render doesn't reload
    const nativeSource = useMemo(
      () => ({
        uri,
        headers: headers ? ([] as string[]).concat(...Object.entries(headers)) : undefined,
        dataSource,
      }),
      // eslint-disable-next-line react-hooks/exhaustive-deps
      [uri, headersKey, dataSource],
    );

    if (!NativeAlphaVideoViewOrNull) {
      return null;
    }

    // Wrap native event handlers to extract nativeEvent data
    const handleNativeLoad = props.onLoad 
      ? (event: { nativeEvent: AlphaVideoLoadEvent }) => {
          props.onLoad?.(event.nativeEvent);
        }
      : undefined;

    const handleNativeError = props.onError
      ? (event: { nativeEvent: { error: string } }) => {
          props.onError?.(event.nativeEvent);
        }
      : undefined;

    const handleNativeEnd = props.onEnd
      ? (event: { nativeEvent: {} }) => {
          props.onEnd?.(event.nativeEvent);
        }
      : undefined;

    return (
      <NativeAlphaVideoViewOrNull
        ref={nativeRef}
        source={nativeSource}
        autoPlay={props.autoPlay}
        loop={props.loop}
        paused={props.paused}
        playbackSpeed={props.playbackSpeed}
        cacheKey={props.source?.cacheKey || props.cacheKey}
        storage={props.storage}
        maxStorageBytes={props.maxStorageBytes}
        mode={resolveMode(props.mode, props.stream)}
        decoder={props.decoder}
        streamCacheSize={props.streamCacheSize}
        streamPrefetchAhead={props.streamPrefetchAhead}
        useDecodeQueue={props.useDecodeQueue}
        verbose={props.verbose}
        style={props.style}
        onLoad={handleNativeLoad}
        onError={handleNativeError}
        onEnd={handleNativeEnd}
      />
    );
  }
);

NativePlayer.displayName = 'NativePlayer';

/**
 * AlphaVideoView - Cross-platform WebM VP9/AV1 video player with alpha transparency support
 * 
 * Uses native C++ implementation with Metal (iOS) and OpenGL ES (Android) for optimal performance.
 * On the web, alpha-video-web: `<video>` where the browser keeps the alpha, otherwise WebCodecs or
 * WebAssembly decoding drawn in WebGL2.
 * 
 * @example
 * ```tsx
 * <AlphaVideoView
 *   source={{ uri: 'https://example.com/video.webm' }}
 *   autoPlay={true}
 *   loop={true}
 *   style={{ width: 300, height: 300 }}
 * />
 * ```
 */
export const AlphaVideoView = forwardRef<AlphaVideoViewRef, AlphaVideoViewProps>(
  (props, ref) => {
    if (Platform.OS === 'web') {
      return <WebPlayer ref={ref} {...props} />;
    }

    return <NativePlayer ref={ref} {...props} />;
  }
);

AlphaVideoView.displayName = 'AlphaVideoView';
