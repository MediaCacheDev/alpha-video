// The AV1 software decoder of alpha-video-web: the av1 codec module's plugin (dav1d,
// codecs/av1/Dav1dDecoderPlugin.cpp), as on Android and iOS, behind the exports of
// decoder_bindings.cpp. src/worker/serve.ts drives both.

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>

#include "Dav1dDecoderPlugin.h"

namespace {

constexpr int kColorSpaceUnspecified = 2;  // AVColorSpace: the page takes the demuxer's

struct Decoder {
    const AlphaVideoDecoderPlugin* plugin;
    void* decoder;
    AlphaVideoPicture picture;
    bool draining;
};

} // namespace

extern "C" {

/** As decoder_bindings.cpp's: 0 if `codec` isn't AV1 or the stream can't be opened. */
EMSCRIPTEN_KEEPALIVE Decoder* avw_decoder_open(int codec, const uint8_t* extradata, int size, int threads) {
    int count = 0;
    const AlphaVideoDecoderPlugin* plugin = alpha_video_av1_plugins(&count)[0];
    if (codec != plugin->codec) return nullptr;
    AlphaVideoStreamInfo stream = {};
    stream.codec = codec;
    stream.codecPrivate = extradata;
    stream.codecPrivateSize = size > 0 ? static_cast<size_t>(size) : 0;
    char error[256] = "";
    void* decoder = plugin->open(&stream, threads > 0 ? threads : 1, error, sizeof error);
    if (!decoder) {
        std::fprintf(stderr, "%s\n", error);
        return nullptr;
    }
    return new Decoder{plugin, decoder, {}, false};
}

EMSCRIPTEN_KEEPALIVE void avw_decoder_close(Decoder* d) {
    d->plugin->close(d->decoder);
    delete d;
}

/** "dav1d" */
EMSCRIPTEN_KEEPALIVE const char* avw_decoder_name(Decoder* d) {
    return d->plugin->name;
}

/** As decoder_bindings.cpp's. The plugin always takes the packet; receive until 0 before the next. */
EMSCRIPTEN_KEEPALIVE int avw_decoder_send(Decoder* d, const uint8_t* data, int size, double ptsUs) {
    if (!data) {
        d->plugin->drain(d->decoder);
        d->draining = true;
        return 1;
    }
    d->draining = false;
    return d->plugin->send(d->decoder, data, size, static_cast<int64_t>(ptsUs)) ? 1 : -1;
}

/** The 16 numbers of decoder_bindings.cpp's avw_decoder_receive; the colour space unspecified. */
EMSCRIPTEN_KEEPALIVE int avw_decoder_receive(Decoder* d, double* out) {
    if (!d->plugin->receive(d->decoder, &d->picture)) return d->draining ? -1 : 0;
    const AlphaVideoPicture& p = d->picture;
    out[0] = p.width;
    out[1] = p.height;
    out[2] = p.bitDepth > 8 ? 2 : 1;
    out[3] = p.bitDepth;
    out[4] = p.chromaShiftX;
    out[5] = p.chromaShiftY;
    out[6] = p.rgb;
    out[7] = p.colorRange;
    out[8] = kColorSpaceUnspecified;
    out[9] = static_cast<double>(p.ptsUs);
    for (int i = 0; i < 3; i++) {
        out[10 + i] = static_cast<double>(reinterpret_cast<uintptr_t>(p.planes[i]));
        out[13 + i] = p.strides[i];
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE void avw_decoder_flush(Decoder* d) {
    d->plugin->flush(d->decoder);
    d->draining = false;
}

} // extern "C"
