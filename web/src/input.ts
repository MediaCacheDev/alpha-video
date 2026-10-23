/**
 * The file a player was given, for either kind of engine: read by range through a ByteReader
 * (range-source-web) for the canvas engines, with a store for URLs, or as a URL for `<video>`,
 * through the range-source Service Worker when one controls the page.
 */
import {
  blobSource,
  createStore,
  httpSource,
  isByteSource,
  memorySource,
  openReader,
  rangeSourceWorker,
  serviceWorkerUrl,
  type ByteReader,
  type ByteSource,
  type ByteStore,
} from 'range-source-web';
import { AlphaVideoError } from './errors';
import { fetchHead } from './sniff';
import type { AlphaVideoOptions, AlphaVideoSource, AlphaVideoStorage } from './types';

/** Keeps this player's stored clips apart from other players' (each has its own cap). */
export const STORAGE_NAMESPACE = 'alpha-video';

const HEAD_BYTES = 65536;

/** A URL `<video>` plays, and what to call once it's done with it. */
export interface VideoSource {
  readonly url: string;
  release(): void;
}

export class Input {
  readonly src: AlphaVideoSource;
  private readonly source: ByteSource;
  private readonly storage: AlphaVideoStorage;
  private readonly store: ByteStore | null;
  private readonly key: string | undefined;
  private readonly maxBytes: number | undefined;
  /** For a URL: fetch options, with every header */
  private readonly init: RequestInit = {};
  private readonly headers: Headers = new Headers();
  /** For a URL: fetch options other than headers, which only the page's own fetch can send */
  private readonly hasInit: boolean = false;

  constructor(options: AlphaVideoOptions) {
    const { src } = options;
    this.src = src;
    let key: string | undefined = options.cacheKey;
    let storage = options.storage ?? 'none';
    if (typeof src === 'string') {
      const { headers: initHeaders, method: _method, signal: _signal, body: _body, ...init } = options.fetchInit ?? {};
      const headers = new Headers(initHeaders);
      for (const [name, value] of Object.entries(options.headers ?? {})) headers.set(name, value);
      this.headers = headers;
      this.hasInit = Object.keys(init).length > 0;
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
    this.storage = storage;
    this.maxBytes = options.maxStorageBytes;
    this.store = key ? createStore(storage, { namespace: STORAGE_NAMESPACE, maxBytes: options.maxStorageBytes }) : null;
  }

  /** A reader over the file, through the store if there is one. Rejects with LOAD_FAILED. */
  async open(signal: AbortSignal, readAheadBytes?: number): Promise<ByteReader> {
    try {
      return await openReader(this.source, { store: this.store, key: this.key, signal, readAheadBytes });
    } catch (e) {
      throw this.loadError(e, signal);
    }
  }

  /**
   * The file's first bytes, for sniffing which engine it needs. Null for a ByteSource (read by
   * the demuxer, which opens it once), and for a URL held in the store when no worker serves
   * `<video>` (the demuxer reads it there, offline too). With a worker, a URL's head is read
   * through the store, so `<video>` then gets those bytes from it. Throws if a URL can't be read
   * from script: CORS, which `<video>` doesn't need.
   */
  async head(signal: AbortSignal): Promise<Uint8Array | null> {
    const { src } = this;
    if (typeof src === 'string') {
      if (await rangeSourceWorker()) return this.readHead(signal);
      if (this.store && this.key && (await this.store.lookup(this.key).catch(() => null))) return null;
      return fetchHead(src, HEAD_BYTES, signal, this.init);
    }
    if (src instanceof ArrayBuffer) return new Uint8Array(src, 0, Math.min(HEAD_BYTES, src.byteLength));
    if (src instanceof Uint8Array) return src.subarray(0, HEAD_BYTES);
    if (typeof Blob !== 'undefined' && src instanceof Blob) return new Uint8Array(await src.slice(0, HEAD_BYTES).arrayBuffer());
    return null;
  }

  /**
   * What `<video>` plays. With a range-source worker controlling the page, a URL the worker
   * streams by range from the store and into it, for any source. Without one, a URL with no
   * headers as it is, or a Blob as a blob: URL of itself, revoked on release; null for the rest,
   * which only the canvas engines can read.
   */
  async videoSource(): Promise<VideoSource | null> {
    const { src } = this;
    const worker = await serviceWorkerUrl(this.workerSource(), {
      namespace: STORAGE_NAMESPACE,
      storage: this.workerStorage(),
      maxBytes: this.maxBytes,
      key: this.key,
      headers: this.headers,
      contentType: (typeof Blob !== 'undefined' && src instanceof Blob && src.type) || 'video/webm',
    }).catch(() => null);
    if (worker) return worker;
    if (typeof src === 'string') return [...this.headers.keys()].length ? null : { url: src, release: () => {} };
    if (typeof Blob !== 'undefined' && src instanceof Blob) {
      const url = URL.createObjectURL(src);
      return { url, release: () => URL.revokeObjectURL(url) };
    }
    return null;
  }

  /** The URL itself, which the worker fetches, unless it needs fetch options only the page has. */
  private workerSource(): AlphaVideoSource {
    return typeof this.src === 'string' && this.hasInit ? this.source : this.src;
  }

  /**
   * The store kind for the worker to open, or the page's own store, read on the page: a store
   * object, 'memory' (this page's), and what `workerSource` reads on the page.
   */
  private workerStorage(): AlphaVideoStorage {
    const { storage, store } = this;
    if (typeof storage === 'object' || storage === 'memory' || this.workerSource() !== this.src) return store ?? 'none';
    return storage;
  }

  private async readHead(signal: AbortSignal): Promise<Uint8Array> {
    const reader = await this.open(signal, 0);
    try {
      const head = await reader.read(0, Math.min(HEAD_BYTES, reader.size), signal);
      // So the worker answers <video>'s first request from the store
      await reader.stored?.();
      return head;
    } catch (e) {
      throw this.loadError(e, signal);
    } finally {
      reader.close();
    }
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
