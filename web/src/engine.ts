import type { AlphaVideoError } from './errors';
import type { AlphaVideoLoadEvent, ObjectFit } from './types';

/** What the player needs from the `<video>` and the canvas engines. */
export interface Engine {
  readonly element: HTMLVideoElement | HTMLCanvasElement;
  play(): void;
  pause(): void;
  stop(): void;
  seekToFrame(frame: number): void;
  seekToTime(seconds: number): void;
  setPlaybackSpeed(speed: number): void;
  setLoop(loop: boolean): void;
  destroy(): void;
}

export interface EngineEvents {
  onLoad(event: AlphaVideoLoadEvent): void;
  onError(error: AlphaVideoError): void;
  onEnd(): void;
}

export interface EngineOptions {
  autoPlay: boolean;
  loop: boolean;
  playbackSpeed: number;
  objectFit: ObjectFit;
}

export function styleElement(element: HTMLElement, objectFit: ObjectFit): void {
  Object.assign(element.style, { display: 'block', width: '100%', height: '100%', objectFit });
}
