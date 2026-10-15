// Types for software.mjs and software-mt.mjs, which scripts/build_wasm.sh generates from
// web/wasm/decoder_bindings.cpp

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
  /** software-mt.mjs: the script its pthread Workers load, the one that imported it */
  mainScriptUrlOrBlob?: string;
  /** software-mt.mjs: pthread Workers started with the module, enough for every decoder thread */
  pthreadPoolSize?: number;
}

export default function createModule(options?: DecoderModuleOptions): Promise<DecoderModule>;
