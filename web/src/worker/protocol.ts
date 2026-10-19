/** Messages between the software engine and a decode Worker, one Worker per stream. */

/** The parts of DedicatedWorkerGlobalScope the Worker uses (the WebWorker lib conflicts with DOM's) */
export interface WorkerScope {
  readonly name: string;
  readonly location: { readonly href: string };
  postMessage(message: FromWorker, transfer: Transferable[]): void;
  onmessage: ((event: MessageEvent<ToWorker>) => void) | null;
}

export type ToWorker =
  | {
      t: 'open';
      /** 0 VP9, 1 AV1 */
      codec: number;
      /** Matroska CodecPrivate, av1C for AV1 */
      extradata: Uint8Array | null;
      threads: number;
      /** Copy plane 0 only: alpha is the luma (or G) plane */
      alpha: boolean;
    }
  | { t: 'packet'; data: ArrayBuffer; timestampUs: number }
  /** The end of the stream: output what the decoder holds, then reset it for a keyframe */
  | { t: 'drain' }
  /** A seek: drop what the decoder holds; frames from here on carry `generation` */
  | { t: 'flush'; generation: number }
  /** A frame's buffer, back for reuse */
  | { t: 'recycle'; buffer: ArrayBuffer };

export interface PlaneLayout {
  offset: number;
  stride: number;
}

export interface DecodedFrame {
  width: number;
  height: number;
  bytesPerSample: number;
  bitDepth: number;
  chromaShiftX: number;
  chromaShiftY: number;
  rgb: boolean;
  /** AVColorRange */
  colorRange: number;
  /** AVColorSpace */
  colorSpace: number;
  timestampUs: number;
  layout: PlaneLayout[];
}

export type FromWorker =
  | { t: 'opened'; name: string }
  | { t: 'frame'; generation: number; frame: DecodedFrame; data: ArrayBuffer }
  /** A packet or drain was handled; its frames were posted before this */
  | { t: 'done' }
  | { t: 'error'; code: 'DECODER_UNSUPPORTED' | 'DECODE_FAILED'; message: string };
