/** The `<video>` engine: what react-native-alpha-video does on the web today. */
import { styleElement, type Engine, type EngineEvents, type EngineOptions } from './engine';
import { AlphaVideoError } from './errors';
import type { VideoSource } from './input';

/** The browser doesn't expose the frame rate. */
const WEB_FPS = 30;

export class VideoEngine implements Engine {
  readonly element: HTMLVideoElement;
  private readonly source: VideoSource;
  private readonly options: EngineOptions;

  /** Plays `source.url`, and releases `source` on destroy. */
  constructor(source: VideoSource, options: EngineOptions, events: EngineEvents) {
    this.options = options;
    this.source = source;
    const video = document.createElement('video');
    this.element = video;
    video.muted = true;
    video.playsInline = true;
    video.loop = options.loop;
    video.autoplay = options.autoPlay;
    video.preload = 'auto';
    styleElement(video, options.objectFit);
    video.addEventListener('loadedmetadata', () => {
      // Loading a source resets playbackRate to 1
      video.playbackRate = this.options.playbackSpeed;
      events.onLoad({
        totalFrames: Math.floor(video.duration * WEB_FPS),
        width: video.videoWidth,
        height: video.videoHeight,
        decoder: 'browser',
      });
      if (this.options.autoPlay) video.play().catch(() => {});
    });
    video.addEventListener('error', () => {
      events.onError(new AlphaVideoError('LOAD_FAILED', video.error?.message || 'The video element could not load the file'));
    });
    video.addEventListener('ended', () => events.onEnd());
    video.src = source.url;
  }

  play(): void {
    this.element.play().catch(() => {});
  }

  pause(): void {
    this.element.pause();
  }

  stop(): void {
    this.element.pause();
    this.element.currentTime = 0;
  }

  seekToFrame(frame: number): void {
    const video = this.element;
    if (!(video.duration > 0)) return;
    const lastFrame = Math.max(0, Math.floor(video.duration * WEB_FPS) - 1);
    const target = Math.min(Math.max(0, Math.floor(frame) || 0), lastFrame);
    // Chrome moves a second seek into the last frame to the very end, which wraps a looping clip to 0
    if (Math.floor(video.currentTime * WEB_FPS + 1e-6) === target && !video.seeking) return;
    // The middle of the frame, so rounding in the browser can't land on the one before
    video.currentTime = (target + 0.5) / WEB_FPS;
  }

  seekToTime(seconds: number): void {
    this.seekToFrame(Math.floor((seconds > 0 ? seconds : 0) * WEB_FPS + 1e-6));
  }

  setPlaybackSpeed(speed: number): void {
    this.options.playbackSpeed = speed;
    this.element.playbackRate = speed;
  }

  setLoop(loop: boolean): void {
    this.element.loop = loop;
  }

  destroy(): void {
    const video = this.element;
    video.pause();
    video.removeAttribute('src');
    video.load();
    video.remove();
    this.source.release();
  }
}
