#include "VpxDecoderPlugin.h"

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include <algorithm>
#include <cstdio>
#include <new>

namespace {

// libvpx keeps no frames back: each packet's frame (a superframe's shown one) is ready after it.
struct VpxDecoder {
    vpx_codec_ctx_t ctx = {};
    vpx_codec_iter_t iter = nullptr;
    int64_t ptsUs = 0;
};

// The libvpx build is 8-bit only
int supportsStream(const AlphaVideoStreamInfo* stream) {
    return stream->codec == ALPHA_VIDEO_CODEC_VP9 && stream->bitDepth <= 8;
}

void* openDecoder(const AlphaVideoStreamInfo*, int threads, char* error, size_t errorSize) {
    auto* decoder = new (std::nothrow) VpxDecoder();
    if (!decoder) {
        std::snprintf(error, errorSize, "libvpx: out of memory");
        return nullptr;
    }
    vpx_codec_dec_cfg_t cfg = {};
    cfg.threads = static_cast<unsigned>(std::clamp(threads, 1, 16));
    if (vpx_codec_dec_init(&decoder->ctx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) {
        std::snprintf(error, errorSize, "libvpx: %s", vpx_codec_error(&decoder->ctx));
        delete decoder;
        return nullptr;
    }
    // Rows of a frame decode in parallel, not only its tile columns, which narrow frames lack
    vpx_codec_control(&decoder->ctx, VP9D_SET_ROW_MT, 1);
    return decoder;
}

int sendPacket(void* opaque, const uint8_t* data, int size, int64_t ptsUs) {
    auto* decoder = static_cast<VpxDecoder*>(opaque);
    decoder->iter = nullptr;
    decoder->ptsUs = ptsUs;
    if (size <= 0) return 1;
    return vpx_codec_decode(&decoder->ctx, data, static_cast<unsigned>(size), nullptr, 0) == VPX_CODEC_OK;
}

int receivePicture(void* opaque, AlphaVideoPicture* out) {
    auto* decoder = static_cast<VpxDecoder*>(opaque);
    const vpx_image_t* img = vpx_codec_get_frame(&decoder->ctx, &decoder->iter);
    if (!img) return 0;
    for (int i = 0; i < 3; i++) {
        out->planes[i] = img->planes[i];
        out->strides[i] = img->stride[i];
    }
    out->width = static_cast<int>(img->d_w);
    out->height = static_cast<int>(img->d_h);
    out->chromaShiftX = static_cast<int>(img->x_chroma_shift);
    out->chromaShiftY = static_cast<int>(img->y_chroma_shift);
    out->bitDepth = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? static_cast<int>(img->bit_depth) : 8;
    out->colorRange = img->range == VPX_CR_FULL_RANGE ? 2 : 1;
    out->rgb = img->cs == VPX_CS_SRGB;
    out->monochrome = 0;
    out->ptsUs = decoder->ptsUs;
    return 1;
}

void drainDecoder(void*) {}

void flushDecoder(void* opaque) {
    static_cast<VpxDecoder*>(opaque)->iter = nullptr;
}

void closeDecoder(void* opaque) {
    auto* decoder = static_cast<VpxDecoder*>(opaque);
    vpx_codec_destroy(&decoder->ctx);
    delete decoder;
}

} // namespace

const AlphaVideoDecoderPlugin* const* alpha_video_vpx_plugins(int* count) {
    static const AlphaVideoDecoderPlugin vp9 = {
        ALPHA_VIDEO_DECODER_ABI, sizeof(AlphaVideoDecoderPlugin), "libvpx", vpx_codec_version_str(),
        ALPHA_VIDEO_CODEC_VP9, supportsStream, openDecoder, sendPacket, receivePicture, drainDecoder,
        flushDecoder, closeDecoder,
    };
    static const AlphaVideoDecoderPlugin* const plugins[] = {&vp9};
    *count = 1;
    return plugins;
}
