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