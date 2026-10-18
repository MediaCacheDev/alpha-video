/** The decode Worker for AV1 streams, single-threaded, for any page. */
import createModule from '../wasm/software-av1.mjs';
import { serve } from './serve';

serve(() => createModule());
