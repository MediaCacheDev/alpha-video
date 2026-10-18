/** The decode Worker for VP9 streams, single-threaded, for any page. */
import createModule from '../wasm/software-vp9.mjs';
import { serve } from './serve';

serve(() => createModule());
