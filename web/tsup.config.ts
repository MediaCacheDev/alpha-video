import { defineConfig } from 'tsup';
import { inlineWorker } from './scripts/inline-worker.mjs';

export default defineConfig({
  entry: { index: 'src/index.ts', react: 'src/react.tsx' },
  format: ['esm', 'cjs'],
  dts: { resolve: ['alpha-video-source'] },
  clean: true,
  // Bundled until alpha-video-source is published, so the tarball installs on its own
  noExternal: ['alpha-video-source'],
  // ESM keeps the WebAssembly modules in their own chunks, loaded only when <video> can't play a file
  splitting: true,
  target: 'es2020',
  external: ['react'],
  esbuildPlugins: [inlineWorker],
});
