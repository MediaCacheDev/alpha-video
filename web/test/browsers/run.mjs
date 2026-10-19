// Plays the repo's test cards in Chromium, Firefox and WebKit with each engine, and checks frame
// codes after seeks, the alpha ramp and the colour swatches against the C++ host tests' tolerance.
//
//   npm run build && npm run test:browsers [-- chromium firefox webkit] [--isolated]
//
// The page is served from http://localhost: WebCodecs needs a secure context. With --isolated
// it's served cross-origin isolated (COOP and COEP headers), so the software engine's decoders
// run threads.
import { createReadStream, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium, firefox, webkit } from 'playwright';

const WEB = fileURLToPath(new URL('../..', import.meta.url));
const FIXTURES = join(WEB, '../tests/fixtures');
const TOLERANCE = 6;
const CARDS = ['card_256x256', 'card_383x217'];
const FILES = ['yuva.webm', 'dual_vp9.webm', 'dual_vp9_p0.webm', 'dual_av1.webm', 'dual_av1_10bit.webm', 'yuva_vp8.webm', 'dual_vp8.webm'];
const DECODERS = ['auto', 'webcodecs', 'software'];
const SEEK_FRAMES = [0, 37, 5];
// A forced engine reports what it can't decode rather than falling back: VP9 profile 1 alpha
// (dual_vp9.webm) has no WebCodecs decoder in Safari, and Firefox converts it as if it were YUV
const EXPECTED_UNSUPPORTED = {
  firefox: ['dual_vp9.webm [webcodecs]'],
  webkit: ['dual_vp9.webm [webcodecs]'],
};

const TYPES = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.json': 'application/json', '.webm': 'video/webm' };
const ISOLATED = process.argv.includes('--isolated');
const ISOLATION_HEADERS = ISOLATED ? { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' } : {};

function serve() {
  const server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const file = path.startsWith('/fixtures/') ? join(FIXTURES, normalize(path.slice(10))) : join(WEB, normalize(path));
    let size;
    try {
      size = statSync(file).size;
    } catch {
      res.writeHead(404).end();
      return;
    }
    const headers = {
      'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream',
      'Accept-Ranges': 'bytes',
      'Cache-Control': 'no-store',
      ...ISOLATION_HEADERS,
    };
    const range = /bytes=(\d*)-(\d*)/.exec(req.headers.range ?? '');
    if (range) {
      const start = range[1] ? Number(range[1]) : size - Number(range[2]);
      const end = range[1] && range[2] ? Math.min(Number(range[2]), size - 1) : size - 1;
      res.writeHead(206, { ...headers, 'Content-Range': `bytes ${start}-${end}/${size}`, 'Content-Length': end - start + 1 });
      createReadStream(file, { start, end }).pipe(res);
    } else {
      res.writeHead(200, { ...headers, 'Content-Length': size });
      createReadStream(file).pipe(res);
    }
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

function check(result, layoutLabel) {
  const problems = [];
  if (result.error) return [result.error];
  for (const r of result.results) {
    if (r.timedOut) problems.push(`frame ${r.frame}: timed out showing code ${r.colourCode}`);
    if (r.alphaCode !== r.frame) problems.push(`frame ${r.frame}: alpha code ${r.alphaCode}`);
  }
  const first = result.results[0];
  if (first && !first.timedOut) {
    for (const p of first.ramp) {
      if (Math.abs(p.alpha - p.expected) > TOLERANCE) problems.push(`alpha ${p.name}: ${p.alpha.toFixed(1)}`);
    }
    for (const p of first.colours) {
      const worst = Math.max(...p.rgb.map((v, i) => Math.abs(v - p.expected[i])));
      if (worst > TOLERANCE) problems.push(`colour ${p.name}: ${p.rgb.map((v) => v.toFixed(0)).join(',')}`);
    }
  }
  return problems;
}

function worstErrors(result) {
  const first = result.results?.[0];
  if (!first?.ramp) return '';
  const alpha = Math.max(...first.ramp.map((p) => Math.abs(p.alpha - p.expected)));
  const colour = Math.max(...first.colours.flatMap((p) => p.rgb.map((v, i) => Math.abs(v - p.expected[i]))));
  return `alpha ±${alpha.toFixed(1)} colour ±${colour.toFixed(1)}`;
}

const server = await serve();
const origin = `http://localhost:${server.address().port}`;
const wanted = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const engines = { chromium, firefox, webkit };
let failures = 0;

for (const [name, type] of Object.entries(engines)) {
  if (wanted.length && !wanted.includes(name)) continue;
  const browser = await type.launch();
  const page = await browser.newPage();
  page.on('pageerror', (e) => console.log(`  [${name}] page error: ${e.message}`));
  page.on('console', (m) => {
    if (m.type() === 'warning' || m.type() === 'error') console.log(`  [${name}] ${m.type()}: ${m.text()}`);
  });
  await page.goto(`${origin}/test/browsers/page.html`);
  await page.waitForFunction(() => window.ready);
  const keeps = await page.evaluate(() => window.probe());
  const isolated = await page.evaluate(() => crossOriginIsolated);
  console.log(`\n${name} ${browser.version()}: <video> keeps WebM alpha: ${keeps}, cross-origin isolated: ${isolated}`);
  if (isolated !== ISOLATED) {
    failures++;
    console.log(`  ✗ the page should${ISOLATED ? '' : ' not'} be cross-origin isolated`);
  }

  for (const card of CARDS) {
    for (const file of FILES) {
      for (const decoder of DECODERS) {
        const label = `${card}/${file} [${decoder}]`;
        const result = await page.evaluate((c) => window.runCase(c), {
          src: `/fixtures/${card}/${file}`,
          layoutUrl: `/fixtures/${card}/layout.json`,
          decoder,
          frames: SEEK_FRAMES,
        });
        const expectUnsupported = (EXPECTED_UNSUPPORTED[name] ?? []).includes(`${file} [${decoder}]`);
        if (expectUnsupported && result.error?.startsWith('DECODER_UNSUPPORTED')) {
          console.log(`  – ${label} unsupported, as expected`);
          continue;
        }
        const problems = check(result, label);
        if (problems.length) failures++;
        const what = result.error ? '' : `${result.decoder}, load ${result.loadMs} ms, ${worstErrors(result)}`;
        console.log(`  ${problems.length ? '✗' : '✓'} ${label} ${what}`);
        for (const p of problems) console.log(`      ${p}`);
      }
    }
  }

  // Playback: frames advance, alpha stays with its colour frame, and the clip loops
  const playback = [
    ['yuva.webm', 'webcodecs'], ['dual_av1.webm', 'webcodecs'], ['yuva_vp8.webm', 'webcodecs'],
    ['dual_vp9.webm', 'software'], ['dual_av1.webm', 'software'], ['dual_vp8.webm', 'software'],
  ];
  for (const [file, decoder] of playback) {
    const result = await page.evaluate((c) => window.playCase(c), {
      src: `/fixtures/card_256x256/${file}`,
      layoutUrl: '/fixtures/card_256x256/layout.json',
      decoder,
      ms: 3000,
      loop: true,
    });
    const looped = result.seen && result.seen.some((f, i) => i > 0 && f < result.seen[i - 1]);
    const ok = !result.error && result.seen.length > 30 && looped && !result.mismatches.length;
    if (!ok) failures++;
    console.log(
      `  ${ok ? '✓' : '✗'} play ${file} [${decoder}] ${result.error ?? `${result.seen.length} frames seen, looped: ${looped}, alpha mismatches: ${result.mismatches.length}`}`,
    );
  }
  await browser.close();
}

server.close();
console.log(failures ? `\n${failures} failed` : '\nall passed');
process.exit(failures ? 1 : 0);
