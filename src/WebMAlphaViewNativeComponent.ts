import type { ViewProps } from 'react-native';
import type { HostComponent } from 'react-native';
import { requireNativeComponent, Platform } from 'react-native';

/**
 * Playback mode for the WebM player.
 * 
 * - `'preload'` (default): Decode all frames upfront before playback starts.
 *   Best for short loops. Zero CPU during playback, instant looping.
 * 
 * - `'stream'`: Decode frames on-demand with a sliding window cache.
 *   Best for long videos or memory-constrained devices. Frames are evicted
 *   from cache, so looping requires re-decoding.
 * 
 * - `'streamAndPreload'`: Start playing immediately while decoding in background.
 *   After first pass completes, all frames are cached for instant looping.
 *   Best of both worlds: fast start + smooth loops.
 */
export type PlaybackMode = 'preload' | 'stream' | 'streamAndPreload';

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
   * Playback mode for the player.
   * 
   * - `'preload'` (default): Decode all frames upfront before playback starts.
   *   Best for short loops. Zero CPU during playback, instant looping.
   * 
   * - `'stream'`: Decode frames on-demand with a sliding window cache.
   *   Best for long videos or memory-constrained devices. Frames are evicted
   *   from cache, so looping requires re-decoding.
   * 
   * - `'streamAndPreload'`: Start playing immediately while decoding in background.
   *   After first pass completes, all frames are cached for instant looping.
   *   Best of both worlds: fast start + smooth loops.
   */
  mode?: PlaybackMode;
  
  /**
   * @deprecated Use `mode` instead. Will be removed in a future version.
   * `stream={true}` is equivalent to `mode="stream"`.
   * `stream={false}` is equivalent to `mode="preload"`.
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
   * Only used when mode='stream' or mode='streamAndPreload'
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