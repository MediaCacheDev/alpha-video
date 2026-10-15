/**
 * Picks an engine for a file and runs it in a container element: `<video>` if this browser
 * keeps WebM alpha and the file is one it plays, else WebCodecs into a canvas, else the
 * software decoders into a canvas.
 */
import { CanvasEngine } from './canvas-engine';
import { WasmDemuxer } from './demuxer';
import type { Engine, EngineEvents, EngineOptions } from './engine';
import { AlphaVideoError } from './errors';
import { videoElementKeepsWebMAlpha } from './probe';
import { fetchHead, sniffWebM } from './sniff';
import { SoftwareSource } from './software-source';
import type { AlphaVideoLoadEvent, AlphaVideoOptions, AlphaVideoPlayer, AlphaVideoSource, Decoder } from './types';
import { VideoEngine } from './video-engine';
import { WebCodecsSource } from './webcodecs-source';

const HARDWARE: Record<string, HardwareAcceleration> = {
  webcodecs: 'no-preference',
  'webcodecs-hw': 'prefer-hardware',
  'webcodecs-sw': 'prefer-software',
};

/** Starts loading `options.src` into `container`; the element is added once an engine is picked. */
export function createAlphaVideo(container: HTMLElement, options: AlphaVideoOptions): AlphaVideoPlayer {
  const engineOptions: EngineOptions = {
    autoPlay: options.autoPlay ?? true,
    loop: options.loop ?? true,
    playbackSpeed: options.playbackSpeed ?? 1,
    objectFit: options.objectFit ?? 'contain',
  };
  const abort = new AbortController();
  let engine: Engine | null = null;
  let destroyed = false;
  // Calls made before an engine exists are replayed on it
  let pending: ((e: Engine) => void)[] = [];
  let settle!: { resolve(e: AlphaVideoLoadEvent): void; reject(e: AlphaVideoError): void };
  const loaded = new Promise<AlphaVideoLoadEvent>((resolve, reject) => (settle = { resolve, reject }));
  loaded.catch(() => {});

  const events: EngineEvents = {
    onLoad: (e) => {
      if (destroyed) return;
      settle.resolve(e);
      options.onLoad?.(e);
    },
    onError: (e) => {
      if (destroyed) return;
      settle.reject(e);
      options.onError?.(e);
    },
    onEnd: () => {
      if (!destroyed) options.onEnd?.();
    },
  };

  const run = (call: (e: Engine) => void) => {
    if (engine) call(engine);
    else if (!destroyed) pending.push(call);
  };

  startEngine(options.src, options.decoder ?? 'auto', engineOptions, events, options.fetchInit, abort.signal)
    .then((e) => {
      if (destroyed) {
        e.destroy();
        return;
      }
      engine = e;
      container.appendChild(e.element);
      pending.forEach((call) => call(e));
      pending = [];
    })
    .catch((e: unknown) => {
      if (destroyed) return;
      events.onError(e instanceof AlphaVideoError ? e : new AlphaVideoError('LOAD_FAILED', String((e as Error)?.message ?? e)));
    });

  return {
    get element() {
      return engine?.element ?? null;
    },
    loaded,
    play: () => run((e) => e.play()),
    pause: () => run((e) => e.pause()),
    stop: () => run((e) => e.stop()),
    seekToFrame: (frame) => run((e) => e.seekToFrame(frame)),
    seekToTime: (seconds) => run((e) => e.seekToTime(seconds)),
    setPlaybackSpeed: (speed) => {
      engineOptions.playbackSpeed = speed;
      run((e) => e.setPlaybackSpeed(speed));
    },
    setLoop: (loop) => {
      engineOptions.loop = loop;
      run((e) => e.setLoop(loop));
    },
    destroy: () => {
      if (destroyed) return;
      destroyed = true;
      abort.abort();
      engine?.destroy();
      engine = null;
      pending = [];
    },
  };
}

async function startEngine(
  src: AlphaVideoSource,
  decoder: Decoder,
  options: EngineOptions,
  events: EngineEvents,
  fetchInit: RequestInit | undefined,
  signal: AbortSignal,
): Promise<Engine> {
  if (decoder === 'browser') return videoEngine(src, options, events);
  if (decoder === 'software') return softwareEngine(await loadBytes(src, fetchInit, signal), options, events);
  if (decoder !== 'auto') {
    const demuxer = await demux(await loadBytes(src, fetchInit, signal));
    if (!demuxer.info.hasAlpha) {
      demuxer.close();
      throw new AlphaVideoError('UNSUPPORTED_FILE', 'The file has no alpha; play it with decoder "browser"');
    }
    return webCodecsEngine(demuxer, HARDWARE[decoder], options, events);
  }

  const keepsAlpha = await videoElementKeepsWebMAlpha();
  let bytes: Uint8Array | null = typeof src === 'string' ? null : await loadBytes(src, fetchInit, signal);
  if (keepsAlpha) {
    // Only a dual-stream file needs more than <video>; the first bytes tell
    let head: Uint8Array;
    try {
      head = bytes ?? (await fetchHead(src as string, 65536, signal));
    } catch {
      // Unreadable from script (CORS), so <video> is all that can play it
      return videoEngine(src, options, events);
    }
    if (sniffWebM(head)?.layout !== 'dual') return videoEngine(src, options, events);
  }
  bytes ??= await loadBytes(src, fetchInit, signal);
  const demuxer = await demux(bytes);
  if (!demuxer.info.hasAlpha || (keepsAlpha && demuxer.info.alphaInBlockAdditional)) {
    demuxer.close();
    return videoEngine(typeof src === 'string' ? src : bytes, options, events);
  }
  try {
    return await webCodecsEngine(demuxer, 'no-preference', options, events);
  } catch (e) {
    if (!(e instanceof AlphaVideoError && e.code === 'DECODER_UNSUPPORTED')) throw e;
    try {
      return await softwareEngine(bytes, options, events);
    } catch (software) {
      if (!(software instanceof AlphaVideoError && software.code === 'DECODER_UNSUPPORTED')) throw software;
      throw new AlphaVideoError('NO_ALPHA_PATH', `No engine in this browser shows this file's alpha (${e.message}; ${software.message})`);
    }
  }
}

function videoEngine(src: AlphaVideoSource, options: EngineOptions, events: EngineEvents): Engine {
  if (typeof src === 'string') return new VideoEngine(src, options, events);
  const url = URL.createObjectURL(src instanceof Blob ? src : new Blob([src as BlobPart], { type: 'video/webm' }));
  return new VideoEngine(url, options, events, url);
}

/** Takes ownership of `demuxer`. */
async function webCodecsEngine(
  demuxer: WasmDemuxer,
  hardwareAcceleration: HardwareAcceleration,
  options: EngineOptions,
  events: EngineEvents,
): Promise<Engine> {
  try {
    return await CanvasEngine.create(
      (callbacks) => WebCodecsSource.create(demuxer, hardwareAcceleration, options.loop, callbacks),
      options,
      events,
    );
  } catch (e) {
    demuxer.close();
    throw e;
  }
}

function softwareEngine(bytes: Uint8Array, options: EngineOptions, events: EngineEvents): Promise<Engine> {
  return CanvasEngine.create((callbacks) => SoftwareSource.create(bytes, options.loop, callbacks), options, events);
}

async function demux(bytes: Uint8Array): Promise<WasmDemuxer> {
  const demuxer = await WasmDemuxer.open(bytes);
  if (!demuxer) throw new AlphaVideoError('UNSUPPORTED_FILE', 'Not a WebM with a VP9 or AV1 track');
  return demuxer;
}

async function loadBytes(src: AlphaVideoSource, init: RequestInit | undefined, signal: AbortSignal): Promise<Uint8Array> {
  if (src instanceof Uint8Array) return src;
  if (src instanceof ArrayBuffer) return new Uint8Array(src);
  if (src instanceof Blob) return new Uint8Array(await src.arrayBuffer());
  let response: Response;
  try {
    response = await fetch(src, { ...init, signal });
  } catch (e) {
    if (signal.aborted) throw e;
    throw new AlphaVideoError('LOAD_FAILED', `Fetching ${src} failed; a file on another origin needs CORS headers (${(e as Error).message})`);
  }
  if (!response.ok) throw new AlphaVideoError('LOAD_FAILED', `HTTP ${response.status} for ${src}`);
  return new Uint8Array(await response.arrayBuffer());
}
