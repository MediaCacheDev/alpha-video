import type { TurboModule } from 'react-native';
import { TurboModuleRegistry } from 'react-native';

/** The decoder utilities. Android only: iOS has no native module, so `get` returns null there. */
export interface Spec extends TurboModule {
  getDecoderInfo(): Promise<string>;
  getMaxDecoderInstances(mimeType: string): Promise<number>;
  getActiveDecoderCount(): Promise<number>;
  getDecoderStatus(): Promise<{
    maxVp9Instances: number;
    maxAv1Instances: number;
    vp9HardwareAvailable: boolean;
    av1HardwareAvailable: boolean;
    activeDecoders: number;
    availableVp9Slots: number;
    maxDualStreamPlayers: number;
    canCreateMore: boolean;
    softwareDecoders: string[];
  }>;
}

export default TurboModuleRegistry.get<Spec>('RNAlphaVideo');
