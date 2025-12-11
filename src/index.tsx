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
  uri: string;
}

/**
 * Props for the WebMAlphaView component
 */
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

// Native component for iOS and Android (Fabric-aware via codegen fallback wrapper)
const NativeWebMAlphaViewOrNull = Platform.OS !== 'web' ? NativeWebMAlphaView : null;

const WebVideoPlayer = forwardRef<WebMAlphaViewRef, WebMAlphaViewProps>(
  (props, ref) => {
    const { source, autoPlay = true, loop = true, paused = false, playbackSpeed = 1.0, style, onLoad, onError } = props;
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
          src={source?.uri}
          autoPlay={autoPlay && !paused}
          loop={loop}
          muted
          playsInline
          onLoadedMetadata={handleLoadedMetadata}
          onError={handleError}
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
 * WebMAlphaView - Cross-platform WebM VP9 video player with alpha transparency support
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
