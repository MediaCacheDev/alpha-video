// Which VideoFrame formats each browser's WebCodecs decoders output, per hardware preference.
//   node test/browsers/formats.mjs
import { build } from 'esbuild';
import { chromium, firefox, webkit } from 'playwright';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB = fileURLToPath(new URL('../..', import.meta.url));
await build({
  stdin: { contents: "export { WasmDemuxer } from './src/demuxer'; export { codecString } from './src/codec';", resolveDir: WEB, loader: 'ts' },
  bundle: true,
  format: 'esm',
  outfile: join(WEB, 'test/browsers/build/internals.mjs'),
  logLevel: 'warning',
});
const server = createServer((req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname;
  const file = path.startsWith('/fixtures/') ? join(WEB, '../tests/fixtures', path.slice(10)) : join(WEB, path);
  let body;
  try {
    body = readFileSync(file);
  } catch {
    res.writeHead(404).end();
    return;
  }
  const type = file.endsWith('.mjs') ? 'text/javascript' : file.endsWith('.html') ? 'text/html' : 'application/octet-stream';
  res.writeHead(200, { 'Content-Type': type }).end(body);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://localhost:${server.address().port}`;

for (const type of [chromium, firefox, webkit]) {
  const browser = await type.launch();
  const page = await browser.newPage();
  await page.goto(`${origin}/test/browsers/page.html`);
  await page.waitForFunction(() => window.ready);
  const out = await page.evaluate(async () => {
    const { WasmDemuxer, codecString } = await import('/test/browsers/build/internals.mjs');
    const results = {};
    for (const file of ['yuva.webm', 'dual_vp9.webm', 'dual_av1_10bit.webm']) {
      const bytes = new Uint8Array(await (await fetch(`/fixtures/card_256x256/${file}`)).arrayBuffer());
      const d = await WasmDemuxer.open(bytes);
      results[file] = { alphaInfo: { range: d.info.alpha.colorRange, space: d.info.alpha.colorSpace } };
      const first = [null, null];
      for (let p = d.read(); p && (!first[0] || !first[1]); p = d.read()) if (p.key && !first[p.stream]) first[p.stream] = p;
      for (const hw of ['no-preference', 'prefer-hardware', 'prefer-software']) {
        const formats = [];
        for (const s of [0, 1]) {
          const codec = codecString(s ? d.info.alpha : d.info.color, first[s].data, d.info.width, d.info.height);
          const frame = await new Promise((resolve) => {
            const dec = new VideoDecoder({ output: (f) => resolve(f), error: (e) => resolve(e.message) });
            try {
              dec.configure({ codec, codedWidth: 256, codedHeight: 256, hardwareAcceleration: hw });
              dec.decode(new EncodedVideoChunk({ type: 'key', timestamp: 0, data: first[s].data }));
              dec.flush().catch((e) => resolve(e.message));
            } catch (e) {
              resolve(e.message);
            }
          });
          if (typeof frame === 'string') formats.push(`error: ${frame}`);
          else {
            formats.push(`${frame.format} ${JSON.stringify(frame.colorSpace.toJSON())}`);
            frame.close();
          }
        }
        results[file][hw] = formats;
      }
      d.close();
    }
    return results;
  });
  console.log(`\n${type.name()} ${browser.version()}`);
  console.log(JSON.stringify(out, null, 1));
  await browser.close();
}
server.close();
