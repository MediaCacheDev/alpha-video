// Types for the software decoder modules, which scripts/build_wasm.sh generates: software-vp9.mjs
// from web/wasm/decoder_bindings.cpp, software-av1.mjs from web/wasm/plugin_bindings.cpp, and
// their threaded builds software-vp9-mt.mjs and software-av1-mt.mjs

export interface DecoderModule {
  readonly wasmMemory: WebAssembly.Memory;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _avw_decoder_open(codec: number, extradata: number, size: number, threads: number): number;
  _avw_decoder_close(decoder: number): void;
  _avw_decoder_name(decoder: number): number;
  _avw_decoder_send(decoder: number, data: number, size: number, ptsUs: number): number;
  _avw_decoder_receive(decoder: number, out: number): number;
  _avw_decoder_flush(decoder: number): void;
}

export interface DecoderModuleOptions {
  /** Threaded builds: the script their pthread Workers load, the one that imported the module */
  mainScriptUrlOrBlob?: string;
  /** Threaded builds: pthread Workers started with the module, enough for every decoder thread */
  pthreadPoolSize?: number;
}

export type CreateDecoderModule = (options?: DecoderModuleOptions) => Promise<DecoderModule>;
