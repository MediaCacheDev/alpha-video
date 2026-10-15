/**
 * The C++ demuxer (cpp/AlphaDemuxer, the one MediaCodec uses on Android) and colour conversion
 * (cpp/YuvToRgb), compiled to WebAssembly. Loaded on first use only, so pages whose files
 * `<video>` plays never download it.
 */
import type { DemuxerModule } from './wasm/demuxer.mjs';

export type Codec = 'vp9' | 'av1';

export interface StreamInfo {
  codec: Codec;
  /** FFmpeg's profile for the track */
  profile: number;
  bitDepth: number;
  /** AVColorRange: 0 unspecified, 1 limited (MPEG), 2 full (JPEG) */
  colorRange: number;
  /** AVColorSpace, the YUV matrix: 1 BT.709, 5 and 6 BT.601, 9 BT.2020, 2 unspecified */
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

let modulePromise: Promise<DemuxerModule> | null = null;

/** Loads the WebAssembly module once per page. */
export function loadDemuxerModule(): Promise<DemuxerModule> {
  if (!modulePromise) {
    modulePromise = import('./wasm/demuxer.mjs').then((m) => m.default());
    modulePromise.catch(() => {
      modulePromise = null;
    });
  }
  return modulePromise;
}

const INFO_NUMBERS = 24;
const PACKET_NUMBERS = 6;

/** One file held in WebAssembly memory and the C++ demuxer reading it. Call close() when done. */
export class WasmDemuxer {
  readonly info: DemuxInfo;
  private readonly module: DemuxerModule;
  private handle: number;
  private fileData: number;
  private readonly out: number;

  private constructor(module: DemuxerModule, handle: number, fileData: number) {
    this.module = module;
    this.handle = handle;
    this.fileData = fileData;
    this.out = module._malloc(INFO_NUMBERS * 8);
    module._avw_info(handle, this.out);
    const n = module.HEAPF64.slice(this.out / 8, this.out / 8 + INFO_NUMBERS);
    const hasAlpha = n[5] !== 0;
    this.info = {
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

  /**
   * Copies `bytes`, a whole WebM, into WebAssembly memory: the demuxer module's, or `module`'s
   * (the software decoders' module has the demuxer too). Returns null if it isn't a WebM with a
   * VP9 or AV1 track.
   */
  static async open(bytes: Uint8Array, module?: DemuxerModule): Promise<WasmDemuxer | null> {
    module ??= await loadDemuxerModule();
    const data = module._malloc(bytes.length);
    if (!data) return null;
    module.HEAPU8.set(bytes, data);
    const handle = module._avw_open(data, bytes.length);
    if (!handle) {
      module._free(data);
      return null;
    }
    return new WasmDemuxer(module, handle, data);
  }

  /** The next colour or alpha packet, or null at the end of the file. */
  read(): DemuxPacket | null {
    const p = this.readInPlace();
    if (!p) return null;
    const { ptr, size, ...packet } = p;
    return { ...packet, data: this.module.HEAPU8.slice(ptr, ptr + size) };
  }

  /** read(), with the data left in WebAssembly memory at `ptr` until the next read, seek or close. */
  readInPlace(): (Omit<DemuxPacket, 'data'> & { ptr: number; size: number }) | null {
    const m = this.module;
    if (!this.handle || !m._avw_read(this.handle, this.out)) return null;
    const [stream, ptr, size, ptsUs, frameIndex, key] = m.HEAPF64.subarray(this.out / 8, this.out / 8 + PACKET_NUMBERS);
    return { stream: stream === 0 ? 0 : 1, ptr, size, ptsUs, frameIndex, key: key !== 0 };
  }

  seekToStart(): void {
    if (this.handle) this.module._avw_seek_start(this.handle);
  }

  /** To the keyframe at or before `timeUs`. */
  seekToTimeUs(timeUs: number): void {
    if (this.handle) this.module._avw_seek_us(this.handle, timeUs);
  }

  /** yuvToRgbMatrix, without waiting: the module is loaded. */
  yuvToRgb(colorRange: number, colorSpace: number): Float32Array {
    return matrixFrom(this.module, colorRange, colorSpace, this.info.width, this.info.height);
  }

  close(): void {
    if (!this.handle) return;
    this.module._avw_close(this.handle);
    this.module._free(this.fileData);
    this.module._free(this.out);
    this.handle = 0;
    this.fileData = 0;
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
