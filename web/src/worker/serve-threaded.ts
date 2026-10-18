import type { CreateDecoderModule } from '../wasm/decoder-module.mjs';
import type { WorkerScope } from './protocol';
import { serve } from './serve';

/**
 * Serves a threaded decoder module, for cross-origin isolated pages. Its pthreads are Workers
 * running the same script, which start the module themselves instead of serving.
 */
export function serveThreaded(createModule: CreateDecoderModule): void {
  const scope = self as unknown as WorkerScope;
  if (scope.name.startsWith('em-pthread')) return;
  // The decoder's threads, plus one spare: the pool can't grow once decoding blocks
  serve((threads) => createModule({ mainScriptUrlOrBlob: scope.location.href, pthreadPoolSize: threads + 1 }));
}
