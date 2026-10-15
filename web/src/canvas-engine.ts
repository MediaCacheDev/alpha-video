/**
 * Plays decoded pairs into a canvas on the page's frame clock: the WebCodecs engine's playback
 * loop. When decoding falls behind, the clock waits for it rather than skipping ahead.
 */
import { styleElement, type Engine, type EngineEvents, type EngineOptions } from './engine';
import type { AlphaVideoError } from './errors';
import { AlphaRenderer, type DrawParams } from './renderer';
import type { FramePair, PairSource, Picture, SourceCallbacks } from './source';

/** A gap between animation frames longer than this (a hidden tab, a long task) doesn't advance the clock. */
const MAX_STEP_MS = 250;

const MATRIX_OF: Record<string, number> = { rgb: 0, bt709: 1, bt470bg: 5, smpte170m: 6, 'bt2020-ncl': 9 };
const AVCOL_RANGE_UNSPECIFIED = 0;
const AVCOL_RANGE_MPEG = 1;
const AVCOL_RANGE_JPEG = 2;
const AVCOL_SPC_RGB = 0;
const AVCOL_SPC_UNSPECIFIED = 2;

export class CanvasEngine implements Engine {
  readonly element: HTMLCanvasElement;
  private readonly source: PairSource;
  private readonly renderer: AlphaRenderer;
  private readonly options: EngineOptions;
  private readonly events: EngineEvents;
  private readonly frameUs: number;
  private params: DrawParams | null = null;
  private current: FramePair | null = null;
  private clockUs = 0;
  private playing: boolean;
  private ended = false;
  private lastTick = 0;
  private raf = 0;
  private destroyed = false;

  private constructor(canvas: HTMLCanvasElement, renderer: AlphaRenderer, source: PairSource, options: EngineOptions, events: EngineEvents) {
    this.element = canvas;
    this.renderer = renderer;
    this.source = source;
    this.options = options;
    this.events = events;
    this.playing = options.autoPlay;
    this.frameUs = 1e6 / (source.info.frameRate || 30);
  }

  /** Plays the source `makeSource` opens, which the engine then owns. */
  static async create(
    makeSource: (callbacks: SourceCallbacks) => Promise<PairSource>,
    options: EngineOptions,
    events: EngineEvents,
  ): Promise<CanvasEngine> {
    const canvas = document.createElement('canvas');
    styleElement(canvas, options.objectFit);
    const renderer = new AlphaRenderer(canvas);
    let engine: CanvasEngine | null = null;
    let source: PairSource;
    try {
      source = await makeSource({
        onUpdate: () => engine?.wake(),
        onError: (e) => engine?.fail(e),
      });
    } catch (e) {
      renderer.dispose();
      throw e;
    }
    const { info } = source;
    canvas.width = info.width;
    canvas.height = info.height;
    engine = new CanvasEngine(canvas, renderer, source, options, events);
    events.onLoad({ totalFrames: info.totalFrames, width: info.width, height: info.height, decoder: source.decoderName });
    source.pump();
    return engine;
  }

  play(): void {
    if (this.ended) this.seekToTime(0);
    this.ended = false;
    this.playing = true;
    this.wake();
  }

  pause(): void {
    this.playing = false;
  }

  stop(): void {
    this.playing = false;
    this.seekToTime(0);
  }

  seekToFrame(frame: number): void {
    const last = Math.max(0, this.source.info.totalFrames - 1);
    const target = Math.min(Math.max(0, Math.floor(frame) || 0), last);
    const fileUs = target * this.frameUs;
    // The canvas keeps showing the old frame until the new one is drawn
    if (this.current) this.source.release(this.current);
    this.current = null;
    // Matroska rounds timestamps to the millisecond, so the frame wanted may be a little either side
    this.clockUs = this.source.seek(fileUs - this.frameUs / 2) + this.frameUs / 2;
    this.ended = false;
    this.wake();
  }

  seekToTime(seconds: number): void {
    this.seekToFrame(Math.floor((seconds > 0 ? seconds : 0) * 1e6 / this.frameUs + 1e-6));
  }

  setPlaybackSpeed(speed: number): void {
    this.options.playbackSpeed = speed;
  }

  setLoop(loop: boolean): void {
    this.options.loop = loop;
    this.source.setLoop(loop);
  }

  destroy(): void {
    if (this.destroyed) return;
    this.destroyed = true;
    cancelAnimationFrame(this.raf);
    if (this.current) this.source.release(this.current);
    this.current = null;
    this.source.close();
    this.renderer.dispose();
    this.element.remove();
  }

  /** Runs the frame loop until there's nothing to show. */
  private wake(): void {
    if (this.destroyed || this.raf) return;
    this.lastTick = performance.now();
    this.raf = requestAnimationFrame(this.tick);
  }

  private tick = (now: number) => {
    this.raf = 0;
    if (this.destroyed) return;
    const step = now - this.lastTick;
    this.lastTick = now;
    if (this.playing && this.current && step < MAX_STEP_MS) {
      this.clockUs += step * 1000 * this.options.playbackSpeed;
    }

    // The first frame after loading or seeking shows as soon as it's decoded
    const next = this.source.nextTimestampUs();
    if (!this.current && next !== null && next > this.clockUs) this.clockUs = next;

    const due = this.source.take(this.clockUs);
    if (due) {
      if (this.current) this.source.release(this.current);
      this.current = due;
      if (!this.params) this.params = this.drawParams(due);
      this.renderer.draw(due, this.params);
    } else if (this.playing && this.current && this.source.nextTimestampUs() === null) {
      // Decoding is behind: hold the clock at the frame on screen
      this.clockUs = Math.min(this.clockUs, this.current.timestampUs + this.frameUs);
    }

    if (this.playing && this.source.finished) {
      this.playing = false;
      this.ended = true;
      this.events.onEnd();
    }
    // Paused, the loop stops once the frame for the clock is on screen
    const waiting = !this.current && !this.source.finished;
    if (this.playing || waiting) this.raf = requestAnimationFrame(this.tick);
  };

  private fail(error: AlphaVideoError): void {
    if (this.destroyed) return;
    this.playing = false;
    this.events.onError(error);
  }

  /**
   * The colour matrix and alpha range: from the container where it says, else from the
   * decoder's frames. Mirrors the native players (cpp/YuvToRgb, YUVAFrame::copyAlphaFromAVFrame).
   */
  private drawParams(pair: FramePair): DrawParams {
    const { color, alpha } = this.source.info;
    const range = color.colorRange !== AVCOL_RANGE_UNSPECIFIED ? color.colorRange : rangeOf(pair.color);
    const space = color.colorSpace !== AVCOL_SPC_UNSPECIFIED ? color.colorSpace : spaceOf(pair.color);
    let alphaFullRange: boolean;
    if (alpha!.colorRange !== AVCOL_RANGE_UNSPECIFIED || alpha!.colorSpace === AVCOL_SPC_RGB) {
      alphaFullRange = alpha!.colorRange === AVCOL_RANGE_JPEG || alpha!.colorSpace === AVCOL_SPC_RGB;
    } else {
      alphaFullRange = rangeOf(pair.alpha) === AVCOL_RANGE_JPEG || spaceOf(pair.alpha) === AVCOL_SPC_RGB;
    }
    return {
      matrix: this.source.yuvToRgb(range, space),
      alphaFullRange,
      alphaUndoExpansion: this.source.info.alphaInBlockAdditional && pair.alpha.colorSpace.fullRange !== true,
    };
  }
}

function rangeOf(p: Picture): number {
  const full = p.colorSpace.fullRange;
  return full == null ? AVCOL_RANGE_UNSPECIFIED : full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

function spaceOf(p: Picture): number {
  return MATRIX_OF[p.colorSpace.matrix ?? ''] ?? AVCOL_SPC_UNSPECIFIED;
}
