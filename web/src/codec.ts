/** WebCodecs codec strings for a stream, from its first keyframe and Matroska CodecPrivate. */
import type { StreamInfo } from './demuxer';

export interface Vp9Header {
  profile: number;
  bitDepth: number;
}

/** The profile and bit depth in a VP9 keyframe's uncompressed header, or null if it isn't one. */
export function readVp9Header(data: Uint8Array): Vp9Header | null {
  let bit = 0;
  const read = (n: number) => {
    let v = 0;
    for (let i = 0; i < n; i++, bit++) {
      if (bit >> 3 >= data.length) throw new RangeError();
      v = (v << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
    }
    return v;
  };
  try {
    if (read(2) !== 2) return null; // frame_marker
    const low = read(1);
    const profile = (read(1) << 1) | low;
    if (profile === 3) read(1);
    if (read(1)) return null; // show_existing_frame
    if (read(1) !== 0) return null; // frame_type: 0 is a keyframe
    read(2); // show_frame, error_resilient_mode
    if (read(8) !== 0x49 || read(8) !== 0x83 || read(8) !== 0x42) return null;
    const bitDepth = profile >= 2 ? (read(1) ? 12 : 10) : 8;
    return { profile, bitDepth };
  } catch {
    return null;
  }
}

// VP9 levels by the largest picture each allows, in luma samples
const VP9_LEVELS: [number, string][] = [
  [36864, '10'],
  [73728, '11'],
  [122880, '20'],
  [245760, '21'],
  [552960, '30'],
  [983040, '31'],
  [2228224, '41'],
  [8912896, '51'],
  [35651584, '61'],
];

const two = (n: number) => String(n).padStart(2, '0');

export function vp9CodecString(header: Vp9Header, width: number, height: number): string {
  const samples = width * height;
  const level = VP9_LEVELS.find(([max]) => samples <= max)?.[1] ?? '62';
  return `vp09.${two(header.profile)}.${level}.${two(header.bitDepth)}`;
}

/** From an av1C record: byte 1 is seq_profile(3) seq_level_idx_0(5), byte 2 seq_tier_0, high_bitdepth, twelve_bit… */
export function av1CodecString(av1C: Uint8Array | null, bitDepth: number): string {
  if (!av1C || av1C.length < 4) return `av01.0.08M.${two(bitDepth)}`;
  const profile = av1C[1] >> 5;
  const level = av1C[1] & 0x1f;
  const tier = av1C[2] & 0x80 ? 'H' : 'M';
  const depth = av1C[2] & 0x40 ? (av1C[2] & 0x20 ? 12 : 10) : 8;
  return `av01.${profile}.${two(level)}${tier}.${two(depth)}`;
}

/** The codec string for `stream`, given its first packet (a keyframe). */
export function codecString(stream: StreamInfo, firstKey: Uint8Array, width: number, height: number): string {
  if (stream.codec === 'av1') return av1CodecString(stream.codecPrivate, stream.bitDepth);
  if (stream.codec === 'vp8') return 'vp8';
  const header = readVp9Header(firstKey) ?? { profile: stream.bitDepth > 8 ? 2 : 0, bitDepth: stream.bitDepth };
  return vp9CodecString(header, width, height);
}
