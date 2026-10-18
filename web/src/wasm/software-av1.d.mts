// Types for software-av1.mjs: dav1d through the av1 codec module's plugin, single-threaded

import type { CreateDecoderModule } from './decoder-module.mjs';

declare const createModule: CreateDecoderModule;
export default createModule;
