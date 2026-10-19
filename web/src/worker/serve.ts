/**
 * The decode Worker: one decoder (FFmpeg's for VP9, dav1d for AV1) for one stream. Packets come
 * in, frames go out as transferred buffers, which come back for reuse once drawn. Decoding blocks
 * this Worker only, never the page.
 */
import type { DecoderModule } from '../wasm/decoder-module.mjs';
import type { DecodedFrame, FromWorker, PlaneLayout, ToWorker, WorkerScope } from './protocol';

const FRAME_NUMBERS = 16;
const POOL_PER_SIZE = 8;

export function serve(load: (threads: number) => Promise<DecoderModule>): void {
  const scope = self as unknown as WorkerScope;
  let m: DecoderModule;
  let decoder = 0;
  let out = 0;
  let input = 0;
  let inputSize = 0;
  let planes = 3;
  let generation = 0;
  const free = new Map<number, ArrayBuffer[]>();

  const post = (message: FromWorker, transfer: Transferable[] = []) => scope.postMessage(message, transfer);

  // Fresh views each time: memory can grow, from this thread or a decoder thread
  const heapU8 = () => new Uint8Array(m.wasmMemory.buffer);
  const heapF64 = () => new Float64Array(m.wasmMemory.buffer);

  const open = async (message: Extract<ToWorker, { t: 'open' }>) => {
    m = await load(message.threads);
    planes = message.alpha ? 1 : 3;
    out = m._malloc(FRAME_NUMBERS * 8);
    const { extradata } = message;
    const ptr = extradata ? m._malloc(extradata.length) : 0;
    if (extradata) heapU8().set(extradata, ptr);
    decoder = m._avw_decoder_open(message.codec, ptr, extradata?.length ?? 0, message.threads);
    if (ptr) m._free(ptr);
    if (!decoder) {
      const codec = ['VP9', 'AV1', 'VP8'][message.codec];
      post({ t: 'error', code: 'DECODER_UNSUPPORTED', message: `The software ${codec} decoder could not open the stream` });
      return;
    }
    const heap = heapU8();
    let name = '';
    for (let i = m._avw_decoder_name(decoder); heap[i]; i++) name += String.fromCharCode(heap[i]);
    post({ t: 'opened', name });
  };

  const send = (data: ArrayBuffer | null, timestampUs: number) => {
    let ptr = 0;
    let size = 0;
    if (data) {
      size = data.byteLength;
      if (size > inputSize) {
        if (input) m._free(input);
        inputSize = Math.max(size, inputSize * 2, 65536);
        input = m._malloc(inputSize);
      }
      ptr = input;
      heapU8().set(new Uint8Array(data), ptr);
    }
    let r = m._avw_decoder_send(decoder, ptr, size, timestampUs);
    if (r === 0 && ptr) {
      receive();
      r = m._avw_decoder_send(decoder, ptr, size, timestampUs);
    }
    if (r < 0) throw new Error(`decoder error ${r}`);
    receive();
  };

  const receive = () => {
    for (;;) {
      const r = m._avw_decoder_receive(decoder, out);
      if (r === 0 || r === -1) return;
      if (r < 0) throw new Error(`decoder error ${r}`);
      postFrame();
    }
  };

  /** Copies the frame avw_decoder_receive described at `out` out of WebAssembly memory. */
  const postFrame = () => {
    const n = heapF64().slice(out / 8, out / 8 + FRAME_NUMBERS);
    const [width, height, bytesPerSample, bitDepth, chromaShiftX, chromaShiftY, rgb, colorRange, colorSpace, timestampUs] = n;
    const chromaHeight = (height + (1 << chromaShiftY) - 1) >> chromaShiftY;
    const layout: PlaneLayout[] = [];
    let total = 0;
    for (let i = 0; i < planes; i++) {
      layout.push({ offset: total, stride: n[13 + i] });
      total += n[13 + i] * (i === 0 ? height : chromaHeight);
    }
    const buffer = free.get(total)?.pop() ?? new ArrayBuffer(total);
    const bytes = new Uint8Array(buffer);
    const heap = heapU8();
    for (let i = 0; i < planes; i++) {
      const ptr = n[10 + i];
      bytes.set(heap.subarray(ptr, ptr + n[13 + i] * (i === 0 ? height : chromaHeight)), layout[i].offset);
    }
    const frame: DecodedFrame = {
      width,
      height,
      bytesPerSample,
      bitDepth,
      chromaShiftX,
      chromaShiftY,
      rgb: rgb !== 0,
      colorRange,
      colorSpace,
      timestampUs,
      layout,
    };
    post({ t: 'frame', generation, frame, data: buffer }, [buffer]);
  };

  const handle = (message: ToWorker) => {
    switch (message.t) {
      case 'packet':
        send(message.data, message.timestampUs);
        post({ t: 'done' });
        break;
      case 'drain':
        send(null, 0);
        m._avw_decoder_flush(decoder);
        post({ t: 'done' });
        break;
      case 'flush':
        generation = message.generation;
        m._avw_decoder_flush(decoder);
        break;
      case 'recycle': {
        const list = free.get(message.buffer.byteLength) ?? [];
        if (list.length < POOL_PER_SIZE) list.push(message.buffer);
        free.set(message.buffer.byteLength, list);
        break;
      }
    }
  };

  scope.onmessage = (e) => {
    const message = e.data;
    if (message.t === 'open') {
      open(message).catch((error: unknown) => post({ t: 'error', code: 'DECODER_UNSUPPORTED', message: String((error as Error)?.message ?? error) }));
      return;
    }
    try {
      handle(message);
    } catch (error) {
      post({ t: 'error', code: 'DECODE_FAILED', message: String((error as Error)?.message ?? error) });
    }
  };
}
