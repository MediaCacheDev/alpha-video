import { memorySource, openReader, type ByteReader, type SourceInfo } from 'range-source-web';
import { afterAll, describe, expect, it } from 'vitest';
import { WasmDemuxer, yuvToRgbMatrix, type DemuxPacket } from '../src/demuxer';
import { AlphaVideoError } from '../src/errors';
import { sniffWebM } from '../src/sniff';
import { cards } from './fixtures';

const JPEG = 2;

/**
 * A reader whose bytes arrive a block at a time, however much the demuxer asks for, so it stops
 * for them again and again: mid-header, mid-frame, in the Cues. Fills can be held back.
 */
class BlockAtATime implements ByteReader {
  static readonly BLOCK = 997;
  readonly info: SourceInfo;
  fills = 0;
  failWith: Error | null = null;
  closed = false;
  private readonly loaded = new Set<number>();
  private held: Array<() => void> | null = null;

  constructor(private readonly data: Uint8Array) {
    this.info = { size: data.byteLength };
  }

  get size(): number {
    return this.data.byteLength;
  }

  peek(offset: number, length: number): Uint8Array | null {
    if (offset >= this.size) return new Uint8Array(0);
    const block = Math.floor(offset / BlockAtATime.BLOCK);
    if (!this.loaded.has(block)) return null;
    return this.data.subarray(offset, Math.min(offset + length, (block + 1) * BlockAtATime.BLOCK, this.size));
  }

  async read(offset: number, length: number, signal?: AbortSignal): Promise<Uint8Array> {
    this.fills++;
    if (this.held) {
      await new Promise<void>((resolve, reject) => {
        this.held!.push(resolve);
        signal?.addEventListener('abort', () => reject(Object.assign(new Error('aborted'), { name: 'AbortError' })));
      });
    }
    if (this.failWith) throw this.failWith;
    this.loaded.add(Math.floor(offset / BlockAtATime.BLOCK));
    return this.data.subarray(offset, Math.min(offset + length, this.size));
  }

  prefetch(): void {}

  close(): void {
    this.closed = true;
  }

  hold(): void {
    this.held = [];
  }

  release(): void {
    const waiting = this.held ?? [];
    this.held = null;
    waiting.forEach((resolve) => resolve());
  }
}

const identity = (p: DemuxPacket) => [p.stream, p.ptsUs, p.key, p.frameIndex, p.data.length, p.data.reduce((h, b) => (h * 31 + b) >>> 0, 7)];

async function readAll(d: WasmDemuxer): Promise<DemuxPacket[]> {
  const packets: DemuxPacket[] = [];
  for (let p = await d.read(); p; p = await d.read()) packets.push(p);
  return packets;
}

describe.each(cards())('$name', (fx) => {
  const opened: WasmDemuxer[] = [];
  afterAll(() => opened.forEach((d) => d.close()));

  async function open(reader?: ByteReader): Promise<WasmDemuxer> {
    const d = await WasmDemuxer.open(reader ?? (await openReader(memorySource(fx.bytes))));
    expect(d).not.toBeNull();
    opened.push(d!);
    return d!;
  }

  it('sniffs the layout from the first 64 KB', () => {
    const s = sniffWebM(fx.bytes.subarray(0, 65536));
    expect(s).toEqual({
      layout: fx.blockAdditional ? 'blockadditional' : 'dual',
      codec: fx.codec,
      width: fx.width,
      height: fx.height,
    });
  });

  it('reads the layout', async () => {
    const { info } = await open();
    expect(info.width).toBe(fx.width);
    expect(info.height).toBe(fx.height);
    expect(info.totalFrames).toBeGreaterThan(0);
    expect(info.hasAlpha).toBe(true);
    expect(info.alphaInBlockAdditional).toBe(fx.blockAdditional);
    expect(info.color.codec).toBe(fx.codec);
    expect(info.alpha!.codec).toBe(fx.codec);
    expect(info.color.bitDepth).toBe(fx.bitDepth);
    if (fx.blockAdditional) expect(info.alpha!.colorRange).toBe(JPEG);
    if (fx.codec === 'av1') {
      expect(info.color.codecPrivate?.length).toBeGreaterThanOrEqual(4);
      expect(info.alpha!.codecPrivate?.length).toBeGreaterThanOrEqual(4);
    }
  });

  it('reads every packet of both streams, keyframes aligned', async () => {
    const d = await open();
    const packets = await readAll(d);
    expect(packets[0]).toMatchObject({ stream: 0, key: true });
    for (const stream of [0, 1] as const) {
      const own = packets.filter((p) => p.stream === stream);
      expect(own.length).toBe(d.info.totalFrames);
      own.forEach((p, i) => {
        expect(p.data.length).toBeGreaterThan(0);
        expect(p.frameIndex).toBe(i);
      });
    }
    const keyPts = (stream: number) => packets.filter((p) => p.stream === stream && p.key).map((p) => p.ptsUs);
    expect(keyPts(1)).toEqual(keyPts(0));
  });

  it('reads the same packets when the bytes arrive a block at a time', async () => {
    const whole = await readAll(await open());
    const reader = new BlockAtATime(fx.bytes);
    const d = await open(reader);
    expect(d.info).toEqual((await open()).info);
    expect((await readAll(d)).map(identity)).toEqual(whole.map(identity));
    expect(reader.fills).toBeGreaterThanOrEqual(Math.floor(fx.bytes.length / BlockAtATime.BLOCK));
  });

  it('seeks back to the start', async () => {
    const d = await open(new BlockAtATime(fx.bytes));
    const first = (await readAll(d)).slice(0, 4);
    d.seekToStart();
    const again = (await readAll(d)).slice(0, 4);
    expect(again.map(identity)).toEqual(first.map(identity));
  });

  it('seeks to keyframes while the bytes arrive a block at a time', async () => {
    const reference = await open();
    const d = await open(new BlockAtATime(fx.bytes));
    const frameUs = 1e6 / d.info.frameRate;
    for (const frame of [d.info.totalFrames - 2, 5, Math.floor(d.info.totalFrames / 2), 0]) {
      const timeUs = frame * frameUs;
      await reference.seekToTimeUs(timeUs);
      await d.seekToTimeUs(timeUs);
      const p = (await d.read())!;
      expect(p).toMatchObject({ stream: 0, key: true });
      expect(p.ptsUs).toBeLessThanOrEqual(timeUs + 1000);
      expect(identity(p).slice(0, 3)).toEqual(identity((await reference.read())!).slice(0, 3));
    }
  });

  it('stops waiting for bytes when interrupted, and seeks after', async () => {
    const source = new BlockAtATime(fx.bytes);
    const d = await open(source);
    source.hold();
    // Packets in blocks already held come at once; read until one waits for the source
    let reading!: Promise<string>;
    for (let i = 0; i < 1000; i++) {
      reading = d.read().then(
        () => 'read',
        (e: Error) => e.name,
      );
      const first = await Promise.race([reading, new Promise((resolve) => setTimeout(resolve, 20, 'waiting'))]);
      if (first === 'waiting') break;
      expect(first).toBe('read');
    }
    d.interrupt();
    expect(await reading).toBe('AbortError');
    source.release();
    await d.seekToTimeUs(0);
    expect(await d.read()).toMatchObject({ stream: 0, key: true, ptsUs: 0 });
  });

  it('reports a failing source as LOAD_FAILED', async () => {
    const source = new BlockAtATime(fx.bytes);
    const d = await open(source);
    source.failWith = new Error('connection reset');
    const error = await readAll(d).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(AlphaVideoError);
    expect((error as AlphaVideoError).code).toBe('LOAD_FAILED');
    expect((error as Error).message).toContain('connection reset');
  });

  it('closes its reader', async () => {
    const reader = new BlockAtATime(fx.bytes);
    const d = (await WasmDemuxer.open(reader))!;
    d.close();
    expect(reader.closed).toBe(true);
    await expect(d.read()).rejects.toMatchObject({ name: 'AbortError' });
  });
});

describe('WasmDemuxer.open', () => {
  it('is null for bytes that are not a WebM', async () => {
    const d = await WasmDemuxer.open(await openReader(memorySource(new Uint8Array(5000).fill(7))));
    expect(d).toBeNull();
  });
});

describe('yuvToRgbMatrix', () => {
  it('is the native BT.601 limited-range matrix for small unspecified video', async () => {
    const m = await yuvToRgbMatrix(0, 2, 640, 480);
    // R = 1.164 (Y - 16/255) + 1.596 (V - 0.5)
    expect(m[0]).toBeCloseTo(1.164, 2);
    expect(m[8]).toBeCloseTo(1.596, 2);
  });

  it('switches to BT.709 above 720×576', async () => {
    const m = await yuvToRgbMatrix(0, 2, 1920, 1080);
    expect(m[8]).toBeCloseTo(1.793, 2);
  });
});

describe('sniffWebM', () => {
  it('rejects bytes that are not a WebM', () => {
    expect(sniffWebM(new Uint8Array([1, 2, 3, 4]))).toBeNull();
  });
});
