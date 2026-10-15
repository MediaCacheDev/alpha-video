// The WebAssembly API of alpha-video-web's demuxer: AlphaDemuxer (FFmpeg's Matroska demuxer, next
// to this file) and cpp/YuvToRgb over a file held in WebAssembly memory. Plain C exports;
// src/wasm/demuxer.ts wraps them.

#include "AlphaDemuxer.h"
#include "YuvToRgb.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstring>
#include <memory>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace {

constexpr int kBufferSize = 32768;

struct Memory {
    const uint8_t* data;
    int64_t size;
    int64_t pos = 0;
};

int readMemory(void* opaque, uint8_t* buf, int bufSize) {
    auto* m = static_cast<Memory*>(opaque);
    const int64_t remaining = m->size - m->pos;
    if (remaining <= 0) return AVERROR_EOF;
    const int n = static_cast<int>(std::min<int64_t>(bufSize, remaining));
    std::memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}

int64_t seekMemory(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<Memory*>(opaque);
    if (whence & AVSEEK_SIZE) return m->size;
    int64_t target;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = m->pos + offset; break;
        case SEEK_END: target = m->size + offset; break;
        default: return AVERROR(EINVAL);
    }
    if (target < 0 || target > m->size) return AVERROR(EINVAL);
    m->pos = target;
    return target;
}

// The file's bytes, the I/O reading them and the demuxer, freed in reverse order.
struct Handle {
    Memory memory;
    AVIOContext* io = nullptr;
    std::unique_ptr<webm::AlphaDemuxer> demuxer;
    webm::AlphaDemuxer::Packet packet;

    ~Handle() {
        demuxer.reset();
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

AVIOContext* openMemoryIO(Memory* memory) {
    auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (!buffer) return nullptr;
    AVIOContext* io = avio_alloc_context(buffer, kBufferSize, 0, memory, readMemory, nullptr, seekMemory);
    if (!io) av_free(buffer);
    return io;
}

void writeStream(const webm::AlphaDemuxer::StreamInfo& s, double* out) {
    out[0] = s.codec;
    out[1] = s.profile;
    out[2] = s.bitDepth;
    out[3] = s.colorRange;
    out[4] = s.colorSpace;
    out[5] = s.colorPrimaries;
    out[6] = s.colorTrc;
    out[7] = static_cast<double>(s.codecPrivate.size());
}

} // namespace

extern "C" {

/** Opens `size` bytes at `data`, which must stay allocated until avw_close. Returns a handle, or 0. */
EMSCRIPTEN_KEEPALIVE Handle* avw_open(const uint8_t* data, int size) {
    auto handle = std::make_unique<Handle>();
    handle->memory = {data, size};
    handle->io = openMemoryIO(&handle->memory);
    if (!handle->io) return nullptr;
    handle->demuxer = webm::AlphaDemuxer::open(handle->io);
    if (!handle->demuxer) return nullptr;
    return handle.release();
}

EMSCRIPTEN_KEEPALIVE void avw_close(Handle* handle) {
    delete handle;
}

/**
 * Writes 24 numbers to `out`: width, height, totalFrames, frameRate, durationUs, hasAlpha,
 * alphaInBlockAdditional, (reserved), then the colour and the alpha stream, 8 each: codec (0 VP9,
 * 1 AV1), profile, bitDepth, AVColorRange, AVColorSpace, AVColorPrimaries, AVColorTransfer,
 * codecPrivate size.
 */
EMSCRIPTEN_KEEPALIVE void avw_info(Handle* handle, double* out) {
    const webm::AlphaDemuxer::Info& info = handle->demuxer->info();
    out[0] = info.width;
    out[1] = info.height;
    out[2] = info.totalFrames;
    out[3] = info.frameRate;
    out[4] = static_cast<double>(info.durationUs);
    out[5] = info.hasAlpha;
    out[6] = info.alphaInBlockAdditional;
    out[7] = 0;
    writeStream(info.color, out + 8);
    writeStream(info.alpha, out + 16);
}

/** The codecPrivate bytes of stream 0 (colour) or 1 (alpha); their size is in avw_info. */
EMSCRIPTEN_KEEPALIVE const uint8_t* avw_codec_private(Handle* handle, int stream) {
    const webm::AlphaDemuxer::Info& info = handle->demuxer->info();
    const std::vector<uint8_t>& bytes = stream == 0 ? info.color.codecPrivate : info.alpha.codecPrivate;
    return bytes.empty() ? nullptr : bytes.data();
}

/**
 * Reads the next packet into 6 numbers at `out`: stream (0 colour, 1 alpha), data pointer,
 * size, ptsUs, frameIndex, key. The data stays valid until the next read, seek or close.
 * Returns 0 at the end of the file.
 */
EMSCRIPTEN_KEEPALIVE int avw_read(Handle* handle, double* out) {
    webm::AlphaDemuxer::Packet& p = handle->packet;
    if (!handle->demuxer->readPacket(&p)) return 0;
    out[0] = p.stream;
    out[1] = static_cast<double>(reinterpret_cast<uintptr_t>(p.data));
    out[2] = p.size;
    out[3] = static_cast<double>(p.ptsUs);
    out[4] = p.frameIndex;
    out[5] = p.key;
    return 1;
}

EMSCRIPTEN_KEEPALIVE void avw_seek_start(Handle* handle) {
    handle->demuxer->seekToStart();
}

EMSCRIPTEN_KEEPALIVE void avw_seek_us(Handle* handle, double timeUs) {
    handle->demuxer->seekToTimeUs(static_cast<int64_t>(timeUs));
}

/**
 * AlphaDemuxer::colorAlphaTracks of `size` bytes at `data`: up to 8 ints at `out`, (codec,
 * profile) pairs. Returns how many ints were written.
 */
EMSCRIPTEN_KEEPALIVE int avw_color_alpha_tracks(const uint8_t* data, int size, int* out) {
    Memory memory{data, size};
    AVIOContext* io = openMemoryIO(&memory);
    if (!io) return 0;
    std::vector<int> tracks = webm::AlphaDemuxer::colorAlphaTracks(io);
    av_freep(&io->buffer);
    avio_context_free(&io);
    const int n = static_cast<int>(std::min<size_t>(tracks.size(), 8));
    std::copy(tracks.begin(), tracks.begin() + n, out);
    return n;
}

/** webm::yuvToRgbFor: 16 floats (column-major mat4) at `out`. */
EMSCRIPTEN_KEEPALIVE void avw_yuv_to_rgb(int colorRange, int colorSpace, int width, int height, float* out) {
    webm::YuvToRgb m = webm::yuvToRgbFor(colorRange, colorSpace, width, height);
    std::memcpy(out, m.m, sizeof(m.m));
}

} // extern "C"
