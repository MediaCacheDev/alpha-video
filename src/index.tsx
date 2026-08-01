/**
 * Package entry point.
 *
 * The player view itself lives in `WebMAlphaView.tsx` and the glow button in `GlowButton.tsx`;
 * this file holds the native-module utilities and re-exports the rest. Splitting them keeps a
 * component that is BUILT ON the player from having to import the barrel that exports it.
 */

import { Platform, NativeModules } from 'react-native';

/**
 * Decoder status information (Android only)
 */
export interface DecoderStatus {
  /** Maximum VP9 decoder instances supported by the device */
  maxVp9Instances: number;
  /** Maximum AV1 decoder instances supported by the device */
  maxAv1Instances: number;
  /** Whether VP9 hardware decoder is available */
  vp9HardwareAvailable: boolean;
  /** Whether AV1 hardware decoder is available */
  av1HardwareAvailable: boolean;
  /** Currently active decoder instances */
  activeDecoders: number;
  /** Remaining available VP9 decoder slots */
  availableVp9Slots: number;
  /** Maximum dual-stream players possible */
  maxDualStreamPlayers: number;
  /** Whether we can create more dual-stream players */
  canCreateMore: boolean;
}

/**
 * Native module for WebM Alpha Player utilities (Android only)
 */
const RNWebMAlphaPlayerModule = Platform.OS === 'android' 
  ? NativeModules.RNWebMAlphaPlayer 
  : null;

/**
 * Get detailed decoder information for all supported codecs (VP9, AV1).
 * Returns a string with all decoder capabilities including max instances,
 * hardware acceleration status, supported resolutions, etc.
 * 
 * @returns Promise<string> - Detailed decoder information
 * @platform android
 */
export async function getDecoderInfo(): Promise<string> {
  if (Platform.OS !== 'android') {
    return 'Decoder info only available on Android';
  }
  if (!RNWebMAlphaPlayerModule) {
    throw new Error('RNWebMAlphaPlayer native module not available');
  }
  return RNWebMAlphaPlayerModule.getDecoderInfo();
}

/** @deprecated Use getDecoderInfo() */
export const getVp9DecoderInfo = getDecoderInfo;

/**
 * Get the maximum number of concurrent decoder instances for a codec.
 * Dual-stream videos require 2 instances per player.
 * 
 * @param mimeType - MIME type (e.g. 'video/x-vnd.on2.vp9' or 'video/av01')
 * @returns Promise<number> - Max instances (-1 if unable to determine)
 * @platform android
 */
export async function getMaxDecoderInstances(mimeType: string): Promise<number> {
  if (Platform.OS !== 'android') {
    return -1;
  }
  if (!RNWebMAlphaPlayerModule) {
    throw new Error('RNWebMAlphaPlayer native module not available');
  }
  return RNWebMAlphaPlayerModule.getMaxDecoderInstances(mimeType);
}

/** @deprecated Use getMaxDecoderInstances() */
export async function getMaxVp9Instances(): Promise<number> {
  if (Platform.OS !== 'android') {
    return -1;
  }
  if (!RNWebMAlphaPlayerModule) {
    throw new Error('RNWebMAlphaPlayer native module not available');
  }
  return RNWebMAlphaPlayerModule.getMaxVp9Instances();
}

/**
 * Get the current number of active VP9 decoder instances.
 * 
 * @returns Promise<number> - Active decoder count
 * @platform android
 */
export async function getActiveDecoderCount(): Promise<number> {
  if (Platform.OS !== 'android') {
    return 0;
  }
  if (!RNWebMAlphaPlayerModule) {
    throw new Error('RNWebMAlphaPlayer native module not available');
  }
  return RNWebMAlphaPlayerModule.getActiveDecoderCount();
}

/**
 * Get comprehensive decoder status as a structured object.
 * Useful for determining if more dual-stream players can be created.
 * 
 * @returns Promise<DecoderStatus> - Decoder status information
 * @platform android
 * 
 * @example
 * ```ts
 * const status = await getDecoderStatus();
 * console.log(`Can create ${status.maxDualStreamPlayers} dual-stream players`);
 * console.log(`Currently using ${status.activeDualStreamPlayers}`);
 * if (!status.canCreateMore) {
 *   console.warn('At decoder limit!');
 * }
 * ```
 */
export async function getDecoderStatus(): Promise<DecoderStatus> {
  if (Platform.OS !== 'android') {
    return {
      maxVp9Instances: -1,
      maxAv1Instances: -1,
      vp9HardwareAvailable: false,
      av1HardwareAvailable: false,
      activeDecoders: 0,
      availableVp9Slots: -1,
      maxDualStreamPlayers: -1,
      canCreateMore: true,
    };
  }
  if (!RNWebMAlphaPlayerModule) {
    throw new Error('RNWebMAlphaPlayer native module not available');
  }
  return RNWebMAlphaPlayerModule.getDecoderStatus();
}

export {
  WebMAlphaView,
  type WebMAlphaViewProps,
  type WebMAlphaViewRef,
  type WebMSource,
  type PlaybackMode,
} from './WebMAlphaView';

/**
 * A button whose glow is a pre-rendered animation, driven by one multi-state sheet.
 *
 * Pairs with react-native-animated-glow's studio export: rows for `default`, `hover` and `press`
 * in one file, plus the per-row playback rates that put each state's own speed back.
 */
export { GlowButton, type GlowButtonProps } from './GlowButton';

export {
  DEFAULT_GLOW_MARGIN,
  STATE_ORDER,
  glowSheetLayout,
  rateForRow,
  rowForState,
  rowOffset,
  type GlowSheetLayout,
  type GlowState,
  type Size,
} from './glowSheet';

// Kept as the package default for callers who imported it that way before the split.
export { default } from './WebMAlphaView';
