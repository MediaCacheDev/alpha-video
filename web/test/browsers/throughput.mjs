// How fast the software engine decodes, with nothing waiting on the clock: colour and alpha pairs
// per second, decoded in the Workers, sent to the page and handed back as soon as they arrive.
//   node test/browsers/throughput.mjs <dir with .webm files> [chromium,firefox,webkit] [--isolated]
// --isolated serves the page cross-origin isolated, so the decoders run threads.
import { build } from 'esbuild';
import { chromium, firefox, webkit } from 'playwright';
import { readdirSync, readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inlineWorker } from '../../scripts/inline-worker.mjs';

const WEB = fileURLToPath(new URL('../..', import.meta.url));
const args = process.argv.slice(2);
const isolated = args.includes('--isolated');
const [dir, only] = args.filter((a) => !a.startsWith('--'));
if (!dir) {
  console.error('usage: node test/browsers/throughput.mjs <dir> [browsers] [--isolated]');
  process.exit(1);
}
const MEDIA = resolve(dir);
const files = readdirSync(MEDIA).filter((f) => f.endsWith('.webm')).sort();

await build({
  stdin: {
    contents:
      "export { SoftwareSource } from './src/software-source'; export { WasmDemuxer } from './src/demuxer'; export { memorySource, openReader } from 'alpha-video-source';",
    resolveDir: WEB,
    loader: 'ts',
  },
  bundle: true,
  format: 'esm',
  splitting: true,
  outdir: join(WEB, 'test/browsers/build/throughput'),
  plugins: [inlineWorker],
  logLevel: 'warning',
});
const headers = isolated ? { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' } : {};
const server = createServer((req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  const file = path.startsWith('/media/') ? join(MEDIA, path.slice(7)) : join(WEB, path);
  let body;
  try {
    body = readFileSync(file);
  } catch {
    res.writeHead(404).end();
    return;
  }
  const type = file.endsWith('.js') || file.endsWith('.mjs') ? 'text/javascript' : file.endsWith('.html') ? 'text/html' : 'application/octet-stream';
  res.writeHead(200, { 'Content-Type': type, ...headers }).end(body);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://localhost:${server.address().port}`;

const types = { chromium, firefox, webkit };
for (const name of only ? only.split(',') : Object.keys(types)) {
  const browser = await types[name].launch();
  const page = await browser.newPage();
  await page.goto(`${origin}/test/browsers/page.html`);
  console.log(`\n${name} ${browser.version()}, cross-origin isolated: ${await page.evaluate(() => crossOriginIsolated)}`);
  for (const file of files) {
    const r = await page.evaluate(async (src) => {
      const { SoftwareSource, WasmDemuxer, memorySource, openReader } = await import('/test/browsers/build/throughput/stdin.js');
      const bytes = new Uint8Array(await (await fetch(src)).arrayBuffer());
      const demuxer = await WasmDemuxer.open(await openReader(memorySource(bytes)));
      let source;
      let pairs = 0;
      let start = 0;
      const result = await new Promise((resolve) => {
        const callbacks = {
          onUpdate: () => {
            // One at a time: take() hands over the latest due pair and releases the ones before it
            for (let t = source.nextTimestampUs(); t !== null; t = source.nextTimestampUs()) {
              source.release(source.take(t));
              pairs++;
            }
            if (source.finished) resolve({ ms: performance.now() - start });
          },
          onError: (e) => resolve({ error: e.message }),
        };
        SoftwareSource.create(demuxer, false, callbacks).then(
          (s) => {
            source = s;
            start = performance.now();
            s.pump();
          },
          (e) => resolve({ error: e.message }),
        );
      });
      const decoder = source?.decoderName;
      source?.close();
      return { ...result, pairs, decoder };
    }, `/media/${file}`);
    const line = r.ms ? `${r.pairs} pairs in ${(r.ms / 1000).toFixed(2)} s, ${(r.pairs / (r.ms / 1000)).toFixed(0)} pairs/s` : r.error;
    console.log(`  ${file} ${r.decoder ?? ''}: ${line}`);
  }
  await browser.close();
}
server.close();
