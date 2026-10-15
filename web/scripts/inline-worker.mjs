// An esbuild plugin: `import source from './decode-worker?worker'` gives the Worker's script,
// bundled with everything it imports (the WebAssembly module too) into one ES module string, to
// start from a blob: URL. So the package has no Worker file for apps or bundlers to host.
import { build } from 'esbuild';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';

export const inlineWorker = {
  name: 'inline-worker',
  setup(b) {
    b.onResolve({ filter: /\?worker$/ }, (args) => {
      const base = resolve(args.resolveDir, args.path.replace(/\?worker$/, ''));
      const path = ['', '.ts', '.js'].map((ext) => base + ext).find((p) => existsSync(p));
      if (!path) return { errors: [{ text: `No Worker script at ${base}` }] };
      return { path, namespace: 'inline-worker' };
    });
    b.onLoad({ filter: /.*/, namespace: 'inline-worker' }, async (args) => {
      const result = await build({
        entryPoints: [args.path],
        bundle: true,
        format: 'esm',
        target: 'es2020',
        minify: true,
        write: false,
        metafile: true,
        logLevel: 'silent',
      });
      return {
        contents: `export default ${JSON.stringify(result.outputFiles[0].text)};`,
        loader: 'js',
        watchFiles: Object.keys(result.metafile.inputs).map((p) => resolve(p)),
      };
    });
  },
};
