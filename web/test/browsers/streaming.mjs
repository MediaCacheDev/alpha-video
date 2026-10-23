// Streams a 10 s, 4 MB dual-stream clip into the canvas engines in Chromium, Firefox and WebKit:
// from a server with Range requests (a far seek fetches from near the end, past what was read
// ahead), from one without (the whole file, in one response), and again from the store with no
// requests at all. Then a single-track alpha clip, which <video> plays where it keeps alpha,
// through the range-source Service Worker (stored, headers, bytes) and without it (a Blob; bytes
// go to the canvas engines, or fail with NEEDS_SERVICE_WORKER when <video> is asked for).
//
//   npm run build && node test/browsers/streaming.mjs [chromium firefox webkit]
//
// Makes the clips with ffmpeg (libvpx-vp9) on first run, in test/browsers/build.
import { execFileSync } from 'node:child_process';
import { createReadStream, existsSync, mkdirSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium, firefox, webkit } from 'playwright';

const WEB = fileURLToPath(new URL('../..', import.meta.url));
const BUILD = join(WEB, 'test/browsers/build');
const CLIP = join(BUILD, 'stream.webm');
const TYPES = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.webm': 'video/webm' };

if (!existsSync(CLIP)) {
  mkdirSync(BUILD, { recursive: true });
  // Noise, so it doesn't compress to nothing; a keyframe a second, alpha as a second track
  execFileSync('ffmpeg', [
    '-v', 'error', '-y',
    '-f', 'lavfi', '-i', 'testsrc2=s=480x270:r=30:d=10,noise=alls=12:allf=t',
    '-f', 'lavfi', '-i', 'gradients=s=480x270:r=30:d=10',
    '-filter_complex', '[0:v]format=yuv420p[c];[1:v]format=gray,format=yuv420p[a]',
    '-map', '[c]', '-map', '[a]',
    '-c:v', 'libvpx-vp9', '-crf', '36', '-b:v', '0', '-g', '30', '-keyint_min', '30', '-auto-alt-ref', '0',
    '-deadline', 'realtime', '-cpu-used', '8', '-row-mt', '1', '-metadata:s:v:1', 'title=Alpha',
    CLIP,
  ]);
}
const SIZE = statSync(CLIP).size;
const ALPHA_CLIP = join(BUILD, 'alpha.webm');
if (!existsSync(ALPHA_CLIP)) {
  // Alpha in BlockAdditional, a single track
  execFileSync('ffmpeg', [
    '-v', 'error', '-y',
    '-f', 'lavfi', '-i', 'testsrc2=s=480x270:r=30:d=10,noise=alls=12:allf=t',
    '-f', 'lavfi', '-i', 'gradients=s=480x270:r=30:d=10',
    '-filter_complex', '[1:v]format=gray[a];[0:v][a]alphamerge,format=yuva420p',
    '-c:v', 'libvpx-vp9', '-crf', '36', '-b:v', '0', '-g', '30', '-auto-alt-ref', '0',
    '-deadline', 'realtime', '-cpu-used', '8', '-row-mt', '1',
    ALPHA_CLIP,
  ]);
}
const CLIPS = { '/stream.webm': CLIP, '/alpha.webm': ALPHA_CLIP };
const WORKER_SCRIPT = join(WEB, 'node_modules/range-source-web/dist/range-source-sw.js');

/** Serves the page, the clips and /range-source-sw.js, with or without Range support, logging the clips' requests. */
function serve(ranges) {
  const log = [];
  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    const path = decodeURIComponent(url.pathname);
    const clip = path in CLIPS;
    const file = clip ? CLIPS[path] : path === '/range-source-sw.js' ? WORKER_SCRIPT : join(WEB, normalize(path));
    let size;
    try {
      size = statSync(file).size;
    } catch {
      res.writeHead(404).end();
      return;
    }
    const headers = { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream', 'Cache-Control': 'no-store' };
    if (ranges) headers['Accept-Ranges'] = 'bytes';
    const range = ranges ? /bytes=(\d*)-(\d*)/.exec(req.headers.range ?? '') : null;
    let start = 0;
    let end = size - 1;
    if (range) {
      start = range[1] ? Number(range[1]) : size - Number(range[2]);
      if (range[1] && range[2]) end = Math.min(Number(range[2]), size - 1);
      res.writeHead(206, { ...headers, 'Content-Range': `bytes ${start}-${end}/${size}`, 'Content-Length': end - start + 1 });
    } else {
      res.writeHead(200, { ...headers, 'Content-Length': size });
    }
    if (clip) log.push({ query: url.search, start, token: req.headers['x-token'] });
    const stream = createReadStream(file, { start, end });
    stream.pipe(res);
    res.on('close', () => stream.destroy());
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ server, log, origin: `http://localhost:${server.address().port}` })));
}

/**
 * In the page: loads the clip paused, seeks, and reports whether a frame with alpha is on the
 * canvas or the video. `bytesFrom` and `blobFrom` fetch the clip first and play its bytes or a Blob.
 */
async function playCase({ src, bytesFrom, blobFrom, storage, headers, decoder, seekTo, playMs }) {
  const { createAlphaVideo } = await import('/dist/index.mjs');
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  if (bytesFrom) src = new Uint8Array(await (await fetch(bytesFrom)).arrayBuffer());
  if (blobFrom) src = await (await fetch(blobFrom)).blob();
  const container = document.createElement('div');
  document.body.append(container);
  let error = null;
  let code = null;
  const player = createAlphaVideo(container, {
    src,
    storage,
    headers,
    decoder,
    autoPlay: false,
    loop: false,
    onError: (e) => {
      code = e.code;
      error = `${e.code}: ${e.message}`;
    },
  });
  try {
    const start = performance.now();
    const loaded = await player.loaded.catch(() => null);
    if (!loaded) return { error, code };
    const loadMs = Math.round(performance.now() - start);
    const drawn = () => {
      const element = player.element;
      const canvas = document.createElement('canvas');
      canvas.width = element.videoWidth || element.width;
      canvas.height = element.videoHeight || element.height;
      if (!canvas.width) return false;
      const ctx = canvas.getContext('2d', { willReadFrequently: true });
      ctx.drawImage(player.element, 0, 0);
      const pixels = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
      let opaque = 0;
      for (let i = 3; i < pixels.length; i += 4) if (pixels[i] > 16) opaque++;
      return opaque > 0;
    };
    if (seekTo !== undefined) player.seekToTime(seekTo);
    let shown = false;
    for (let i = 0; i < 100 && !shown && !error; i++) {
      await sleep(50);
      shown = drawn();
    }
    if (playMs) {
      player.setPlaybackSpeed(10);
      const ended = new Promise((resolve) => {
        player.play();
        setTimeout(resolve, playMs);
      });
      await ended;
    }
    return { error, decoder: loaded.decoder, loadMs, shown };
  } finally {
    // Stored blocks are written in the background
    await sleep(300);
    player.destroy();
    container.remove();
  }
}

const wanted = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const ranged = await serve(true);
const plain = await serve(false);
let failures = 0;
const report = (ok, label, detail) => {
  if (!ok) failures++;
  console.log(`  ${ok ? '✓' : '✗'} ${label}: ${detail}`);
};

console.log(`clip: ${SIZE} bytes`);
for (const [name, type] of Object.entries({ chromium, firefox, webkit })) {
  if (wanted.length && !wanted.includes(name)) continue;
  const browser = await type.launch();
  const page = await browser.newPage();
  page.on('pageerror', (e) => console.log(`  [${name}] page error: ${e.message}`));
  console.log(`\n${name} ${browser.version()}`);

  for (const { server, label } of [
    { server: ranged, label: 'range requests' },
    { server: plain, label: 'no range requests' },
  ]) {
    await page.goto(`${server.origin}/test/browsers/page.html`);
    await page.waitForFunction(() => window.ready);
    const query = `?${name}-${label.replace(/ /g, '-')}`;
    const requests = () => server.log.filter((r) => r.query === query);

    const r = await page.evaluate(playCase, { src: `/stream.webm${query}`, storage: 'none', seekTo: 9 });
    const far = requests().filter((q) => q.start >= SIZE / 2);
    const starts = requests().map((q) => q.start).join(', ');
    if (server === ranged) {
      report(!r.error && r.shown && far.length > 0, `seek to 9 s, ${label}`, r.error ?? `${r.decoder}, load ${r.loadMs} ms, requests from ${starts}`);
    } else {
      report(!r.error && r.shown, `seek to 9 s, ${label}`, r.error ?? `${r.decoder}, load ${r.loadMs} ms, ${requests().length} requests`);
    }
  }

  // Played to the end once, a stored clip plays again without a single request
  await page.goto(`${ranged.origin}/test/browsers/page.html`);
  await page.waitForFunction(() => window.ready);
  const query = `?${name}-stored`;
  const requests = () => ranged.log.filter((r) => r.query === query).length;
  const first = await page.evaluate(playCase, { src: `/stream.webm${query}`, storage: 'auto', playMs: 1500 });
  const firstRequests = requests();
  const again = await page.evaluate(playCase, { src: `/stream.webm${query}`, storage: 'auto', seekTo: 5 });
  const againRequests = requests() - firstRequests;
  report(
    !first.error && !again.error && again.shown && againRequests === 0,
    'replay from the store',
    first.error ?? again.error ?? `${firstRequests} requests, then ${againRequests}; load ${first.loadMs} ms, then ${again.loadMs} ms`,
  );

  // Without a worker: a Blob plays in <video> as itself; bytes go to the canvas engines, and
  // fail with NEEDS_SERVICE_WORKER when <video> is asked for
  const alpha = (t) => `/alpha.webm?${name}-${t}`;
  const blob = await page.evaluate(playCase, { blobFrom: alpha('blob'), decoder: 'browser' });
  report(!blob.error && blob.shown, 'a Blob in <video> without a worker', blob.error ?? `${blob.decoder}, load ${blob.loadMs} ms`);
  const bytes = await page.evaluate(playCase, { bytesFrom: alpha('bytes'), decoder: 'browser' });
  report(bytes.code === 'NEEDS_SERVICE_WORKER', "bytes in <video> without a worker: NEEDS_SERVICE_WORKER", bytes.error ?? `${bytes.decoder}`);
  const canvas = await page.evaluate(playCase, { bytesFrom: alpha('bytes-auto') });
  report(!canvas.error && canvas.shown && canvas.decoder !== 'browser', 'bytes without a worker play on a canvas', canvas.error ?? canvas.decoder);

  // With the range-source worker controlling the page (its own context, so the cases above have none)
  const context = await browser.newContext();
  const swPage = await context.newPage();
  swPage.on('pageerror', (e) => console.log(`  [${name}] page error: ${e.message}`));
  await swPage.goto(`${ranged.origin}/test/browsers/page.html`);
  await swPage.waitForFunction(() => window.ready);
  const controlled = await swPage.evaluate(async () => {
    const { registerRangeSourceWorker, rangeSourceWorker } = await import('/dist/index.mjs');
    await registerRangeSourceWorker('/range-source-sw.js', { scope: '/' });
    return rangeSourceWorker();
  });
  report(controlled, 'the worker controls the page', String(controlled));
  if (controlled) {
    // <video> keeps alpha in Chromium and Firefox; WebKit plays alpha on a canvas
    const browserDecoder = name !== 'webkit';
    const asExpected = (r) => !r.error && r.shown && (r.decoder === 'browser') === browserDecoder;
    const log = (t) => ranged.log.filter((r) => r.query === `?${name}-${t}`);

    const first = await swPage.evaluate(playCase, { src: alpha('sw-stored'), storage: 'auto', playMs: 1500 });
    const firstStarts = log('sw-stored').map((r) => r.start);
    const again = await swPage.evaluate(playCase, { src: alpha('sw-stored'), storage: 'auto', seekTo: 5 });
    const againRequests = log('sw-stored').length - firstStarts.length;
    // The worker takes the head the page stored, so nothing is fetched twice
    const fromZero = firstStarts.filter((s) => s === 0).length;
    report(
      asExpected(first) && asExpected(again) && againRequests === 0 && fromZero === 1,
      'through the worker, then from the store',
      first.error ?? again.error ?? `${first.decoder}, requests from ${firstStarts.join(', ')}, then ${againRequests}`,
    );

    const auth = await swPage.evaluate(playCase, { src: alpha('sw-headers'), headers: { 'x-token': 'secret' } });
    const tokens = log('sw-headers').map((r) => r.token);
    report(
      asExpected(auth) && tokens.length > 0 && tokens.every((t) => t === 'secret'),
      'headers through the worker',
      auth.error ?? `${auth.decoder}, ${tokens.length} requests with the token`,
    );

    const held = await swPage.evaluate(playCase, { bytesFrom: alpha('sw-bytes'), decoder: 'browser' });
    report(!held.error && held.shown, 'bytes in <video> through the worker', held.error ?? `${held.decoder}, load ${held.loadMs} ms`);
  }
  await context.close();
  await browser.close();
}

ranged.server.close();
plain.server.close();
console.log(failures ? `\n${failures} failed` : '\nall passed');
process.exit(failures ? 1 : 0);
