// Plays WebM files through once with each engine and reports how close to real time they ran:
// the canvas engines' clock waits for decoding, so a slow engine takes longer than the clip.
//
//   npm run build && node test/browsers/speed.mjs <dir with .webm files> [chromium firefox webkit]
import { execFileSync } from 'node:child_process';
import { createReadStream, readdirSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium, firefox, webkit } from 'playwright';

const WEB = fileURLToPath(new URL('../..', import.meta.url));
const [mediaDir, ...wanted] = process.argv.slice(2);
if (!mediaDir) {
  console.error('usage: node test/browsers/speed.mjs <dir with .webm files> [chromium firefox webkit]');
  process.exit(1);
}
const files = readdirSync(mediaDir).filter((f) => f.endsWith('.webm'));
const DECODERS = ['auto', 'webcodecs', 'software'];
const TYPES = { '.html': 'text/html', '.mjs': 'text/javascript', '.webm': 'video/webm', '.json': 'application/json' };

function durationOf(file) {
  const out = execFileSync('ffprobe', ['-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', join(mediaDir, file)]);
  return Number(String(out).trim());
}

const server = createServer((req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  const file = path.startsWith('/media/') ? join(mediaDir, normalize(path.slice(7))) : join(WEB, normalize(path));
  let size;
  try {
    size = statSync(file).size;
  } catch {
    res.writeHead(404).end();
    return;
  }
  const headers = { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream', 'Accept-Ranges': 'bytes' };
  const range = /bytes=(\d+)-(\d*)/.exec(req.headers.range ?? '');
  if (range) {
    const start = Number(range[1]);
    const end = range[2] ? Math.min(Number(range[2]), size - 1) : size - 1;
    res.writeHead(206, { ...headers, 'Content-Range': `bytes ${start}-${end}/${size}`, 'Content-Length': end - start + 1 });
    createReadStream(file, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { ...headers, 'Content-Length': size });
    createReadStream(file).pipe(res);
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://localhost:${server.address().port}`;

for (const [name, type] of Object.entries({ chromium, firefox, webkit })) {
  if (wanted.length && !wanted.includes(name)) continue;
  const browser = await type.launch();
  const page = await browser.newPage();
  await page.goto(`${origin}/test/browsers/page.html`);
  await page.waitForFunction(() => window.ready);
  console.log(`\n${name} ${browser.version()}`);
  for (const file of files) {
    const duration = durationOf(file);
    for (const decoder of DECODERS) {
      const r = await page.evaluate(
        async ({ src, decoder, timeoutMs }) => {
          const { createAlphaVideo } = await import('/dist/index.mjs');
          // Frames shown: the canvas engines draw once per frame; <video> reports its own
          let drawn = 0;
          const drawArrays = WebGL2RenderingContext.prototype.drawArrays;
          WebGL2RenderingContext.prototype.drawArrays = function (...args) {
            drawn++;
            return drawArrays.apply(this, args);
          };
          const container = document.createElement('div');
          container.style.cssText = 'width:480px;height:270px';
          document.body.append(container);
          const state = { loadedAt: 0, decoder: '', player: null };
          const countVideoFrames = (video) => {
            if (!video.requestVideoFrameCallback) return;
            const tick = () => {
              drawn++;
              video.requestVideoFrameCallback(tick);
            };
            video.requestVideoFrameCallback(tick);
          };
          const result = await new Promise((resolve) => {
            state.player = createAlphaVideo(container, {
              src,
              decoder,
              loop: false,
              onLoad: (e) => {
                state.loadedAt = performance.now();
                state.decoder = e.decoder;
                state.totalFrames = e.totalFrames;
                const el = container.querySelector('video');
                if (el) countVideoFrames(el);
              },
              onEnd: () => resolve({ ms: performance.now() - state.loadedAt }),
              onError: (e) => resolve({ error: `${e.code}: ${e.message}` }),
            });
            setTimeout(() => resolve({ error: 'timed out' }), timeoutMs);
          });
          state.player.destroy();
          container.remove();
          WebGL2RenderingContext.prototype.drawArrays = drawArrays;
          return { ...result, decoder: state.decoder, drawn, totalFrames: state.totalFrames };
        },
        { src: `/media/${file}`, decoder, timeoutMs: duration * 1000 * 6 + 5000 },
      );
      const speed = r.ms
        ? `${(duration / (r.ms / 1000)).toFixed(2)}× real time, ${r.drawn} of ${r.totalFrames} frames shown`
        : r.error;
      console.log(`  ${file} [${decoder}] ${r.decoder ?? ''}: ${speed}`);
    }
  }
  await browser.close();
}
server.close();
