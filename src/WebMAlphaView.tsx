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
  UIManager,
  findNodeHandle,
} from 'react-native';
import NativeWebMAlphaView from './WebMAlphaViewNativeComponent';

// Web video element type
type VideoElement = HTMLVideoElement;

/**
 * Source types for WebM video
 */
export interface WebMSource {
  /**
   * https:// (downloaded to the cache), file://, an absolute path, asset://relative/path
   * (app bundle / APK assets; the site root on web), content:// (Android), or
   * Image.resolveAssetSource(require(...)).uri
   */
  uri: string;
}

/**
 * Props for the WebMAlphaView component
 */
/**
 * Playback mode for the WebM player.
 */
export type PlaybackMode = 'preload' | 'stream' | 'streamAndPreload';

export interface WebMAlphaViewProps extends ViewProps {
  /** Video source URL or path */
  source: WebMSource;
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
   */
  mode?: PlaybackMode;
  
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
  onLoad?: (event: { totalFrames: number; width: number; height: number }) => void;
  /** Callback when error occurs */
  onError?: (event: { error: string }) => void;
  /** Callback when video ends */
  onEnd?: (event: {}) => void;
}

/**
 * Methods exposed by the WebMAlphaView ref
 */
export interface WebMAlphaViewRef {
  play: () => void;
  pause: () => void;
  stop: () => void;
  seekToFrame: (frame: number) => void;
}

// Native component for iOS and Android. An old-architecture view (requireNativeComponent); on
// the new architecture it runs through React Native's interop layer.
const NativeWebMAlphaViewOrNull = Platform.OS !== 'web' ? NativeWebMAlphaView : null;

/** asset://path names the app's bundled media; on the web it is served from the site root. */
function toWebUrl(uri: string | undefined): string | undefined {
  return uri?.startsWith('asset://') ? '/' + uri.slice('asset://'.length).replace(/^\/+/, '') : uri;
}

const WebVideoPlayer = forwardRef<WebMAlphaViewRef, WebMAlphaViewProps>(
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
      seekToFrame: (frame: number) => {
        if (videoRef.current) {
          const fps = 30;
          videoRef.current.currentTime = frame / fps;
        }
      },
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

      const fps = 30;
      const totalFrames = Math.floor(video.duration * fps);
      
      onLoad?.({
        totalFrames,
        width: video.videoWidth,
        height: video.videoHeight,
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
const NativePlayer = forwardRef<WebMAlphaViewRef, WebMAlphaViewProps>(
  (props, ref) => {
    const nativeRef = useRef<any>(null);

    useImperativeHandle(ref, () => ({
      play: () => {
        if (nativeRef.current) {
          const handle = findNodeHandle(nativeRef.current);
          if (handle) {
            UIManager.dispatchViewManagerCommand(
              handle,
              UIManager.getViewManagerConfig('RNWebMAlphaView')?.Commands?.play ?? 'play',
              []
            );
          }
        }
      },
      pause: () => {
        if (nativeRef.current) {
          const handle = findNodeHandle(nativeRef.current);
          if (handle) {
            UIManager.dispatchViewManagerCommand(
              handle,
              UIManager.getViewManagerConfig('RNWebMAlphaView')?.Commands?.pause ?? 'pause',
              []
            );
          }
        }
      },
      stop: () => {
        if (nativeRef.current) {
          const handle = findNodeHandle(nativeRef.current);
          if (handle) {
            UIManager.dispatchViewManagerCommand(
              handle,
              UIManager.getViewManagerConfig('RNWebMAlphaView')?.Commands?.stop ?? 'stop',
              []
            );
          }
        }
      },
      seekToFrame: (frame: number) => {
        if (nativeRef.current) {
          const handle = findNodeHandle(nativeRef.current);
          if (handle) {
            UIManager.dispatchViewManagerCommand(
              handle,
              UIManager.getViewManagerConfig('RNWebMAlphaView')?.Commands?.seekToFrame ?? 'seekToFrame',
              [frame]
            );
          }
        }
      },
    }));

    if (!NativeWebMAlphaViewOrNull) {
      return null;
    }

    // Wrap native event handlers to extract nativeEvent data
    const handleNativeLoad = props.onLoad 
      ? (event: { nativeEvent: { totalFrames: number; width: number; height: number } }) => {
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
      <NativeWebMAlphaViewOrNull
        ref={nativeRef}
        source={props.source}
        autoPlay={props.autoPlay}
        loop={props.loop}
        paused={props.paused}
        playbackSpeed={props.playbackSpeed}
        cacheKey={props.cacheKey}
        mode={props.mode}
        stream={props.stream}
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
 * WebMAlphaView - Cross-platform WebM VP9/AV1 video player with alpha transparency support
 * 
 * Uses native C++ implementation with Metal (iOS) and OpenGL ES (Android) for optimal performance
 * Falls back to HTML5 video element on web platforms
 * 
 * @example
 * ```tsx
 * <WebMAlphaView
 *   source={{ uri: 'https://example.com/video.webm' }}
 *   autoPlay={true}
 *   loop={true}
 *   style={{ width: 300, height: 300 }}
 * />
 * ```
 */
export const WebMAlphaView = forwardRef<WebMAlphaViewRef, WebMAlphaViewProps>(
  (props, ref) => {
    if (Platform.OS === 'web') {
      return <WebVideoPlayer ref={ref} {...props} />;
    }

    return <NativePlayer ref={ref} {...props} />;
  }
);

WebMAlphaView.displayName = 'WebMAlphaView';

const styles = StyleSheet.create({
  container: {
    overflow: 'hidden',
  },
});

// Default export
export default WebMAlphaView;
