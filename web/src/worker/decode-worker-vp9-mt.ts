/** The decode Worker for VP9 streams, threaded, for cross-origin isolated pages. */
import createModule from '../wasm/software-vp9-mt.mjs';
import { serveThreaded } from './serve-threaded';

serveThreaded(createModule);
