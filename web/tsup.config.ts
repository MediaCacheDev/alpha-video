import { defineConfig } from 'tsup';
import { inlineWorker } from './scripts/inline-worker.mjs';

export default defineConfig({
  entry: { index: 'src/index.ts', react: 'src/react.tsx' },
  format: ['esm', 'cjs'],
  dts: true,
  clean: true,
  // ESM keeps the WebAssembly modules in their own chunks, loaded only when <video> can't play a file
  splitting: true,
  target: 'es2020',
  external: ['react'],
  esbuildPlugins: [inlineWorker],
});
