// iOS and Android bundles get this instead of WebPlayer.web.tsx, so they never include alpha-video-web.
import { forwardRef } from 'react';
import type { AlphaVideoViewProps, AlphaVideoViewRef } from './AlphaVideoView';

export const WebPlayer = forwardRef<AlphaVideoViewRef, AlphaVideoViewProps>(() => null);

WebPlayer.displayName = 'WebPlayer';
