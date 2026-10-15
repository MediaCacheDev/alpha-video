/** The decode Worker with the single-threaded decoders, for any page. */
import createModule from '../wasm/software.mjs';
import { serve } from './serve';

serve(() => createModule());
