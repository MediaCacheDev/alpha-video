/**
 * The player view, split out from the package barrel.
 *
 * Separate from `index.tsx` so that anything built ON the player can import it without the barrel
 * importing that code back. A cycle through the entry point happens to work while every reference
 * is inside a render, which makes it a trap rather than a design.
 */

import React, { useRef, useEffect, useState, forwardRef, useImperativeHandle } from 'react';
import {
  Platform,
  View,
  StyleSheet,
  ViewProps,
  StyleProp,
  ViewStyle,
} from 'react-native';
import NativeAlphaVideoView, { Commands, type Decoder } from './AlphaVideoViewNativeComponent';

export type { Decoder };

// Web video element type
type VideoElement = HTMLVideoElement;

/**
 * Source types for WebM video
 */
export interface AlphaVideoSource {
  /**
   * https:// (downloaded to the cache), file://, an absolute path, asset://relative/path
   * (app bundle / APK assets; the site root on web), content:// (Android), or
   * Image.resolveAssetSource(require(...)).uri
   */
  uri: string;
}

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
   * The engine and decoders playing the file: `'ffmpeg (libvpx-vp9)'`,
   * `'mediacodec-hw (c2.qti.vp9.decoder)'`, `'mediacodec-sw (...)'`, `'mediacodec (hardware, software)'`
   * for one of each, or `'browser'` on web.
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
   * Custom cache key/path for downloaded files.
   * If provided, downloaded files will be saved to this path under the cache directory.
  * Example: 'videos/custom-paths/abc123-60x60.webm'
   * If not provided, files are cached using a hash of the URL.
   */
  cacheKey?: string;
  
  /**
   * Playback mode for the player.
   * - 'preload' (default): Decode all frames upfront before playback starts.
   * - 'stream': Decode frames on-demand with a sliding window cache.
   * - 'streamAndPreload': Start playing immediately while decoding in background.
   *
   * Applies to FFmpeg only: a file MediaCodec plays on Android (see `decoder`) decodes as it is read.
   */
  mode?: PlaybackMode;

  /**
   * Android: the decoder for files with colour and alpha as separate VP9/AV1 bitstreams
   * (default `'auto'`; see {@link Decoder}). `onLoad` reports the one used. iOS always uses
   * FFmpeg and web the browser.
   */
  decoder?: Decoder;
  
  /**
   * @deprecated Use `mode` instead.
   * Enable streaming mode for low RAM usage.
   * When true: decode on-demand with sliding window cache (good for long videos like YouTube)
   * When false (default): preload all frames to RAM (good for short loops like glow effects)
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
  /** Callback when error occurs */
  onError?: (event: { error: string }) => void;
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

/** asset://path names the app's bundled media; on the web it is served from the site root. */
function toWebUrl(uri: string | undefined): string | undefined {
  return uri?.startsWith('asset://') ? '/' + uri.slice('asset://'.length).replace(/^\/+/, '') : uri;
}

/** The browser doesn't expose the frame rate. */
const WEB_FPS = 30;

/** Clamped to the clip; ignored until its duration is known. */
function seekVideoToFrame(video: VideoElement | null, frame: number) {
  if (!video || !(video.duration > 0)) return;
  const lastFrame = Math.max(0, Math.floor(video.duration * WEB_FPS) - 1);
  const target = Math.min(Math.max(0, Math.floor(frame) || 0), lastFrame);
  // Chrome moves a second seek into the last frame to the very end, which wraps a looping clip to 0
  if (Math.floor(video.currentTime * WEB_FPS + 1e-6) === target && !video.seeking) return;
  // The middle of the frame, so rounding in the browser can't land on the one before
  video.currentTime = (target + 0.5) / WEB_FPS;
}

const WebVideoPlayer = forwardRef<AlphaVideoViewRef, AlphaVideoViewProps>(
  (props, ref) => {
    const { source, autoPlay = true, loop = true, paused = false, playbackSpeed = 1.0, style, onLoad, onError, onEnd } = props;
    const videoRef = useRef<VideoElement | null>(null);
    const [isLoaded, setIsLoaded] = useState(false);

    useImperativeHandle(ref, () => ({
      play: () => {
        videoRef.current?.play().catch(() => {});
      },
      pause: () => {
        videoRef.current?.pause();
      },
      stop: () => {
        if (videoRef.current) {
          videoRef.current.pause();
          videoRef.current.currentTime = 0;
        }
      },
      seekToFrame: (frame: number) => seekVideoToFrame(videoRef.current, frame),
      seekToTime: (seconds: number) =>
        seekVideoToFrame(videoRef.current, Math.floor((seconds > 0 ? seconds : 0) * WEB_FPS + 1e-6)),
    }));

    useEffect(() => {
      const video = videoRef.current;
      if (!video) return;
      video.playbackRate = playbackSpeed;
    }, [playbackSpeed]);

    useEffect(() => {
      const video = videoRef.current;
      if (!video || !isLoaded) return;

      if (paused) {
        video.pause();
      } else if (autoPlay) {
        video.play().catch(() => {});
      }
    }, [paused, autoPlay, isLoaded]);

    const handleLoadedMetadata = () => {
      const video = videoRef.current;
      if (!video) return;

      setIsLoaded(true);

      // Loading a source resets playbackRate to 1, and the effect above won't re-fire for a
      // source change, so a non-default rate has to be put back here.
      video.playbackRate = playbackSpeed;

      const totalFrames = Math.floor(video.duration * WEB_FPS);
      
      onLoad?.({
        totalFrames,
        width: video.videoWidth,
        height: video.videoHeight,
        decoder: 'browser',
      });

      if (autoPlay && !paused) {
        video.play().catch(() => {});
      }
    };

    const handleError = () => {
      const video = videoRef.current;
      const errorMessage = video?.error?.message || 'Unknown video error';
      onError?.({ error: errorMessage });
    };

    return (
      <View style={[styles.container, style]}>
        <video
          ref={videoRef as React.RefObject<HTMLVideoElement>}
          src={toWebUrl(source?.uri)}
          autoPlay={autoPlay && !paused}
          loop={loop}
          muted
          playsInline
          onLoadedMetadata={handleLoadedMetadata}
          onError={handleError}
          onEnded={() => onEnd?.({})}
          style={{
            width: '100%',
            height: '100%',
            objectFit: 'contain',
          }}
        />
      </View>
    );
  }
);

WebVideoPlayer.displayName = 'WebVideoPlayer';

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
        source={props.source}
        autoPlay={props.autoPlay}
        loop={props.loop}
        paused={props.paused}
        playbackSpeed={props.playbackSpeed}
        cacheKey={props.cacheKey}
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
 * Uses native C++ implementation with Metal (iOS) and OpenGL ES (Android) for optimal performance
 * Falls back to HTML5 video element on web platforms
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
      return <WebVideoPlayer ref={ref} {...props} />;
    }

    return <NativePlayer ref={ref} {...props} />;
  }
);

AlphaVideoView.displayName = 'AlphaVideoView';

const styles = StyleSheet.create({
  container: {
    overflow: 'hidden',
  },
});
