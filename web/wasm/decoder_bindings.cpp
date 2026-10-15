// The software decoders of alpha-video-web: one FFmpeg decoder per stream (its VP9 decoder, dav1d
// for AV1), fed the C++ demuxer's packets in a Web Worker. Plain C exports;
// src/worker/decode-worker.ts wraps them.

#include <emscripten/emscripten.h>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

namespace {

struct Decoder {
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    ~Decoder() {
        avcodec_free_context(&ctx);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }
};

} // namespace

extern "C" {

/**
 * A decoder for `codec` (0 VP9, 1 AV1), with the stream's CodecPrivate (av1C for AV1) as
 * extradata, on `threads` threads: 1 in the single-threaded build; in the threaded one, no more
 * than the module's pthread pool holds. Returns 0 if it can't be opened.
 */
EMSCRIPTEN_KEEPALIVE Decoder* avw_decoder_open(int codec, const uint8_t* extradata, int size, int threads) {
    // FFmpeg logs to the console; its info lines (dav1d's version on every open) are noise there
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* c = codec == 1 ? avcodec_find_decoder_by_name("libdav1d") : avcodec_find_decoder(AV_CODEC_ID_VP9);
    if (!c) return nullptr;
    auto* d = new Decoder;
    d->ctx = avcodec_alloc_context3(c);
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->packet || !d->frame) {
        delete d;
        return nullptr;
    }
    d->ctx->thread_count = threads > 0 ? threads : 1;
    if (extradata && size > 0) {
        d->ctx->extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (d->ctx->extradata) {
            std::memcpy(d->ctx->extradata, extradata, size);
            d->ctx->extradata_size = size;
        }
    }
    if (avcodec_open2(d->ctx, c, nullptr) < 0) {
        delete d;
        return nullptr;
    }
    return d;
}

EMSCRIPTEN_KEEPALIVE void avw_decoder_close(Decoder* d) {
    delete d;
}

/** FFmpeg's name for the decoder: "vp9" or "libdav1d". */
EMSCRIPTEN_KEEPALIVE const char* avw_decoder_name(Decoder* d) {
    return d->ctx->codec->name;
}

/**
 * Sends `size` bytes at `data`, or with null `data` the end of the stream. Returns 1 if taken,
 * 0 if frames must be received first, negative on an error.
 */
EMSCRIPTEN_KEEPALIVE int avw_decoder_send(Decoder* d, const uint8_t* data, int size, double ptsUs) {
    int r;
    if (!data) {
        r = avcodec_send_packet(d->ctx, nullptr);
    } else {
        av_packet_unref(d->packet);
        if (av_new_packet(d->packet, size) < 0) return AVERROR(ENOMEM);
        std::memcpy(d->packet->data, data, size);
        d->packet->pts = static_cast<int64_t>(ptsUs);
        r = avcodec_send_packet(d->ctx, d->packet);
    }
    if (r == AVERROR(EAGAIN)) return 0;
    if (r == AVERROR_EOF) return 0;
    return r < 0 ? r : 1;
}

/**
 * Receives a frame into 16 numbers at `out`: width, height, bytes per sample, bit depth, chroma
 * shift x and y, RGB (planes G, B, R), AVColorRange, AVColorSpace, ptsUs, then pointers to the
 * three planes and their strides. They stay valid until the next receive, flush or close.
 * Returns 1 for a frame, 0 if the decoder needs input, -1 once drained, other negatives on errors.
 */
EMSCRIPTEN_KEEPALIVE int avw_decoder_receive(Decoder* d, double* out) {
    av_frame_unref(d->frame);
    const int r = avcodec_receive_frame(d->ctx, d->frame);
    if (r == AVERROR(EAGAIN)) return 0;
    if (r == AVERROR_EOF) return -1;
    if (r < 0) return r;
    const AVFrame* f = d->frame;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
    if (!desc) return AVERROR(EINVAL);
    out[0] = f->width;
    out[1] = f->height;
    out[2] = desc->comp[0].step;
    out[3] = desc->comp[0].depth;
    out[4] = desc->log2_chroma_w;
    out[5] = desc->log2_chroma_h;
    out[6] = (desc->flags & AV_PIX_FMT_FLAG_RGB) ? 1 : 0;
    out[7] = f->color_range;
    out[8] = f->colorspace;
    out[9] = static_cast<double>(f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts);
    for (int i = 0; i < 3; i++) {
        out[10 + i] = static_cast<double>(reinterpret_cast<uintptr_t>(f->data[i]));
        out[13 + i] = f->linesize[i];
    }
    return 1;
}

/** Drops everything in the decoder, for a seek or a loop after draining. */
EMSCRIPTEN_KEEPALIVE void avw_decoder_flush(Decoder* d) {
    avcodec_flush_buffers(d->ctx);
}

} // extern "C"
