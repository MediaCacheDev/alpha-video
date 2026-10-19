// Types for software-vpx.mjs: FFmpeg's VP8 and VP9 decoders, single-threaded

import type { CreateDecoderModule } from './decoder-module.mjs';

declare const createModule: CreateDecoderModule;
export default createModule;
