/** The decode Worker for VP8 and VP9 streams, threaded, for cross-origin isolated pages. */
import createModule from '../wasm/software-vpx-mt.mjs';
import { serveThreaded } from './serve-threaded';

serveThreaded(createModule);
