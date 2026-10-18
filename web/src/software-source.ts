/**
 * Colour and alpha decoded in WebAssembly (FFmpeg's VP9 decoder, dav1d for AV1), for streams
 * WebCodecs can't decode. The C++ demuxer runs on the page's thread; each stream decodes in its
 * own Worker, so the two decode in parallel and the page stays free. On a cross-origin isolated
 * page the decoders also run threads of their own.
 */
import { isAbort, type DemuxInfo, type DemuxPacket, type StreamInfo, type WasmDemuxer } from './demuxer';
import { AlphaVideoError } from './errors';
import { insertByTimestamp, type FramePair, type PairSource, type Planes, type SourceCallbacks } from './source';
import type { DecodedFrame, FromWorker, ToWorker } from './worker/protocol';

type Stream = 0 | 1;

/** Decoded pairs to stay ahead of the playhead */
const MAX_AHEAD = 4;
/** Packets queued per Worker: one decoding, one waiting */
const MAX_QUEUED = 2;

// AVColorSpace to VideoColorSpace names ('bt2020-ncl' isn't in TypeScript's list yet)
const MATRIX_NAMES: Record<number, string> = { 0: 'rgb', 1: 'bt709', 5: 'bt470bg', 6: 'smpte170m', 9: 'bt2020-ncl' };

interface Decoded {
  timestampUs: number;
  picture: Planes;
}

/** Decoder threads of the page's open sources, so several videos don't oversubscribe the cores */
let threadsInUse = 0;

/** Threads per decoder, colour and alpha, or null to decode single-threaded. */
function threadCounts(): [number, number] | null {
  if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') return null;
  const free = (navigator.hardwareConcurrency || 4) - threadsInUse;
  if (free < 4) return null;
  return [Math.min(4, free >> 1), Math.min(2, free >> 2)];
}

const workerUrls = new Map<boolean, Promise<string>>();

/** The decode Worker's script as a blob: URL, once per page per build. */
function workerUrl(threaded: boolean): Promise<string> {
  let url = workerUrls.get(threaded);
  if (!url) {
    url = (threaded ? import('./worker/decode-worker-mt?worker') : import('./worker/decode-worker?worker')).then((m) =>
      URL.createObjectURL(new Blob([m.default], { type: 'text/javascript' })),
    );
    url.catch(() => workerUrls.delete(threaded));
    workerUrls.set(threaded, url);
  }
  return url;
}

interface OpenedWorker {
  worker: Worker;
  name: string;
}

function startWorker(url: string, stream: StreamInfo, threads: number, alpha: boolean): Promise<OpenedWorker> {
  return new Promise((resolve, reject) => {
    const blocked = (detail: string) =>
      new AlphaVideoError('DECODER_UNSUPPORTED', `The decoder Worker could not start (${detail}); a Content-Security-Policy needs worker-src blob:`);
    let worker: Worker;
    try {
      worker = new Worker(url, { type: 'module', name: 'alpha-video-decoder' });
    } catch (e) {
      reject(blocked(String((e as Error)?.message ?? e)));
      return;
    }
    worker.onerror = (e) => {
      e.preventDefault();
      worker.terminate();
      reject(blocked(e.message || 'script error'));
    };
    worker.onmessage = (e: MessageEvent<FromWorker>) => {
      const message = e.data;
      if (message.t === 'opened') {
        worker.onerror = null;
        worker.onmessage = null;
        resolve({ worker, name: message.name });
      } else if (message.t === 'error') {
        worker.terminate();
        reject(new AlphaVideoError(message.code, message.message));
      }
    };
    const open: ToWorker = { t: 'open', codec: stream.codec === 'av1' ? 1 : 0, extradata: stream.codecPrivate, threads, alpha };
    worker.postMessage(open);
  });
}

async function startWorkers(info: DemuxInfo, threads: [number, number] | null): Promise<OpenedWorker[]> {
  const url = await workerUrl(threads !== null);
  const streams = [info.color, info.alpha!];
  const started = streams.map((stream, i) => startWorker(url, stream, threads?.[i] ?? 1, i === 1));
  const results = await Promise.allSettled(started);
  const failed = results.find((r): r is PromiseRejectedResult => r.status === 'rejected');
  if (failed) {
    for (const r of results) if (r.status === 'fulfilled') r.value.worker.terminate();
    throw failed.reason;
  }
  return results.map((r) => (r as PromiseFulfilledResult<OpenedWorker>).value);
}

export class SoftwareSource implements PairSource {
  readonly info: DemuxInfo;
  readonly decoderName: string;

  private readonly demuxer: WasmDemuxer;
  private readonly workers: [Worker, Worker];
  private readonly threads: number;
  private readonly callbacks: SourceCallbacks;
  private loop: boolean;
  private generation = 0;
  private queued: [number, number] = [0, 0];
  private needKey: [boolean, boolean] = [true, true];
  private outputs: [Decoded[], Decoded[]] = [[], []];
  private ready: FramePair[] = [];
  private inputDone = false;
  private epochUs = 0;
  private lastPtsUs = 0;
  private dropBeforeUs = -Infinity;
  private closed = false;
  private pumping = false;
  private pumpAgain = false;
  /** A seek for the feed loop to make before reading on */
  private seekTo: number | null = null;

  private constructor(demuxer: WasmDemuxer, opened: OpenedWorker[], threads: [number, number] | null, loop: boolean, callbacks: SourceCallbacks) {
    this.demuxer = demuxer;
    this.info = demuxer.info;
    this.workers = [opened[0].worker, opened[1].worker];
    this.threads = threads ? threads[0] + threads[1] : 0;
    threadsInUse += this.threads;
    this.loop = loop;
    this.callbacks = callbacks;
    const names = [...new Set(opened.map((o) => o.name))].join(', ');
    this.decoderName = threads ? `software (${names}, ${threads[0]}+${threads[1]} threads)` : `software (${names})`;
    this.workers.forEach((worker, stream) => {
      worker.onmessage = (e: MessageEvent<FromWorker>) => this.onMessage(stream as Stream, e.data);
      worker.onerror = (e) => {
        e.preventDefault();
        this.fail(new AlphaVideoError('DECODE_FAILED', `The ${stream ? 'alpha' : 'colour'} decoder Worker failed: ${e.message}`));
      };
    });
  }

  /**
   * Starts a decode Worker per stream of `demuxer`, which must have alpha, and owns the demuxer
   * once this resolves; it plays from the start. Threaded where the page allows it, else (or if
   * that fails) single-threaded.
   */
  static async create(demuxer: WasmDemuxer, loop: boolean, callbacks: SourceCallbacks): Promise<SoftwareSource> {
    if (!demuxer.info.alpha) throw new AlphaVideoError('UNSUPPORTED_FILE', 'The file has no alpha stream');
    let threads = threadCounts();
    let opened: OpenedWorker[];
    try {
      opened = await startWorkers(demuxer.info, threads);
    } catch (e) {
      if (!threads) throw e;
      console.warn(`alpha-video-web: the threaded decoders failed (${(e as Error)?.message ?? e}); decoding single-threaded`);
      threads = null;
      opened = await startWorkers(demuxer.info, null);
    }
    demuxer.seekToStart();
    return new SoftwareSource(demuxer, opened, threads, loop, callbacks);
  }

  yuvToRgb(colorRange: number, colorSpace: number): Float32Array {
    return this.demuxer.yuvToRgb(colorRange, colorSpace);
  }

  setLoop(loop: boolean): void {
    this.loop = loop;
  }

  pump(): void {
    if (this.closed) return;
    if (this.pumping) {
      this.pumpAgain = true;
      return;
    }
    this.pumping = true;
    void this.feedLoop().finally(() => {
      this.pumping = false;
      if (this.pumpAgain) this.pump();
    });
  }

  nextTimestampUs(): number | null {
    return this.ready.length ? this.ready[0].timestampUs : null;
  }

  take(timestampUs: number): FramePair | null {
    let due: FramePair | null = null;
    while (this.ready.length && this.ready[0].timestampUs <= timestampUs) {
      if (due) this.release(due);
      due = this.ready.shift()!;
    }
    if (due) this.pump();
    return due;
  }

  get finished(): boolean {
    return this.inputDone && !this.queued[0] && !this.queued[1] && !this.ready.length;
  }

  seek(fileTimeUs: number): number {
    this.discardDecoded();
    this.generation++;
    for (const worker of this.workers) this.post(worker, { t: 'flush', generation: this.generation });
    this.seekTo = fileTimeUs;
    this.demuxer.interrupt();
    this.dropBeforeUs = this.epochUs + fileTimeUs - 1000;
    this.pump();
    return this.epochUs + fileTimeUs;
  }

  release(pair: FramePair): void {
    this.recycle(0, pair.color as Planes);
    this.recycle(1, pair.alpha as Planes);
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    threadsInUse -= this.threads;
    for (const worker of this.workers) worker.terminate();
    this.outputs = [[], []];
    this.ready = [];
    this.demuxer.close();
  }

  /** Pairs decoded or on their way, to keep MAX_AHEAD of; frames a decoder holds back aren't counted */
  private ahead(): number {
    return this.ready.length + this.outputs[0].length + this.queued[0];
  }

  /** The only caller of the demuxer while playing, so its reads and seeks never overlap. */
  private async feedLoop(): Promise<void> {
    try {
      for (;;) {
        this.pumpAgain = false;
        if (this.closed) return;
        if (this.seekTo !== null) {
          const timeUs = this.seekTo;
          this.seekTo = null;
          await this.demuxer.seekToTimeUs(timeUs);
          continue;
        }
        if (this.inputDone || this.queued[0] >= MAX_QUEUED || this.queued[1] >= MAX_QUEUED || this.ahead() >= MAX_AHEAD) return;
        const packet = await this.demuxer.read();
        // Read before a seek that came while it was on its way
        if (this.closed || this.seekTo !== null) continue;
        if (packet) this.send(packet);
        else this.endOfFile();
      }
    } catch (e) {
      if (this.closed) return;
      if (isAbort(e) && this.seekTo !== null) {
        this.pumpAgain = true;
        return;
      }
      this.fail(e instanceof AlphaVideoError ? e : new AlphaVideoError('LOAD_FAILED', String((e as Error)?.message ?? e)));
    }
  }

  private send(packet: DemuxPacket): void {
    const stream = packet.stream;
    if (this.needKey[stream]) {
      if (!packet.key) return;
      this.needKey[stream] = false;
    }
    if (stream === 0) this.lastPtsUs = Math.max(this.lastPtsUs, packet.ptsUs);
    const data = packet.data.buffer as ArrayBuffer;
    this.queued[stream]++;
    this.post(this.workers[stream], { t: 'packet', data, timestampUs: this.epochUs + packet.ptsUs }, [data]);
  }

  private endOfFile(): void {
    // Each Worker drains its decoder and resets it to take the file again from a keyframe
    for (const stream of [0, 1] as const) {
      this.queued[stream]++;
      this.post(this.workers[stream], { t: 'drain' });
    }
    this.needKey = [true, true];
    if (!this.loop) {
      this.inputDone = true;
      return;
    }
    const frameUs = 1e6 / (this.info.frameRate || 30);
    this.epochUs += Math.max(this.info.durationUs, this.lastPtsUs + frameUs);
    this.lastPtsUs = 0;
    this.demuxer.seekToStart();
  }

  private onMessage(stream: Stream, message: FromWorker): void {
    if (this.closed) return;
    switch (message.t) {
      case 'frame': {
        const picture = toPlanes(message.frame, message.data);
        if (message.generation !== this.generation || picture.timestampUs < this.dropBeforeUs) {
          this.recycle(stream, picture.picture);
          return;
        }
        this.outputs[stream].push(picture);
        if (this.pairUp()) this.callbacks.onUpdate();
        break;
      }
      case 'done':
        this.queued[stream]--;
        this.pump();
        if (this.finished) this.callbacks.onUpdate();
        break;
      case 'error':
        this.fail(new AlphaVideoError(message.code, `${stream ? 'Alpha' : 'Colour'}: ${message.message}`));
        break;
    }
  }

  /** Pairs colour and alpha frames by timestamp; true if any became ready. */
  private pairUp(): boolean {
    const [colors, alphas] = this.outputs;
    let paired = false;
    while (colors.length && alphas.length) {
      if (colors[0].timestampUs !== alphas[0].timestampUs) {
        const stream: Stream = colors[0].timestampUs < alphas[0].timestampUs ? 0 : 1;
        this.recycle(stream, this.outputs[stream].shift()!.picture);
        continue;
      }
      const color = colors.shift()!;
      const alpha = alphas.shift()!;
      insertByTimestamp(this.ready, { timestampUs: color.timestampUs, color: color.picture, alpha: alpha.picture });
      paired = true;
    }
    return paired;
  }

  private discardDecoded(): void {
    this.outputs.forEach((queue, stream) => queue.forEach((d) => this.recycle(stream as Stream, d.picture)));
    this.outputs = [[], []];
    for (const pair of this.ready) this.release(pair);
    this.ready = [];
    this.needKey = [true, true];
    this.inputDone = false;
  }

  /** Hands a frame's buffer back to the Worker that filled it. */
  private recycle(stream: Stream, picture: Planes): void {
    const buffer = picture.data.buffer as ArrayBuffer;
    if (this.closed || !buffer.byteLength) return;
    this.post(this.workers[stream], { t: 'recycle', buffer }, [buffer]);
  }

  private post(worker: Worker, message: ToWorker, transfer: Transferable[] = []): void {
    worker.postMessage(message, transfer);
  }

  private fail(error: AlphaVideoError): void {
    if (this.closed) return;
    this.callbacks.onError(error);
  }
}

function toPlanes(frame: DecodedFrame, data: ArrayBuffer): Decoded {
  const chromaWidth = (frame.width + (1 << frame.chromaShiftX) - 1) >> frame.chromaShiftX;
  const chromaHeight = (frame.height + (1 << frame.chromaShiftY) - 1) >> frame.chromaShiftY;
  return {
    timestampUs: frame.timestampUs,
    picture: {
      kind: 'planes',
      width: frame.width,
      height: frame.height,
      chromaWidth,
      chromaHeight,
      bytesPerSample: frame.bytesPerSample === 2 ? 2 : 1,
      bitDepth: frame.bitDepth,
      interleavedChroma: false,
      data: new Uint8Array(data),
      layout: frame.layout,
      colorSpace: {
        fullRange: frame.colorRange === 2 ? true : frame.colorRange === 1 ? false : null,
        matrix: ((frame.rgb ? 'rgb' : MATRIX_NAMES[frame.colorSpace]) ?? null) as VideoMatrixCoefficients | null,
      },
    },
  };
}
