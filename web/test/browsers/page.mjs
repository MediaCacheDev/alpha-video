// Runs one case in the page for run.mjs: loads a test card, seeks to frames, measures them.
import { createAlphaVideo, videoElementKeepsWebMAlpha } from '/dist/index.mjs';

// Pixels of a WebGL canvas can only be read back after compositing when they're preserved
const getContext = HTMLCanvasElement.prototype.getContext;
HTMLCanvasElement.prototype.getContext = function (type, attributes) {
  if (type === 'webgl2') attributes = { ...attributes, preserveDrawingBuffer: true };
  return getContext.call(this, type, attributes);
};

const CODE_BITS = 12;

/** Mean of each channel over the middle half of `r`, like planeMean in the C++ host tests. */
function mean(pixels, width, r) {
  const x0 = r.x + (r.w >> 2), x1 = Math.max(x0 + 1, r.x + ((3 * r.w) >> 2));
  const y0 = r.y + (r.h >> 2), y1 = Math.max(y0 + 1, r.y + ((3 * r.h) >> 2));
  const sum = [0, 0, 0, 0];
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      for (let c = 0; c < 4; c++) sum[c] += pixels[(y * width + x) * 4 + c];
    }
  }
  return sum.map((s) => s / ((x1 - x0) * (y1 - y0)));
}

function measure(element, layout) {
  const canvas = document.createElement('canvas');
  canvas.width = layout.width;
  canvas.height = layout.height;
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.drawImage(element, 0, 0, layout.width, layout.height);
  const pixels = ctx.getImageData(0, 0, layout.width, layout.height).data;
  let colour = 0, inverted = 0, alpha = 0;
  for (let bit = 0; bit < CODE_BITS; bit++) {
    colour = (colour << 1) | (mean(pixels, layout.width, layout.codeCells[bit])[0] > 127.5);
    inverted = (inverted << 1) | (mean(pixels, layout.width, layout.invertedCells[bit])[0] > 127.5);
    alpha = (alpha << 1) | (mean(pixels, layout.width, layout.alphaCodeCells[bit])[3] > 127.5);
  }
  return {
    colourCode: (colour ^ inverted) === (1 << CODE_BITS) - 1 ? colour : -1,
    alphaCode: alpha,
    ramp: layout.alphaRamp.map((p) => ({ name: p.name, expected: p.rgba[3], alpha: mean(pixels, layout.width, p.rect)[3] })),
    colours: layout.colours.map((p) => ({ name: p.name, expected: p.rgba.slice(0, 3), rgb: mean(pixels, layout.width, p.rect).slice(0, 3) })),
  };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitForFrame(player, layout, frame, timeoutMs = 8000) {
  const start = performance.now();
  let last = null;
  while (performance.now() - start < timeoutMs) {
    if (player.element) {
      last = measure(player.element, layout);
      if (last.colourCode === frame) return { ...last, ms: Math.round(performance.now() - start) };
    }
    await sleep(40);
  }
  return { ...last, timedOut: true };
}

window.probe = () => videoElementKeepsWebMAlpha();

window.runCase = async ({ src, layoutUrl, decoder, frames }) => {
  const layout = await (await fetch(layoutUrl)).json();
  const container = document.createElement('div');
  container.style.cssText = `width:${layout.width}px;height:${layout.height}px`;
  document.body.append(container);
  const started = performance.now();
  const player = createAlphaVideo(container, { src, decoder, autoPlay: false, loop: false });
  try {
    const load = await player.loaded.catch((e) => ({ error: `${e.code}: ${e.message}` }));
    if (load.error) return { error: load.error };
    const loadMs = Math.round(performance.now() - started);
    const results = [];
    for (const frame of frames) {
      player.seekToFrame(frame);
      results.push({ frame, ...(await waitForFrame(player, layout, frame)) });
    }
    return { decoder: load.decoder, element: player.element?.tagName.toLowerCase(), loadMs, results };
  } finally {
    player.destroy();
    container.remove();
  }
};

/** Plays from the start and reports the frame codes seen for `ms`. */
window.playCase = async ({ src, layoutUrl, decoder, ms, loop }) => {
  const layout = await (await fetch(layoutUrl)).json();
  const container = document.createElement('div');
  document.body.append(container);
  let ended = false;
  const player = createAlphaVideo(container, { src, decoder, autoPlay: true, loop, onEnd: () => (ended = true) });
  try {
    const load = await player.loaded.catch((e) => ({ error: `${e.code}: ${e.message}` }));
    if (load.error) return { error: load.error };
    const seen = [];
    const mismatches = [];
    const start = performance.now();
    while (performance.now() - start < ms) {
      await new Promise((r) => requestAnimationFrame(r));
      if (!player.element) continue;
      const m = measure(player.element, layout);
      if (m.colourCode >= 0 && seen[seen.length - 1] !== m.colourCode) seen.push(m.colourCode);
      if (m.colourCode >= 0 && m.alphaCode !== m.colourCode) mismatches.push(`${m.colourCode}/${m.alphaCode}`);
    }
    return { decoder: load.decoder, seen, mismatches, ended };
  } finally {
    player.destroy();
    container.remove();
  }
};

window.ready = true;
