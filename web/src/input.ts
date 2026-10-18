/**
 * The file a player was given, for either kind of engine: read by range through a ByteReader
 * (alpha-video-source) for the canvas engines, with a store for URLs, or as a URL for `<video>`.
 */
import {
  blobSource,
  createStore,
  httpSource,
  isByteSource,
  memorySource,
  openReader,
  type ByteReader,
  type ByteSource,
  type ByteStore,
} from 'alpha-video-source';
import { AlphaVideoError } from './errors';
import { fetchHead } from './sniff';
import type { AlphaVideoOptions, AlphaVideoSource } from './types';

/** Keeps this player's stored clips apart from other players' (each has its own cap). */
export const STORAGE_NAMESPACE = 'alpha-video';

const HEAD_BYTES = 65536;

export class Input {
  readonly src: AlphaVideoSource;
  private readonly source: ByteSource;
  private readonly store: ByteStore | null;
  private readonly key: string | undefined;
  /** For a URL: fetch options, with every header */
  private readonly init: RequestInit = {};
  private readonly hasHeaders: boolean = false;

  constructor(options: AlphaVideoOptions) {
    const { src } = options;
    this.src = src;
    let key: string | undefined = options.cacheKey;
    let storage = options.storage ?? 'none';
    if (typeof src === 'string') {
      const { headers: initHeaders, method: _method, signal: _signal, body: _body, ...init } = options.fetchInit ?? {};
      const headers = new Headers(initHeaders);
      for (const [name, value] of Object.entries(options.headers ?? {})) headers.set(name, value);
      this.hasHeaders = [...headers.keys()].length > 0;
      this.init = { ...init, headers };
      this.source = httpSource(src, { headers, init });
      key ||= src;
      storage = options.storage ?? 'auto';
    } else if (src instanceof ArrayBuffer || src instanceof Uint8Array) {
      this.source = memorySource(src);
      key = undefined;
    } else if (typeof Blob !== 'undefined' && src instanceof Blob) {
      this.source = blobSource(src);
    } else if (isByteSource(src)) {
      this.source = src;
    } else {
      throw new AlphaVideoError('LOAD_FAILED', 'src takes a URL, a Blob, an ArrayBuffer, a Uint8Array or a ByteSource');
    }
    this.key = key;
    this.store = key ? createStore(storage, { namespace: STORAGE_NAMESPACE, maxBytes: options.maxStorageBytes }) : null;
  }

  /** A reader over the file, through the store if there is one. Rejects with LOAD_FAILED. */
  async open(signal: AbortSignal): Promise<ByteReader> {
    try {
      return await openReader(this.source, { store: this.store, key: this.key, signal });
    } catch (e) {
      throw this.loadError(e, signal);
    }
  }

  /**
   * The file's first bytes, for sniffing which engine it needs. Null for a URL held in the store
   * (the reader reads it there, offline too) and for a ByteSource (read by the demuxer, which
   * opens it once). Throws if a URL can't be read from script: CORS, which `<video>` doesn't need.
   */
  async head(signal: AbortSignal): Promise<Uint8Array | null> {
    const { src } = this;
    if (typeof src === 'string') {
      if (this.store && this.key && (await this.store.lookup(this.key).catch(() => null))) return null;
      return fetchHead(src, HEAD_BYTES, signal, this.init);
    }
    if (src instanceof ArrayBuffer) return new Uint8Array(src, 0, Math.min(HEAD_BYTES, src.byteLength));
    if (src instanceof Uint8Array) return src.subarray(0, HEAD_BYTES);
    if (typeof Blob !== 'undefined' && src instanceof Blob) return new Uint8Array(await src.slice(0, HEAD_BYTES).arrayBuffer());
    return null;
  }

  /** Whether `<video>` can load the file as it is: a URL without headers, a Blob or bytes. */
  get videoLoadable(): boolean {
    return typeof this.src === 'string' ? !this.hasHeaders : !isByteSource(this.src);
  }

  /**
   * A URL `<video>` plays: the URL, a blob: URL for a Blob or bytes, or for a ByteSource or a URL
   * that needs headers, a blob: URL of the whole file read through the reader (and the store).
   * `owned` blob: URLs are the caller's to revoke.
   */
  async videoUrl(signal: AbortSignal): Promise<{ url: string; owned: boolean }> {
    const { src } = this;
    if (typeof src === 'string' && !this.hasHeaders) return { url: src, owned: false };
    let blob: Blob;
    if (typeof Blob !== 'undefined' && src instanceof Blob) {
      blob = src;
    } else if (src instanceof ArrayBuffer || src instanceof Uint8Array) {
      blob = new Blob([src as BlobPart], { type: 'video/webm' });
    } else {
      const reader = await this.open(signal);
      try {
        blob = new Blob([(await reader.read(0, reader.size, signal)) as BlobPart], { type: 'video/webm' });
      } catch (e) {
        throw this.loadError(e, signal);
      } finally {
        reader.close();
      }
    }
    return { url: URL.createObjectURL(blob), owned: true };
  }

  private loadError(e: unknown, signal: AbortSignal): unknown {
    if (signal.aborted || e instanceof AlphaVideoError) return e;
    const message = String((e as Error)?.message ?? e);
    if (typeof this.src === 'string' && e instanceof TypeError) {
      return new AlphaVideoError('LOAD_FAILED', `Fetching ${this.src} failed; a file on another origin needs CORS headers (${message})`);
    }
    return new AlphaVideoError('LOAD_FAILED', message);
  }
}
