/** The decode Worker for AV1 streams, threaded, for cross-origin isolated pages. */
import createModule from '../wasm/software-av1-mt.mjs';
import { serveThreaded } from './serve-threaded';

serveThreaded(createModule);
