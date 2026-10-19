/**
 * What a WebM holds, from its first bytes, so a file `<video>` can play needn't load the
 * WebAssembly demuxer. Follows the C++ demuxer's rule (cpp/AlphaDemuxer): colour is the first
 * VP8, VP9 or AV1 track, alpha the second, or a single VP8 or VP9 track's BlockAdditional.
 */

export type WebMLayout =
  /** One VP8 or VP9 track with alpha in each block's BlockAdditional: the file browsers play with alpha. */
  | 'blockadditional'
  /** Colour and alpha in two VP8, VP9 or AV1 tracks. */
  | 'dual'
  /** One track and no alpha. */
  | 'opaque';

export interface WebMSniff {
  layout: WebMLayout;
  codec: 'vp9' | 'av1' | 'vp8';
  width: number;
  height: number;
}

const CODEC_IDS: Record<string, WebMSniff['codec']> = { V_VP8: 'vp8', V_VP9: 'vp9', V_AV1: 'av1' };

const ID = {
  EBML: 0x1a45dfa3,
  Segment: 0x18538067,
  Tracks: 0x1654ae6b,
  TrackEntry: 0xae,
  TrackType: 0x83,
  CodecID: 0x86,
  Video: 0xe0,
  AlphaMode: 0x53c0,
  PixelWidth: 0xb0,
  PixelHeight: 0xba,
  Cluster: 0x1f43b675,
  BlockGroup: 0xa0,
  BlockAdditions: 0x75a1,
  SimpleBlock: 0xa3,
} as const;

// Masters whose children are read in place; every other element is skipped by its size
const MASTERS = new Set<number>([ID.Segment, ID.Tracks, ID.TrackEntry, ID.Video, ID.Cluster, ID.BlockGroup]);

interface Element {
  id: number;
  dataStart: number;
  /** -1 for an unknown size (a live or streamed Segment or Cluster) */
  size: number;
}

function readVint(b: Uint8Array, pos: number, keepMarker: boolean): { value: number; length: number } | null {
  if (pos >= b.length) return null;
  const first = b[pos];
  let length = 1;
  while (length <= 8 && !(first & (0x80 >> (length - 1)))) length++;
  if (length > 8 || pos + length > b.length) return null;
  let value = keepMarker ? first : first & (0xff >> length);
  let allOnes = value === (0xff >> length);
  for (let i = 1; i < length; i++) {
    value = value * 256 + b[pos + i];
    if (b[pos + i] !== 0xff) allOnes = false;
  }
  return { value: !keepMarker && allOnes ? -1 : value, length };
}

function readElement(b: Uint8Array, pos: number): Element | null {
  const id = readVint(b, pos, true);
  if (!id) return null;
  const size = readVint(b, pos + id.length, false);
  if (!size) return null;
  return { id: id.value, dataStart: pos + id.length + size.length, size: size.value };
}

function readUint(b: Uint8Array, e: Element): number {
  let v = 0;
  for (let i = 0; i < e.size; i++) v = v * 256 + b[e.dataStart + i];
  return v;
}

function readString(b: Uint8Array, e: Element): string {
  let s = '';
  for (let i = 0; i < e.size && b[e.dataStart + i] !== 0; i++) s += String.fromCharCode(b[e.dataStart + i]);
  return s;
}

interface Track {
  video: boolean;
  codecId: string;
  alphaMode: number;
  width: number;
  height: number;
}

/**
 * Reads `bytes`, the start of a WebM (64 KB is plenty for files from FFmpeg or libwebm). Returns
 * null if it isn't a WebM with a VP8, VP9 or AV1 track, or if the bytes end before the tracks do.
 */
export function sniffWebM(bytes: Uint8Array): WebMSniff | null {
  const tracks: Track[] = [];
  let track: Track | null = null;
  let tracksDone = false;
  let blockAdditionsSeen = false;
  let firstBlockSeen = false;
  // End offsets of the open masters, innermost last, to know when a TrackEntry closes
  const open: { id: number; end: number }[] = [];

  let pos = 0;
  const first = readElement(bytes, 0);
  if (!first || first.id !== ID.EBML) return null;
  pos = first.dataStart + first.size;

  while (pos < bytes.length) {
    while (open.length && pos >= open[open.length - 1].end) {
      const closed = open.pop()!.id;
      if (closed === ID.Tracks) tracksDone = true;
      if (closed === ID.TrackEntry && track) {
        tracks.push(track);
        track = null;
      }
    }
    const e = readElement(bytes, pos);
    if (!e) break;
    const end = e.size < 0 ? Infinity : e.dataStart + e.size;

    if (MASTERS.has(e.id)) {
      if (e.id === ID.TrackEntry) track = { video: false, codecId: '', alphaMode: 0, width: 0, height: 0 };
      if (e.id === ID.Cluster) tracksDone = true;
      if (e.id === ID.BlockGroup) firstBlockSeen = true;
      open.push({ id: e.id, end });
      pos = e.dataStart;
      continue;
    }
    if (e.id === ID.BlockAdditions) blockAdditionsSeen = true;
    if (e.id === ID.SimpleBlock) firstBlockSeen = true;
    if (end > bytes.length) break;
    if (track) {
      if (e.id === ID.TrackType) track.video = readUint(bytes, e) === 1;
      else if (e.id === ID.CodecID) track.codecId = readString(bytes, e);
      else if (e.id === ID.AlphaMode) track.alphaMode = readUint(bytes, e);
      else if (e.id === ID.PixelWidth) track.width = readUint(bytes, e);
      else if (e.id === ID.PixelHeight) track.height = readUint(bytes, e);
    }
    if (tracksDone && firstBlockSeen && !open.some((o) => o.id === ID.BlockGroup)) break;
    pos = end;
  }
  while (open.length) {
    if (open.pop()!.id === ID.TrackEntry && track) tracks.push(track);
  }

  const video = tracks.filter((t) => t.video && CODEC_IDS[t.codecId]);
  if (!video.length || !tracksDone) return null;
  const colour = video[0];
  const codec = CODEC_IDS[colour.codecId];
  let layout: WebMLayout = 'opaque';
  if (video.length >= 2) layout = 'dual';
  else if (codec !== 'av1' && (colour.alphaMode === 1 || blockAdditionsSeen)) layout = 'blockadditional';
  return { layout, codec, width: colour.width, height: colour.height };
}

/**
 * The first `limit` bytes of `url`: a Range request, or the start of a full response, cancelled
 * once enough has arrived. Throws if the file can't be fetched (including CORS). `init` adds
 * fetch options and headers.
 */
export async function fetchHead(url: string, limit = 65536, signal?: AbortSignal, init?: RequestInit): Promise<Uint8Array> {
  const headers = new Headers(init?.headers);
  headers.set('Range', `bytes=0-${limit - 1}`);
  const response = await fetch(url, { ...init, headers, signal });
  if (!response.ok) throw new Error(`HTTP ${response.status} for ${url}`);
  if (!response.body) return new Uint8Array(await response.arrayBuffer()).subarray(0, limit);
  const reader = response.body.getReader();
  const chunks: Uint8Array[] = [];
  let length = 0;
  while (length < limit) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    length += value.length;
  }
  reader.cancel().catch(() => {});
  const out = new Uint8Array(Math.min(length, limit));
  let offset = 0;
  for (const c of chunks) {
    const n = Math.min(c.length, out.length - offset);
    out.set(c.subarray(0, n), offset);
    offset += n;
    if (offset >= out.length) break;
  }
  return out;
}
