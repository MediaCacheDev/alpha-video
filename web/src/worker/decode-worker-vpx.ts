/** The decode Worker for VP8 and VP9 streams, single-threaded, for any page. */
import createModule from '../wasm/software-vpx.mjs';
import { serve } from './serve';

serve(() => createModule());
