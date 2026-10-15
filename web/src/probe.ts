/**
 * Whether this browser's `<video>` shows WebM alpha. `canPlayType` can't tell: every engine says
 * "probably" for VP9 WebM, including WebKit, which drops the alpha. So a 16×16 one-frame clip,
 * left half transparent and right half opaque red, is drawn into a canvas and two pixels are read.
 *
 * The clip, 599 bytes:
 *   ffmpeg -f lavfi -i "color=c=red:s=16x16:d=0.04,format=rgba,geq=r=255:g=0:b=0:a='if(lt(X,8),0,255)'" \
 *     -frames:v 1 -c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0 -b:v 0 -crf 30 probe.webm
 */
const PROBE_WEBM =
  'GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAInEU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHWTbuMU6uEElTDZ1OsggEnTbuMU6uEHFO7a1OsggIR7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsCrXsYMPQkBNgIxMYXZmNjIuMy4xMDBXQYxMYXZmNjIuMy4xMDBEiYhARAAAAAAAABZUrmvMrgEAAAAAAABD14EBc8WIvx3QsR1SUVecgQAitZyDdW5kiIEAhoVWX1ZQOYOBASPjg4QCYloA4JSwgRC6gRCagQJTwIEBVbCEVbmBARJUw2dAf3Nzn2PAgGfImUWjh0VOQ09ERVJEh4xMYXZmNjIuMy4xMDBzc9pjwItjxYi/HdCxHVJRV2fIpUWjh0VOQ09ERVJEh5hMYXZjNjIuMTEuMTAwIGxpYnZweC12cDlnyKFFo4hEVVJBVElPTkSHkzAwOjAwOjAwLjA0MDAwMDAwMAAfQ7Z14OeBAKDboayBAAAAgkmDQgAA8AD2ADgkHBg+AAAwYAAAEL//+NF3////8g+/////6iMAAHWhqqao7oEBpaOCSYNCAADwAPYAOCQcGD4AACAgAFFgf///a9l///+2YRrYABxTu2uRu4+zgQC3iveBAfGCAazwgQM=';

/** How long to wait for the probe's first frame, e.g. on iOS in Low Power Mode, which loads no media without a tap. */
const PROBE_TIMEOUT_MS = 1000;

let probe: Promise<boolean> | null = null;

/**
 * Resolves true if `<video>` in this browser keeps WebM VP9 alpha. Runs once per page; later
 * calls share the result. Never rejects: anything that goes wrong counts as false.
 */
export function videoElementKeepsWebMAlpha(): Promise<boolean> {
  if (!probe) probe = runProbe();
  return probe;
}

async function runProbe(): Promise<boolean> {
  if (typeof document === 'undefined') return false;
  const bytes = Uint8Array.from(atob(PROBE_WEBM), (c) => c.charCodeAt(0));
  // A blob: URL, not data:. WebKit treats a video from a data: URL as cross-origin, so reading
  // its pixels back throws.
  const url = URL.createObjectURL(new Blob([bytes], { type: 'video/webm' }));
  const video = document.createElement('video');
  video.muted = true;
  video.playsInline = true;
  video.preload = 'auto';
  try {
    const loaded = await firstFrame(video, url);
    if (!loaded) return false;
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 16;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    if (!ctx) return false;
    ctx.drawImage(video, 0, 0, 16, 16);
    const transparent = ctx.getImageData(3, 8, 1, 1).data;
    const opaque = ctx.getImageData(12, 8, 1, 1).data;
    return transparent[3] < 32 && opaque[3] > 224 && opaque[0] > 200;
  } catch {
    return false;
  } finally {
    video.removeAttribute('src');
    video.load();
    URL.revokeObjectURL(url);
  }
}

function firstFrame(video: HTMLVideoElement, url: string): Promise<boolean> {
  return new Promise((resolve) => {
    const done = (ok: boolean) => {
      clearTimeout(timeout);
      clearTimeout(nudge);
      video.onloadeddata = video.onerror = null;
      resolve(ok);
    };
    const timeout = setTimeout(() => done(false), PROBE_TIMEOUT_MS);
    // Some browsers only decode the first frame once asked to play
    const nudge = setTimeout(() => video.play().catch(() => {}), 300);
    video.onloadeddata = () => done(true);
    video.onerror = () => done(false);
    video.src = url;
    video.load();
  });
}
