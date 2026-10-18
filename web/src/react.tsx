/**
 * The player as a React component (React only, no React Native):
 *
 *   import { AlphaVideo } from 'alpha-video-web/react';
 *
 *   <AlphaVideo src="/glow.webm" style={{ width: 300, height: 300 }} />
 */
import React, { forwardRef, useEffect, useImperativeHandle, useRef } from 'react';
import type { AlphaVideoError } from './errors';
import { createAlphaVideo } from './player';
import type { AlphaVideoLoadEvent, AlphaVideoPlayer, AlphaVideoSource, AlphaVideoStorage, Decoder, ObjectFit } from './types';

export type { AlphaVideoLoadEvent, AlphaVideoSource, AlphaVideoStorage, Decoder, ObjectFit };

export interface AlphaVideoProps {
  src: AlphaVideoSource;
  /** default true */
  autoPlay?: boolean;
  /** default true */
  loop?: boolean;
  /** default false */
  paused?: boolean;
  /** default 1 */
  playbackSpeed?: number;
  /** default `'auto'`; see {@link Decoder} */
  decoder?: Decoder;
  /** default `'contain'` */
  objectFit?: ObjectFit;
  fetchInit?: RequestInit;
  /** Sent with every request for a URL, e.g. Authorization */
  headers?: Record<string, string>;
  /** Names the stored copy, for URLs that change while the file doesn't; defaults to the URL */
  cacheKey?: string;
  /** default `'auto'` for URLs, `'none'` for the rest; see {@link AlphaVideoStorage} */
  storage?: AlphaVideoStorage;
  /** default 512 MB */
  maxStorageBytes?: number;
  /** The container's class; the video fills it */
  className?: string;
  style?: React.CSSProperties;
  onLoad?: (event: AlphaVideoLoadEvent) => void;
  onError?: (error: AlphaVideoError) => void;
  onEnd?: () => void;
}

/** Seeks show the frame (clamped to the clip) and keep playing, or stay paused. */
export interface AlphaVideoRef {
  play(): void;
  pause(): void;
  /** Pauses on the first frame */
  stop(): void;
  seekToFrame(frame: number): void;
  /** Seeks to the frame showing at that time, in seconds */
  seekToTime(seconds: number): void;
}

export const AlphaVideo = forwardRef<AlphaVideoRef, AlphaVideoProps>(function AlphaVideo(
  {
    src,
    autoPlay = true,
    loop = true,
    paused = false,
    playbackSpeed = 1,
    decoder = 'auto',
    objectFit = 'contain',
    fetchInit,
    headers,
    cacheKey,
    storage,
    maxStorageBytes,
    className,
    style,
    onLoad,
    onError,
    onEnd,
  },
  ref,
) {
  // Headers are usually a new object each render; a new player only when they change
  const headersKey = headers ? JSON.stringify(headers) : '';
  const containerRef = useRef<HTMLDivElement>(null);
  const playerRef = useRef<AlphaVideoPlayer | null>(null);
  const callbacks = useRef({ onLoad, onError, onEnd });
  callbacks.current = { onLoad, onError, onEnd };
  const latest = useRef({ paused, loop, playbackSpeed });
  latest.current = { paused, loop, playbackSpeed };

  useImperativeHandle(
    ref,
    () => ({
      play: () => playerRef.current?.play(),
      pause: () => playerRef.current?.pause(),
      stop: () => playerRef.current?.stop(),
      seekToFrame: (frame) => playerRef.current?.seekToFrame(frame),
      seekToTime: (seconds) => playerRef.current?.seekToTime(seconds),
    }),
    [],
  );

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;
    const player = createAlphaVideo(container, {
      src,
      autoPlay: autoPlay && !latest.current.paused,
      loop: latest.current.loop,
      playbackSpeed: latest.current.playbackSpeed,
      decoder,
      objectFit,
      fetchInit,
      headers,
      cacheKey,
      storage,
      maxStorageBytes,
      onLoad: (e) => callbacks.current.onLoad?.(e),
      onError: (e) => callbacks.current.onError?.(e),
      onEnd: () => callbacks.current.onEnd?.(),
    });
    playerRef.current = player;
    return () => {
      player.destroy();
      playerRef.current = null;
    };
    // A new player for a new file or engine; the other props are applied to the running one
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [src, decoder, objectFit, headersKey, cacheKey, storage, maxStorageBytes]);

  const first = useRef(true);
  useEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    if (paused) playerRef.current?.pause();
    else playerRef.current?.play();
  }, [paused]);
  useEffect(() => playerRef.current?.setLoop(loop), [loop]);
  useEffect(() => playerRef.current?.setPlaybackSpeed(playbackSpeed), [playbackSpeed]);

  return <div ref={containerRef} className={className} style={style} />;
});
