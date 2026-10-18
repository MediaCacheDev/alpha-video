/**
 * The web player: alpha-video-web, which shows the alpha in every browser. It plays in `<video>`
 * where the browser keeps WebM alpha, otherwise through WebCodecs or WebAssembly decoding drawn
 * in WebGL2, loading those engines only when a file needs them.
 */

import React, { forwardRef } from 'react';
import { View, StyleSheet } from 'react-native';
import { AlphaVideo } from 'alpha-video-web/react';
import { isUriSource, type AlphaVideoViewProps, type AlphaVideoViewRef } from './AlphaVideoView';

/** asset://path names the app's bundled media; on the web it is served from the site root. */
function toWebUrl(uri: string): string {
  return uri.startsWith('asset://') ? '/' + uri.slice('asset://'.length).replace(/^\/+/, '') : uri;
}

export const WebPlayer = forwardRef<AlphaVideoViewRef, AlphaVideoViewProps>((props, ref) => {
  const { source, autoPlay = true, loop = true, paused = false, playbackSpeed = 1, webDecoder = 'auto', cacheKey } = props;
  const { storage, maxStorageBytes } = props;
  const { style, onLoad, onError, onEnd } = props;
  // Streamed by range from http(s) URLs and kept in the browser's storage (OPFS, else IndexedDB)
  // by the canvas engines; <video> uses the HTTP cache. A Blob or byte source plays as it is.
  const uriSource = isUriSource(source) ? source : undefined;
  const src = isUriSource(source) ? (source.uri ? toWebUrl(source.uri) : undefined) : source;

  return (
    <View style={[styles.container, style]}>
      {src ? (
        <AlphaVideo
          ref={ref}
          src={src}
          autoPlay={autoPlay}
          loop={loop}
          paused={paused}
          playbackSpeed={playbackSpeed}
          decoder={webDecoder}
          headers={uriSource?.headers}
          cacheKey={uriSource?.cacheKey || cacheKey}
          storage={storage === 'disk' ? 'auto' : storage}
          maxStorageBytes={maxStorageBytes}
          style={fill}
          onLoad={({ totalFrames, width, height, decoder }) => onLoad?.({ totalFrames, width, height, decoder })}
          onError={(error) => onError?.({ error: error.message, code: error.code })}
          onEnd={() => onEnd?.({})}
        />
      ) : null}
    </View>
  );
});

WebPlayer.displayName = 'WebPlayer';

const styles = StyleSheet.create({
  container: {
    overflow: 'hidden',
  },
});

// A DOM style: AlphaVideo renders a <div>
const fill: React.CSSProperties = { width: '100%', height: '100%' };
