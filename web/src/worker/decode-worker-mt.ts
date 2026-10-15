/**
 * The decode Worker with the threaded decoders, for cross-origin isolated pages. Its pthreads are
 * Workers running this same script, which start the module themselves instead of serving.
 */
import createModule from '../wasm/software-mt.mjs';
import type { WorkerScope } from './protocol';
import { serve } from './serve';

const scope = self as unknown as WorkerScope;

if (!scope.name.startsWith('em-pthread')) {
  // The decoder's threads, plus one spare: the pool can't grow once decoding blocks
  serve((threads) => createModule({ mainScriptUrlOrBlob: scope.location.href, pthreadPoolSize: threads + 1 }));
}
