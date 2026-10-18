// Types for demuxer.mjs, which scripts/build_wasm.sh generates from web/wasm/demuxer_bindings.cpp

export interface DemuxerModule {
  readonly HEAPU8: Uint8Array;
  readonly HEAP32: Int32Array;
  readonly HEAPF32: Float32Array;
  readonly HEAPF64: Float64Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _avw_create(source: number, size: number): number;
  _avw_close(handle: number): void;
  _avw_try_open(handle: number): number;
  _avw_info(handle: number, out: number): void;
  _avw_codec_private(handle: number, stream: number): number;
  _avw_read(handle: number, out: number): number;
  _avw_failed(handle: number): number;
  _avw_seek_start(handle: number): void;
  _avw_seek_us(handle: number, timeUs: number): number;
  _avw_yuv_to_rgb(colorRange: number, colorSpace: number, width: number, height: number, out: number): void;
}

export default function createDemuxerModule(options?: object): Promise<DemuxerModule>;
