/**
 * Colour and alpha decoded by two WebCodecs VideoDecoders, fed by the C++ demuxer and paired by
 * timestamp: the web counterpart of the Android MediaCodec path.
 */
import { codecString } from './codec';
import { isAbort, type DemuxInfo, type DemuxPacket, type WasmDemuxer } from './demuxer';
import { AlphaVideoError } from './errors';
import { BufferPool, insertByTimestamp, type FramePair, type PairSource, type Picture, type SourceCallbacks } from './source';

type Stream = 0 | 1;

interface PendingPicture {
  timestampUs: number;
  picture: Promise<Picture>;
}

// Subsampling shifts and bytes per sample of the YUV formats copied to planes
const PLANAR: Record<string, { sx: number; sy: number; bytes: 1 | 2; depth?: number; nv12?: boolean }> = {
  I420: { sx: 1, sy: 1, bytes: 1 },
  I420A: { sx: 1, sy: 1, bytes: 1 },
  I420P10: { sx: 1, sy: 1, bytes: 2, depth: 10 },
  I420P12: { sx: 1, sy: 1, bytes: 2, depth: 12 },
  I420AP10: { sx: 1, sy: 1, bytes: 2, depth: 10 },
  I420AP12: { sx: 1, sy: 1, bytes: 2, depth: 12 },
  I422: { sx: 1, sy: 0, bytes: 1 },
  I422P10: { sx: 1, sy: 0, bytes: 2, depth: 10 },
  I422P12: { sx: 1, sy: 0, bytes: 2, depth: 12 },
  I444: { sx: 0, sy: 0, bytes: 1 },
  I444P10: { sx: 0, sy: 0, bytes: 2, depth: 10 },
  I444P12: { sx: 0, sy: 0, bytes: 2, depth: 12 },
  NV12: { sx: 1, sy: 1, bytes: 1, nv12: true },
};

const AVCOL_SPC_RGB = 0;

/** Decoded pairs (or frames still in the decoders) to stay ahead of the playhead */
const MAX_AHEAD = 6;
/** Chunks waiting in a decoder's queue before feeding stops until it takes them */
const MAX_DECODE_QUEUE = 3;

export class WebCodecsSource implements PairSource {
  readonly info: DemuxInfo;
  readonly decoderName: string;
  /** The demuxer has been read to its end and won't loop */
  inputDone = false;
  /** inputDone, and every frame has come out of the decoders */
  drained = false;

  private readonly demuxer: WasmDemuxer;
  private readonly configs: [VideoDecoderConfig, VideoDecoderConfig];
  private readonly callbacks: SourceCallbacks;
  private loop: boolean;
  private decoders: [VideoDecoder, VideoDecoder];
  private needKey: [boolean, boolean] = [true, true];
  private inDecoder: [number, number] = [0, 0];
  private outputs: [PendingPicture[], PendingPicture[]] = [[], []];
  private converting = 0;
  private ready: FramePair[] = [];
  private epochUs = 0;
  private lastPtsUs = 0;
  private dropBeforeUs = -Infinity;
  private flushes = 0;
  private closed = false;
  private readonly pool = new BufferPool();
  private pumping = false;
  private pumpAgain = false;
  /** A seek for the feed loop to make before reading on */
  private seekTo: number | null = null;

  private constructor(
    demuxer: WasmDemuxer,
    configs: [VideoDecoderConfig, VideoDecoderConfig],
    loop: boolean,
    callbacks: SourceCallbacks,
  ) {
    this.demuxer = demuxer;
    this.info = demuxer.info;
    this.configs = configs;
    const [color, alpha] = [configs[0].codec, configs[1].codec];
    this.decoderName = `webcodecs (${color === alpha ? color : `${color}, ${alpha}`})`;
    this.loop = loop;
    this.callbacks = callbacks;
    this.decoders = [this.createDecoder(0), this.createDecoder(1)];
  }

  /**
   * Configures a decoder for each stream of `demuxer`, which must have alpha, and owns it once
   * this resolves. Throws DECODER_UNSUPPORTED if this browser can't decode either one.
   */
  static async create(
    demuxer: WasmDemuxer,
    hardwareAcceleration: HardwareAcceleration,
    loop: boolean,
    callbacks: SourceCallbacks,
  ): Promise<WebCodecsSource> {
    const { info } = demuxer;
    if (!info.alpha) throw new AlphaVideoError('UNSUPPORTED_FILE', 'The file has no alpha stream');
    if (typeof VideoDecoder === 'undefined') {
      throw new AlphaVideoError('DECODER_UNSUPPORTED', 'WebCodecs is unavailable (it needs https or localhost)');
    }
    // The VP9 profile and bit depth are in the first keyframe, not the container
    const firstKey: (Uint8Array | null)[] = [null, null];
    while (!firstKey[0] || !firstKey[1]) {
      const p = await demuxer.read();
      if (!p) break;
      if (p.key && !firstKey[p.stream]) firstKey[p.stream] = p.data;
    }
    if (!firstKey[0] || !firstKey[1]) throw new AlphaVideoError('UNSUPPORTED_FILE', 'No keyframe in the colour or alpha stream');

    const config = (stream: Stream): VideoDecoderConfig => ({
      codec: codecString(stream === 0 ? info.color : info.alpha!, firstKey[stream]!, info.width, info.height),
      codedWidth: info.width,
      codedHeight: info.height,
      hardwareAcceleration,
      optimizeForLatency: true,
    });
    const configs: [VideoDecoderConfig, VideoDecoderConfig] = [config(0), config(1)];
    for (const c of configs) {
      const support = await VideoDecoder.isConfigSupported(c).catch(() => ({ supported: false }));
      if (!support.supported) throw new AlphaVideoError('DECODER_UNSUPPORTED', `This browser can't decode ${c.codec}`);
    }
    // Alpha coded as RGB (VP9 profile 1, gbrp) is only right from its planes: Firefox, which
    // outputs BGRX, converts it as if it were YUV
    if (info.alpha.colorSpace === AVCOL_SPC_RGB) {
      const format = await firstFrameFormat(configs[1], firstKey[1]!);
      if (!format || !PLANAR[format]) {
        throw new AlphaVideoError('DECODER_UNSUPPORTED', `This browser's ${configs[1].codec} decoder outputs RGB alpha as ${format ?? 'nothing'}`);
      }
    }
    demuxer.seekToStart();
    return new WebCodecsSource(demuxer, configs, loop, callbacks);
  }

  yuvToRgb(colorRange: number, colorSpace: number): Float32Array {
    return this.demuxer.yuvToRgb(colorRange, colorSpace);
  }

  setLoop(loop: boolean): void {
    this.loop = loop;
  }

  /** Feeds the decoders until enough is decoded or queued. Safe to call any time. */
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

  /** The earliest ready pair's timestamp, or null if none is decoded yet. */
  nextTimestampUs(): number | null {
    return this.ready.length ? this.ready[0].timestampUs : null;
  }

  /**
   * The latest ready pair at or before `timestampUs`, which the caller now owns and must
   * close; earlier ones are closed. Null if none is due.
   */
  take(timestampUs: number): FramePair | null {
    let due: FramePair | null = null;
    while (this.ready.length && this.ready[0].timestampUs <= timestampUs) {
      if (due) this.pool.releasePair(due);
      due = this.ready.shift()!;
    }
    if (due) this.pump();
    return due;
  }

  /** Whether every frame has been taken and none will follow. */
  get finished(): boolean {
    return this.drained && !this.ready.length && !this.converting && !this.outputs[0].length;
  }

  /**
   * Restarts decoding at the keyframe before `fileTimeUs`, dropping frames before it. Returns
   * the timeline timestamp of `fileTimeUs`.
   */
  seek(fileTimeUs: number): number {
    this.discardDecoding();
    this.seekTo = fileTimeUs;
    this.demuxer.interrupt();
    this.dropBeforeUs = this.epochUs + fileTimeUs - 1000;
    this.pump();
    return this.epochUs + fileTimeUs;
  }

  /** Returns a pair taken with take() to its buffers. */
  release(pair: FramePair): void {
    this.pool.releasePair(pair);
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.discardDecoding();
    for (const d of this.decoders) if (d.state !== 'closed') d.close();
    this.demuxer.close();
  }

  private createDecoder(stream: Stream): VideoDecoder {
    const decoder: VideoDecoder = new VideoDecoder({
      output: (frame) => this.onOutput(stream, frame, decoder),
      error: (e) => {
        if (decoder !== this.decoders[stream] || this.closed) return;
        this.callbacks.onError(new AlphaVideoError('DECODE_FAILED', `${stream ? 'Alpha' : 'Colour'} decoder: ${e.message}`));
      },
    });
    decoder.configure(this.configs[stream]);
    decoder.addEventListener('dequeue', () => this.pump());
    return decoder;
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
        if (this.inputDone || !this.wantsInput()) return;
        const packet = await this.demuxer.read();
        // Read before a seek that came while it was on its way
        if (this.closed || this.seekTo !== null) continue;
        if (packet) this.feed(packet);
        else this.endOfFile();
      }
    } catch (e) {
      if (this.closed) return;
      if (isAbort(e) && this.seekTo !== null) {
        this.pumpAgain = true;
        return;
      }
      this.callbacks.onError(e instanceof AlphaVideoError ? e : new AlphaVideoError('LOAD_FAILED', String((e as Error)?.message ?? e)));
    }
  }

  private wantsInput(): boolean {
    const [color, alpha] = this.decoders;
    const buffered = this.ready.length + this.converting;
    if (buffered > 0 && buffered + this.inDecoder[0] >= MAX_AHEAD) return false;
    return color.decodeQueueSize < MAX_DECODE_QUEUE && alpha.decodeQueueSize < MAX_DECODE_QUEUE;
  }

  private feed(packet: DemuxPacket): void {
    const stream = packet.stream;
    if (this.needKey[stream]) {
      if (!packet.key) return;
      this.needKey[stream] = false;
    }
    if (stream === 0) this.lastPtsUs = Math.max(this.lastPtsUs, packet.ptsUs);
    const chunk = new EncodedVideoChunk({
      type: packet.key ? 'key' : 'delta',
      timestamp: this.epochUs + packet.ptsUs,
      data: packet.data,
    });
    this.inDecoder[stream]++;
    this.decoders[stream].decode(chunk);
  }

  private endOfFile(): void {
    // A flush outputs the frames still in the decoders, and makes them want a keyframe next
    const flushes = ++this.flushes;
    const flushed = Promise.all(this.decoders.map((d) => d.flush())).catch(() => {});
    this.needKey = [true, true];
    if (this.loop) {
      const frameUs = 1e6 / (this.info.frameRate || 30);
      this.epochUs += Math.max(this.info.durationUs, this.lastPtsUs + frameUs);
      this.lastPtsUs = 0;
      this.demuxer.seekToStart();
      return;
    }
    this.inputDone = true;
    flushed.then(() => {
      if (this.closed || flushes !== this.flushes) return;
      this.drained = true;
      this.callbacks.onUpdate();
    });
  }

  private onOutput(stream: Stream, frame: VideoFrame, decoder: VideoDecoder): void {
    if (this.closed || decoder !== this.decoders[stream]) {
      frame.close();
      return;
    }
    this.inDecoder[stream] = Math.max(0, this.inDecoder[stream] - 1);
    if (frame.timestamp < this.dropBeforeUs) {
      frame.close();
      this.pump();
      return;
    }
    this.outputs[stream].push({ timestampUs: frame.timestamp, picture: this.toPicture(frame) });
    this.pairUp();
  }

  private pairUp(): void {
    const [colors, alphas] = this.outputs;
    while (colors.length && alphas.length) {
      const c = colors[0];
      const a = alphas[0];
      if (c.timestampUs !== a.timestampUs) {
        // A frame whose partner never came (a decoder dropped it): discard the earlier one
        const lonely = c.timestampUs < a.timestampUs ? colors.shift()! : alphas.shift()!;
        lonely.picture.then((p) => this.pool.releasePicture(p));
        continue;
      }
      colors.shift();
      alphas.shift();
      const decoders = this.decoders;
      Promise.all([c.picture, a.picture]).then(
        ([color, alpha]) => {
          const pair = { timestampUs: c.timestampUs, color, alpha };
          if (this.closed || decoders !== this.decoders) {
            this.pool.releasePair(pair);
            return;
          }
          insertByTimestamp(this.ready, pair);
          this.callbacks.onUpdate();
        },
        (e: Error) => {
          if (!this.closed) this.callbacks.onError(new AlphaVideoError('DECODE_FAILED', `Reading a frame: ${e.message}`));
        },
      );
    }
  }

  private async toPicture(frame: VideoFrame): Promise<Picture> {
    const colorSpace = frame.colorSpace.toJSON();
    const f = frame.format ? PLANAR[frame.format] : undefined;
    if (!f) return { kind: 'frame', frame, colorSpace };
    this.converting++;
    try {
      const data = this.pool.take(frame.allocationSize());
      const layout = await frame.copyTo(data);
      const width = frame.visibleRect?.width ?? frame.codedWidth;
      const height = frame.visibleRect?.height ?? frame.codedHeight;
      return {
        kind: 'planes',
        width,
        height,
        chromaWidth: (width + f.sx) >> f.sx,
        chromaHeight: (height + f.sy) >> f.sy,
        bytesPerSample: f.bytes,
        bitDepth: f.depth ?? 8,
        interleavedChroma: !!f.nv12,
        data,
        layout,
        colorSpace,
      };
    } finally {
      frame.close();
      this.converting--;
      this.pump();
    }
  }

  /** New decoders, nothing queued: for a seek or close. */
  private discardDecoding(): void {
    this.flushes++;
    for (const d of this.decoders) if (d.state !== 'closed') d.close();
    for (const queue of this.outputs) {
      for (const p of queue) p.picture.then((pic) => this.pool.releasePicture(pic));
      queue.length = 0;
    }
    for (const pair of this.ready) this.pool.releasePair(pair);
    this.ready = [];
    this.inDecoder = [0, 0];
    this.needKey = [true, true];
    this.inputDone = false;
    this.drained = false;
    if (!this.closed) this.decoders = [this.createDecoder(0), this.createDecoder(1)];
  }
}

/** The format of the frame a decoder outputs for one keyframe, or null if it outputs none. */
function firstFrameFormat(config: VideoDecoderConfig, key: Uint8Array): Promise<VideoPixelFormat | null> {
  return new Promise((resolve) => {
    const decoder = new VideoDecoder({
      output: (frame) => {
        resolve(frame.format);
        frame.close();
      },
      error: () => resolve(null),
    });
    decoder.configure(config);
    decoder.decode(new EncodedVideoChunk({ type: 'key', timestamp: 0, data: key }));
    decoder
      .flush()
      .catch(() => {})
      .finally(() => {
        resolve(null);
        if (decoder.state !== 'closed') decoder.close();
      });
  });
}
