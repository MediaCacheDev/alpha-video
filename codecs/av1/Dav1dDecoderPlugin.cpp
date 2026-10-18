#include "Dav1dDecoderPlugin.h"

#include <dav1d/dav1d.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>

#include <mutex>

// Exported by libdav1d (src/cpu.h), but not in its public headers
extern "C" void dav1d_set_cpu_flags_mask(unsigned mask);
#endif

namespace {

// dav1d takes SVE2 from HWCAP2 alone. Android's emulator on Apple M4 reports SVE2 without SVE and
// traps SVE instructions, so 10-bit AV1 crashed there; real SVE2 CPUs always have SVE.
void maskUnusableCpuFlags() {
#if defined(__aarch64__) && defined(__linux__)
    static std::once_flag once;
    std::call_once(once, [] {
        if ((getauxval(AT_HWCAP2) & HWCAP2_SVE2) && !(getauxval(AT_HWCAP) & HWCAP_SVE)) {
            constexpr unsigned kDav1dArmSve = 1u << 3, kDav1dArmSve2 = 1u << 4;  // src/arm/cpu.h
            dav1d_set_cpu_flags_mask(~(kDav1dArmSve | kDav1dArmSve2));
        }
    });
#endif
}

// Driven as FFmpeg's libdav1d wrapper drives it. dav1d_get_picture with no new data since the
// last call drains frames held by frame threads, so it's only called right after sending data,
// or when draining.
struct Dav1dDecoder {
    Dav1dContext* ctx = nullptr;
    Dav1dData data = {};
    Dav1dPicture picture = {};
    bool draining = false;
};

int supportsStream(const AlphaVideoStreamInfo* stream) {
    return stream->codec == ALPHA_VIDEO_CODEC_AV1;
}

void* openDecoder(const AlphaVideoStreamInfo*, int threads, char* error, size_t errorSize) {
    maskUnusableCpuFlags();
    auto* decoder = new (std::nothrow) Dav1dDecoder();
    if (!decoder) {
        std::snprintf(error, errorSize, "dav1d: out of memory");
        return nullptr;
    }
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = std::max(threads, 0);
    settings.max_frame_delay = 0;  // dav1d's choice for the thread count
    settings.all_layers = 0;       // Only the highest spatial layer
    if (dav1d_open(&decoder->ctx, &settings) < 0) {
        std::snprintf(error, errorSize, "dav1d: failed to open");
        delete decoder;
        return nullptr;
    }
    return decoder;
}

int sendPacket(void* opaque, const uint8_t* data, int size, int64_t ptsUs) {
    auto* decoder = static_cast<Dav1dDecoder*>(opaque);
    decoder->draining = false;
    dav1d_data_unref(&decoder->data);
    if (size <= 0) return 1;
    uint8_t* buffer = dav1d_data_create(&decoder->data, static_cast<size_t>(size));
    if (!buffer) return 0;
    std::memcpy(buffer, data, static_cast<size_t>(size));
    decoder->data.m.timestamp = ptsUs;
    return 1;
}

int receivePicture(void* opaque, AlphaVideoPicture* out) {
    auto* decoder = static_cast<Dav1dDecoder*>(opaque);
    dav1d_picture_unref(&decoder->picture);
    // A drain can stop at a frame it drops (an invisible one) with more behind it
    int drainRetries = 1;
    for (;;) {
        if (decoder->data.sz) {
            const int res = dav1d_send_data(decoder->ctx, &decoder->data);
            if (res < 0 && res != DAV1D_ERR(EAGAIN)) {
                dav1d_data_unref(&decoder->data);
                return 0;
            }
        } else if (!decoder->draining) {
            return 0;
        }
        const int res = dav1d_get_picture(decoder->ctx, &decoder->picture);
        if (res == 0) break;
        if (res != DAV1D_ERR(EAGAIN)) return 0;
        if (decoder->draining && drainRetries-- == 0) return 0;
    }

    const Dav1dPicture& p = decoder->picture;
    const Dav1dSequenceHeader* seq = p.seq_hdr;
    out->planes[0] = static_cast<const uint8_t*>(p.data[0]);
    out->planes[1] = static_cast<const uint8_t*>(p.data[1]);
    out->planes[2] = static_cast<const uint8_t*>(p.data[2]);
    out->strides[0] = static_cast<int>(p.stride[0]);
    out->strides[1] = out->strides[2] = static_cast<int>(p.stride[1]);
    out->width = p.p.w;
    out->height = p.p.h;
    out->chromaShiftX = p.p.layout == DAV1D_PIXEL_LAYOUT_I420 || p.p.layout == DAV1D_PIXEL_LAYOUT_I422;
    out->chromaShiftY = p.p.layout == DAV1D_PIXEL_LAYOUT_I420;
    out->monochrome = p.p.layout == DAV1D_PIXEL_LAYOUT_I400;
    if (out->monochrome) out->planes[1] = out->planes[2] = nullptr;
    out->bitDepth = p.p.bpc;
    out->colorRange = seq && seq->color_range ? 2 : 1;
    // FFmpeg's test for GBR output
    out->rgb = seq && p.p.layout == DAV1D_PIXEL_LAYOUT_I444 && seq->mtrx == DAV1D_MC_IDENTITY &&
               seq->pri == DAV1D_COLOR_PRI_BT709 && seq->trc == DAV1D_TRC_SRGB;
    out->ptsUs = p.m.timestamp;
    return 1;
}

void drainDecoder(void* opaque) {
    static_cast<Dav1dDecoder*>(opaque)->draining = true;
}

void flushDecoder(void* opaque) {
    auto* decoder = static_cast<Dav1dDecoder*>(opaque);
    dav1d_data_unref(&decoder->data);
    dav1d_picture_unref(&decoder->picture);
    dav1d_flush(decoder->ctx);
    decoder->draining = false;
}

void closeDecoder(void* opaque) {
    auto* decoder = static_cast<Dav1dDecoder*>(opaque);
    dav1d_data_unref(&decoder->data);
    dav1d_picture_unref(&decoder->picture);
    if (decoder->ctx) dav1d_close(&decoder->ctx);
    delete decoder;
}

} // namespace

const AlphaVideoDecoderPlugin* const* alpha_video_av1_plugins(int* count) {
    static const AlphaVideoDecoderPlugin av1 = {
        ALPHA_VIDEO_DECODER_ABI, sizeof(AlphaVideoDecoderPlugin), "dav1d", dav1d_version(),
        ALPHA_VIDEO_CODEC_AV1, supportsStream, openDecoder, sendPacket, receivePicture, drainDecoder,
        flushDecoder, closeDecoder,
    };
    static const AlphaVideoDecoderPlugin* const plugins[] = {&av1};
    *count = 1;
    return plugins;
}
