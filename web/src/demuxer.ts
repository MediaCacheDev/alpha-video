/**
 * The C++ demuxer (cpp/AlphaDemuxer, the one Android and iOS use) and colour conversion
 * (cpp/YuvToRgb), compiled to WebAssembly. Loaded on first use only, so pages whose files
 * `<video>` plays never download it.
 *
 * The demuxer reads through a ByteReader (alpha-video-source), so a file streams in by range as
 * it plays and seeks fetch only what they need. WebAssembly can't wait for a fetch: when the
 * bytes it asks for aren't in memory it stops (ByteSource::kPending), this side fetches them,
 * and calls it again.
 */
import type { ByteReader } from 'alpha-video-source';
import { AlphaVideoError } from './errors';
import type { DemuxerModule } from './wasm/demuxer.mjs';

export type Codec = 'vp9' | 'av1';

export interface StreamInfo {
  codec: Codec;
  /** The bitstream's profile */
  profile: number;
  bitDepth: number;
  /** H.273 (and AVColorRange): 0 unspecified, 1 limited (MPEG), 2 full (JPEG) */
  colorRange: number;
  /** H.273 matrix (and AVColorSpace): 1 BT.709, 5 and 6 BT.601, 9 BT.2020, 2 unspecified */
  colorSpace: number;
  colorPrimaries: number;
  colorTrc: number;
  /** Matroska CodecPrivate: the av1C record for AV1, usually absent for VP9 */
  codecPrivate: Uint8Array | null;
}

export interface DemuxInfo {
  width: number;
  height: number;
  totalFrames: number;
  frameRate: number;
  durationUs: number;
  hasAlpha: boolean;
  /** Alpha is in each block's BlockAdditional, not a second track */
  alphaInBlockAdditional: boolean;
  color: StreamInfo;
  /** Present when hasAlpha */
  alpha: StreamInfo | null;
}

export interface DemuxPacket {
  /** 0 colour, 1 alpha */
  stream: 0 | 1;
  /** A copy, safe to keep */
  data: Uint8Array;
  ptsUs: number;
  frameIndex: number;
  key: boolean;
}

interface Module extends DemuxerModule {
  readSource?: (source: number, pos: number, out: number, size: number) => number;
}

let modulePromise: Promise<Module> | null = null;
const demuxers = new Map<number, WasmDemuxer>();
let nextSourceId = 1;

/** Loads the WebAssembly module once per page. */
export function loadDemuxerModule(): Promise<Module> {
  if (!modulePromise) {
    modulePromise = import('./wasm/demuxer.mjs').then((m) => m.default()).then((module: Module) => {
      module.readSource = (source, pos, out, size) => demuxers.get(source)?.copyHeld(pos, out, size) ?? -1;
      return module;
    });
    modulePromise.catch(() => {
      modulePromise = null;
    });
  }
  return modulePromise;
}

const INFO_NUMBERS = 24;
const PACKET_NUMBERS = 6;
// What the module's readSource answers for bytes not in memory yet (ByteSource::kPending)
const PENDING = -2;

const enum Opened {
  Done = 0,
  Pending = 1,
  Failed = 2,
}
const enum Read {
  End = 0,
  Packet = 1,
  Pending = 2,
}

/**
 * The C++ demuxer over a ByteReader, which it owns. One call at a time: await each read or seek
 * before the next, or interrupt() it. Call close() when done.
 */
export class WasmDemuxer {
  private readonly module: Module;
  private readonly reader: ByteReader;
  private readonly id = nextSourceId++;
  private handle: number;
  private readonly out: number;
  private infoValue: DemuxInfo | null = null;
  /** The range the demuxer last asked for and didn't find in memory */
  private wanted: { offset: number; length: number } | null = null;
  private filling: AbortController | null = null;

  private constructor(module: Module, reader: ByteReader) {
    this.module = module;
    this.reader = reader;
    demuxers.set(this.id, this);
    this.handle = module._avw_create(this.id, reader.size);
    this.out = module._malloc(INFO_NUMBERS * 8);
  }

  /**
   * Reads the headers through `reader`, which the demuxer then owns (and closes). Null, with
   * the reader closed, if it isn't a WebM with a VP9 or AV1 track. Rejects with LOAD_FAILED if
   * reading fails.
   */
  static async open(reader: ByteReader): Promise<WasmDemuxer | null> {
    let module: Module;
    try {
      module = await loadDemuxerModule();
    } catch (e) {
      reader.close();
      throw e;
    }
    const demuxer = new WasmDemuxer(module, reader);
    try {
      for (;;) {
        const opened: Opened = module._avw_try_open(demuxer.handle);
        if (opened === Opened.Done) break;
        if (opened === Opened.Failed) {
          demuxer.close();
          return null;
        }
        await demuxer.fill();
      }
    } catch (e) {
      demuxer.close();
      throw e;
    }
    demuxer.infoValue = demuxer.readInfo();
    return demuxer;
  }

  get info(): DemuxInfo {
    return this.infoValue!;
  }

  /** The next colour or alpha packet, or null at the end of the file. */
  async read(): Promise<DemuxPacket | null> {
    const m = this.module;
    for (;;) {
      if (!this.handle) throw abortError();
      const r: Read = m._avw_read(this.handle, this.out);
      if (r === Read.Packet) {
        const [stream, ptr, size, ptsUs, frameIndex, key] = m.HEAPF64.subarray(this.out / 8, this.out / 8 + PACKET_NUMBERS);
        return { stream: stream === 0 ? 0 : 1, data: m.HEAPU8.slice(ptr, ptr + size), ptsUs, frameIndex, key: key !== 0 };
      }
      if (r === Read.End) {
        if (m._avw_failed(this.handle)) throw new AlphaVideoError('LOAD_FAILED', 'Reading the file failed');
        return null;
      }
      await this.fill();
    }
  }

  /** Back to the first packet, with frameIndex counting from 0 again. Reads nothing yet. */
  seekToStart(): void {
    if (this.handle) this.module._avw_seek_start(this.handle);
  }

  /** To the keyframe at or before `timeUs`, reading the Cues or scanning for it as needed. */
  async seekToTimeUs(timeUs: number): Promise<void> {
    for (;;) {
      if (!this.handle) throw abortError();
      if (!this.module._avw_seek_us(this.handle, timeUs)) return;
      await this.fill();
    }
  }

  /**
   * Makes the read or seek in progress reject with an AbortError now, rather than once its
   * bytes arrive, and stops fetching them. The demuxer stays usable: seek it next.
   */
  interrupt(): void {
    this.filling?.abort();
  }

  /** yuvToRgbMatrix, without waiting: the module is loaded. */
  yuvToRgb(colorRange: number, colorSpace: number): Float32Array {
    return matrixFrom(this.module, colorRange, colorSpace, this.info.width, this.info.height);
  }

  close(): void {
    if (!this.handle) return;
    this.filling?.abort();
    this.module._avw_close(this.handle);
    this.module._free(this.out);
    this.handle = 0;
    demuxers.delete(this.id);
    this.reader.close();
  }

  /** The module's readSource: copies bytes held in memory to `out`, or answers PENDING. */
  copyHeld(offset: number, out: number, length: number): number {
    let bytes: Uint8Array | null;
    try {
      bytes = this.reader.peek(offset, length);
    } catch {
      return -1;
    }
    if (!bytes) {
      this.wanted = { offset, length };
      return PENDING;
    }
    this.module.HEAPU8.set(bytes, out);
    return bytes.byteLength;
  }

  /** Fetches the range the demuxer stopped for, all of it in one go. */
  private async fill(): Promise<void> {
    const wanted = this.wanted;
    this.wanted = null;
    if (!wanted) throw new AlphaVideoError('LOAD_FAILED', 'The demuxer waited for bytes without asking for any');
    const controller = new AbortController();
    this.filling = controller;
    try {
      await this.reader.read(wanted.offset, wanted.length, controller.signal);
    } catch (e) {
      if (isAbort(e) || e instanceof AlphaVideoError) throw e;
      throw new AlphaVideoError('LOAD_FAILED', `Reading the file failed: ${(e as Error)?.message ?? e}`);
    } finally {
      if (this.filling === controller) this.filling = null;
    }
    if (!this.handle) throw abortError();
  }

  private readInfo(): DemuxInfo {
    this.module._avw_info(this.handle, this.out);
    const n = this.module.HEAPF64.slice(this.out / 8, this.out / 8 + INFO_NUMBERS);
    const hasAlpha = n[5] !== 0;
    return {
      width: n[0],
      height: n[1],
      totalFrames: n[2],
      frameRate: n[3],
      durationUs: n[4],
      hasAlpha,
      alphaInBlockAdditional: n[6] !== 0,
      color: this.streamInfo(n, 0),
      alpha: hasAlpha ? this.streamInfo(n, 1) : null,
    };
  }

  private streamInfo(n: Float64Array, stream: 0 | 1): StreamInfo {
    const o = 8 + stream * 8;
    const privateSize = n[o + 7];
    let codecPrivate: Uint8Array | null = null;
    if (privateSize > 0) {
      const ptr = this.module._avw_codec_private(this.handle, stream);
      codecPrivate = this.module.HEAPU8.slice(ptr, ptr + privateSize);
    }
    return {
      codec: n[o] === 1 ? 'av1' : 'vp9',
      profile: n[o + 1],
      bitDepth: n[o + 2],
      colorRange: n[o + 3],
      colorSpace: n[o + 4],
      colorPrimaries: n[o + 5],
      colorTrc: n[o + 6],
      codecPrivate,
    };
  }
}

export function isAbort(error: unknown): boolean {
  return (error as { name?: string } | null)?.name === 'AbortError';
}

function abortError(): Error {
  if (typeof DOMException === 'function') return new DOMException('The demuxer was closed or interrupted', 'AbortError');
  return Object.assign(new Error('The demuxer was closed or interrupted'), { name: 'AbortError' });
}

/**
 * The YUV to RGB matrix the native players use (cpp/YuvToRgb): 16 floats, a column-major mat4
 * for (Y, U, V, 1). An unspecified range is limited; an unspecified matrix is BT.601 up to
 * 720×576 and BT.709 above.
 */
export async function yuvToRgbMatrix(colorRange: number, colorSpace: number, width: number, height: number): Promise<Float32Array> {
  return matrixFrom(await loadDemuxerModule(), colorRange, colorSpace, width, height);
}

function matrixFrom(m: DemuxerModule, colorRange: number, colorSpace: number, width: number, height: number): Float32Array {
  const out = m._malloc(64);
  m._avw_yuv_to_rgb(colorRange, colorSpace, width, height, out);
  const matrix = m.HEAPF32.slice(out / 4, out / 4 + 16);
  m._free(out);
  return matrix;
}
