// The WebAssembly API of alpha-video-web's demuxer: the core's AlphaDemuxer (cpp/, on libwebm's
// webm_parser) reading through JavaScript, and cpp/YuvToRgb. Plain C exports; src/demuxer.ts
// wraps them.

#include "AlphaDemuxer.h"
#include "ByteSource.h"
#include "YuvToRgb.h"

#include <emscripten/emscripten.h>

#include <cstring>
#include <memory>

// Module.readSource(source, pos, out, size), set by src/demuxer.ts: copies up to `size` bytes at
// `pos` of source `source` to `out` and returns how many, 0 at the end, -1 on an error, or -2
// (ByteSource::kPending) when they aren't in memory yet. It may not wait: JavaScript fetches
// them, then calls the demuxer again.
EM_JS(double, avw_source_read, (int source, double pos, uint8_t* out, double size), {
    return Module.readSource(source, pos, out, size);
});

namespace {

class JsSource : public webm::ByteSource {
public:
    JsSource(int id, int64_t size) : id_(id), size_(size) {}
    int64_t read(int64_t pos, uint8_t* out, int64_t size) override {
        return static_cast<int64_t>(avw_source_read(id_, static_cast<double>(pos), out, static_cast<double>(size)));
    }
    int64_t size() const override { return size_; }

private:
    int id_;
    int64_t size_;
};

struct Handle {
    std::unique_ptr<webm::AlphaDemuxer> demuxer;
    webm::AlphaDemuxer::Packet packet;
};

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

/** A demuxer reading JavaScript source `source`, of `size` bytes (-1 if unknown), not yet open: call avw_try_open. */
EMSCRIPTEN_KEEPALIVE Handle* avw_create(int source, double size) {
    auto handle = new Handle;
    handle->demuxer = webm::AlphaDemuxer::create(std::make_unique<JsSource>(source, static_cast<int64_t>(size)));
    return handle;
}

EMSCRIPTEN_KEEPALIVE void avw_close(Handle* handle) {
    delete handle;
}

/** AlphaDemuxer::tryOpen: 0 open, 1 pending (call again once the bytes are in memory), 2 not a WebM with a VP9 or AV1 track. */
EMSCRIPTEN_KEEPALIVE int avw_try_open(Handle* handle) {
    switch (handle->demuxer->tryOpen()) {
        case webm::AlphaDemuxer::Progress::kDone: return 0;
        case webm::AlphaDemuxer::Progress::kPending: return 1;
        case webm::AlphaDemuxer::Progress::kFailed: break;
    }
    return 2;
}

/**
 * Writes 24 numbers to `out`: width, height, totalFrames, frameRate, durationUs, hasAlpha,
 * alphaInBlockAdditional, startUs, then the colour and the alpha stream, 8 each: codec (0 VP8,
 * 1 VP9, 2 AV1), profile, bitDepth, colour range, matrix, primaries and transfer (H.273), codecPrivate
 * size.
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
    out[7] = static_cast<double>(info.startUs);
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
 * Returns 1 for a packet, 0 at the end of the file or when reading failed (avw_failed), 2 when
 * pending: call again once the bytes are in memory.
 */
EMSCRIPTEN_KEEPALIVE int avw_read(Handle* handle, double* out) {
    webm::AlphaDemuxer::Packet& p = handle->packet;
    if (!handle->demuxer->readPacket(&p)) return handle->demuxer->pending() ? 2 : 0;
    out[0] = p.stream;
    out[1] = static_cast<double>(reinterpret_cast<uintptr_t>(p.data));
    out[2] = p.size;
    out[3] = static_cast<double>(p.ptsUs);
    out[4] = p.frameIndex;
    out[5] = p.key;
    return 1;
}

EMSCRIPTEN_KEEPALIVE int avw_failed(Handle* handle) {
    return handle->demuxer->failed();
}

EMSCRIPTEN_KEEPALIVE void avw_seek_start(Handle* handle) {
    handle->demuxer->seekToStart();
}

/** AlphaDemuxer::seekToTimeUs. Returns 1 when pending: seek again once the bytes are in memory. */
EMSCRIPTEN_KEEPALIVE int avw_seek_us(Handle* handle, double timeUs) {
    handle->demuxer->seekToTimeUs(static_cast<int64_t>(timeUs));
    return handle->demuxer->pending();
}

/** webm::yuvToRgbFor: 16 floats (column-major mat4) at `out`. */
EMSCRIPTEN_KEEPALIVE void avw_yuv_to_rgb(int colorRange, int colorSpace, int width, int height, float* out) {
    webm::YuvToRgb m = webm::yuvToRgbFor(colorRange, colorSpace, width, height);
    std::memcpy(out, m.m, sizeof(m.m));
}

} // extern "C"
