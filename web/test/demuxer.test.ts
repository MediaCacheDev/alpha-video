import { afterAll, describe, expect, it } from 'vitest';
import { WasmDemuxer, yuvToRgbMatrix, type DemuxPacket } from '../src/demuxer';
import { sniffWebM } from '../src/sniff';
import { cards } from './fixtures';

const JPEG = 2;

describe.each(cards())('$name', (fx) => {
  const opened: WasmDemuxer[] = [];
  afterAll(() => opened.forEach((d) => d.close()));

  async function open(): Promise<WasmDemuxer> {
    const d = await WasmDemuxer.open(fx.bytes);
    expect(d).not.toBeNull();
    opened.push(d!);
    return d!;
  }

  function readAll(d: WasmDemuxer): DemuxPacket[] {
    const packets: DemuxPacket[] = [];
    for (let p = d.read(); p; p = d.read()) packets.push(p);
    return packets;
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
    const packets = readAll(d);
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

  it('seeks back to the start', async () => {
    const d = await open();
    const first = readAll(d).slice(0, 4);
    d.seekToStart();
    const again = readAll(d).slice(0, 4);
    expect(again.map((p) => [p.stream, p.ptsUs, p.key, p.data.length])).toEqual(
      first.map((p) => [p.stream, p.ptsUs, p.key, p.data.length]),
    );
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
