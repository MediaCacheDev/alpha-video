/**
 * Picks an engine for a file and runs it in a container element: `<video>` if this browser
 * keeps WebM alpha and the file is one it plays, else WebCodecs into a canvas, else the
 * software decoders into a canvas.
 */
import { CanvasEngine } from './canvas-engine';
import { WasmDemuxer } from './demuxer';
import type { Engine, EngineEvents, EngineOptions } from './engine';
import { AlphaVideoError } from './errors';
import { Input } from './input';
import { videoElementKeepsWebMAlpha } from './probe';
import { sniffWebM } from './sniff';
import { SoftwareSource } from './software-source';
import type { AlphaVideoLoadEvent, AlphaVideoOptions, AlphaVideoPlayer, Decoder } from './types';
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

  Promise.resolve()
    .then(() => startEngine(new Input(options), options.decoder ?? 'auto', engineOptions, events, abort.signal))
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
  input: Input,
  decoder: Decoder,
  options: EngineOptions,
  events: EngineEvents,
  signal: AbortSignal,
): Promise<Engine> {
  if (decoder === 'browser') return videoEngine(input, options, events, signal);
  if (decoder !== 'auto') {
    const demuxer = await demux(input, signal);
    if (!demuxer.info.hasAlpha) {
      demuxer.close();
      throw new AlphaVideoError('UNSUPPORTED_FILE', 'The file has no alpha; play it with decoder "browser"');
    }
    return withDemuxer(demuxer, () =>
      decoder === 'software' ? softwareEngine(demuxer, options, events) : webCodecsEngine(demuxer, HARDWARE[decoder], options, events),
    );
  }

  const keepsAlpha = await videoElementKeepsWebMAlpha();
  if (keepsAlpha) {
    // Only a dual-stream file needs more than <video>; the first bytes tell
    let head: Uint8Array | null;
    try {
      head = await input.head(signal);
    } catch (e) {
      if (signal.aborted) throw e;
      // Unreadable from script (CORS), so <video> is all that can play it
      if (input.videoLoadable) return videoEngine(input, options, events, signal);
      throw new AlphaVideoError('LOAD_FAILED', `Fetching ${String(input.src)} failed: ${(e as Error)?.message ?? e}`);
    }
    if (head && sniffWebM(head)?.layout !== 'dual') return videoEngine(input, options, events, signal);
  }
  const demuxer = await demux(input, signal);
  if (!demuxer.info.hasAlpha || (keepsAlpha && demuxer.info.alphaInBlockAdditional)) {
    demuxer.close();
    return videoEngine(input, options, events, signal);
  }
  return withDemuxer(demuxer, async () => {
    try {
      return await webCodecsEngine(demuxer, 'no-preference', options, events);
    } catch (e) {
      if (!(e instanceof AlphaVideoError && e.code === 'DECODER_UNSUPPORTED')) throw e;
      try {
        return await softwareEngine(demuxer, options, events);
      } catch (software) {
        if (!(software instanceof AlphaVideoError && software.code === 'DECODER_UNSUPPORTED')) throw software;
        throw new AlphaVideoError('NO_ALPHA_PATH', `No engine in this browser shows this file's alpha (${e.message}; ${software.message})`);
      }
    }
  });
}

async function videoEngine(input: Input, options: EngineOptions, events: EngineEvents, signal: AbortSignal): Promise<Engine> {
  const { url, owned } = await input.videoUrl(signal);
  return new VideoEngine(url, options, events, owned ? url : undefined);
}

/** Closes `demuxer` if `start` fails; once it succeeds, the engine's source owns it. */
async function withDemuxer(demuxer: WasmDemuxer, start: () => Promise<Engine>): Promise<Engine> {
  try {
    return await start();
  } catch (e) {
    demuxer.close();
    throw e;
  }
}

function webCodecsEngine(
  demuxer: WasmDemuxer,
  hardwareAcceleration: HardwareAcceleration,
  options: EngineOptions,
  events: EngineEvents,
): Promise<Engine> {
  return CanvasEngine.create(
    (callbacks) => WebCodecsSource.create(demuxer, hardwareAcceleration, options.loop, callbacks),
    options,
    events,
  );
}

function softwareEngine(demuxer: WasmDemuxer, options: EngineOptions, events: EngineEvents): Promise<Engine> {
  return CanvasEngine.create((callbacks) => SoftwareSource.create(demuxer, options.loop, callbacks), options, events);
}

async function demux(input: Input, signal: AbortSignal): Promise<WasmDemuxer> {
  const demuxer = await WasmDemuxer.open(await input.open(signal));
  if (!demuxer) throw new AlphaVideoError('UNSUPPORTED_FILE', 'Not a WebM with a VP9 or AV1 track');
  return demuxer;
}
