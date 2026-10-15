/** What the canvas engine plays: decoded colour and alpha pairs, from WebCodecs or the software decoders. */
import type { DemuxInfo } from './demuxer';
import type { AlphaVideoError } from './errors';

/** A decoded picture as YUV (or G, B, R) planes in memory. */
export interface Planes {
  kind: 'planes';
  width: number;
  height: number;
  chromaWidth: number;
  chromaHeight: number;
  bytesPerSample: 1 | 2;
  bitDepth: number;
  /** NV12: U and V interleaved in plane 1 */
  interleavedChroma: boolean;
  data: Uint8Array;
  /** Plane 0 only, for alpha */
  layout: PlaneLayout[];
  colorSpace: VideoColorSpaceInit;
}

/** A VideoFrame in a format without planes to copy (Firefox outputs BGRX), drawn by the browser. */
export interface FramePicture {
  kind: 'frame';
  frame: VideoFrame;
  colorSpace: VideoColorSpaceInit;
}

export type Picture = Planes | FramePicture;

export interface FramePair {
  /** On a timeline that keeps counting up across loops */
  timestampUs: number;
  color: Picture;
  alpha: Picture;
}

export interface SourceCallbacks {
  /** A pair became ready, or the end of the file was reached */
  onUpdate(): void;
  onError(error: AlphaVideoError): void;
}

export interface PairSource {
  readonly info: DemuxInfo;
  /** For onLoad: the codec strings or decoders in use */
  readonly decoderName: string;
  /** The core's YUV to RGB matrix for this file's size (cpp/YuvToRgb) */
  yuvToRgb(colorRange: number, colorSpace: number): Float32Array;
  setLoop(loop: boolean): void;
  /** Decodes ahead until enough is ready. Safe to call any time. */
  pump(): void;
  /** The earliest ready pair's timestamp, or null if none is decoded yet */
  nextTimestampUs(): number | null;
  /** The latest ready pair at or before `timestampUs`, now the caller's to release; earlier ones are released */
  take(timestampUs: number): FramePair | null;
  /** Every frame has been taken and none will follow */
  readonly finished: boolean;
  /** Restarts at the keyframe before `fileTimeUs`, dropping frames before it; returns its timeline timestamp */
  seek(fileTimeUs: number): number;
  release(pair: FramePair): void;
  close(): void;
}

const POOL_PER_SIZE = 8;

/** Reuses frame buffers by size, so playback doesn't allocate a buffer per frame. */
export class BufferPool {
  private readonly free = new Map<number, Uint8Array[]>();

  take(size: number): Uint8Array {
    return this.free.get(size)?.pop() ?? new Uint8Array(size);
  }

  releasePicture(p: Picture): void {
    if (p.kind === 'frame') {
      p.frame.close();
      return;
    }
    const list = this.free.get(p.data.length) ?? [];
    if (list.length < POOL_PER_SIZE) list.push(p.data);
    this.free.set(p.data.length, list);
  }

  releasePair(pair: FramePair): void {
    this.releasePicture(pair.color);
    this.releasePicture(pair.alpha);
  }
}

/** Inserts `pair` into `ready`, which is in timestamp order. */
export function insertByTimestamp(ready: FramePair[], pair: FramePair): void {
  let i = ready.length;
  while (i > 0 && ready[i - 1].timestampUs > pair.timestampUs) i--;
  ready.splice(i, 0, pair);
}
