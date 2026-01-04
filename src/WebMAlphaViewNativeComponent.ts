import type { ViewProps } from 'react-native';
import type { HostComponent } from 'react-native';
import { requireNativeComponent, Platform } from 'react-native';

export interface NativeProps extends ViewProps {
  source?: {
    uri: string;
  };
  autoPlay?: boolean;
  loop?: boolean;
  paused?: boolean;
  playbackSpeed?: number;
  /** Custom cache key/path for downloaded files */
  cacheKey?: string;
  
  /**
   * Enable streaming mode for low RAM usage.
   * When true: decode on-demand with sliding window cache (good for long videos)
   * When false (default): preload all frames (good for short loops)
   */
  stream?: boolean;
  
  /**
   * Number of frames to keep in cache when streaming.
   * Default: 30 frames (~1 second at 30fps)
   * Only used when stream=true
   */
  streamCacheSize?: number;
  
  /**
   * Number of frames to prefetch ahead of playback position when streaming.
   * Default: 15 frames (~0.5 seconds at 30fps)
   * Only used when stream=true
   */
  streamPrefetchAhead?: number;
  
  onLoad?: (event: { nativeEvent: { totalFrames: number; width: number; height: number } }) => void;
  onError?: (event: { nativeEvent: { error: string } }) => void;
  onEnd?: (event: { nativeEvent: {} }) => void;
}

// Safely require the native component with error handling
// This prevents the entire module from crashing if the native component isn't available
let RNWebMAlphaView: HostComponent<NativeProps> | null = null;

if (Platform.OS !== 'web') {
  try {
    RNWebMAlphaView = requireNativeComponent<NativeProps>('RNWebMAlphaView');
  } catch (e) {
    console.warn(
      'react-native-webm-alpha-player: Failed to load native component RNWebMAlphaView.',
      'Make sure the native module is properly linked.',
      e
    );
  }
}

export default RNWebMAlphaView;