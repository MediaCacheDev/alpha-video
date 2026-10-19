import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

/** The repo's test cards (tests/fixtures), shared with the C++ host tests. */
export const FIXTURES = join(__dirname, '../../tests/fixtures');

export interface Fixture {
  /** e.g. card_256x256/dual_av1.webm */
  name: string;
  bytes: Uint8Array;
  width: number;
  height: number;
  codec: 'vp9' | 'av1' | 'vp8';
  bitDepth: number;
  blockAdditional: boolean;
}

export function cards(): Fixture[] {
  const out: Fixture[] = [];
  for (const dir of readdirSync(FIXTURES)) {
    const m = /^card_(\d+)x(\d+)$/.exec(dir);
    if (!m || !statSync(join(FIXTURES, dir)).isDirectory()) continue;
    for (const file of readdirSync(join(FIXTURES, dir)).filter((f) => f.endsWith('.webm'))) {
      out.push({
        name: `${dir}/${file}`,
        bytes: new Uint8Array(readFileSync(join(FIXTURES, dir, file))),
        width: Number(m[1]),
        height: Number(m[2]),
        codec: file.includes('av1') ? 'av1' : file.includes('vp8') ? 'vp8' : 'vp9',
        bitDepth: file.includes('10bit') ? 10 : 8,
        blockAdditional: file.startsWith('yuva'),
      });
    }
  }
  return out;
}
