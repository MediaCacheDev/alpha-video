#include "VideoDecoder.h"

#include "YuvToRgb.h"

#include <dav1d/dav1d.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>

#include <mutex>

// Exported by libdav1d (src/cpu.h), but not in its public headers
extern "C" void dav1d_set_cpu_flags_mask(unsigned mask);
#endif

namespace webm {

namespace {

// dav1d takes SVE2 from HWCAP2 alone. Android's emulator on Apple M4 reports SVE2 without SVE and
// traps SVE instructions, so 10-bit AV1 crashed there; real SVE2 CPUs always have SVE.
void maskUnusableDav1dCpuFlags() {
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

// libvpx keeps no frames back: each packet's frame (a superframe's shown one) is ready after it.
class VpxDecoder : public VideoDecoder {
public:
    ~VpxDecoder() override {
        if (open_) vpx_codec_destroy(&ctx_);
    }

    bool open(int threads, std::string* error) {
        vpx_codec_dec_cfg_t cfg = {};
        cfg.threads = static_cast<unsigned>(std::clamp(threads, 1, 16));
        if (vpx_codec_dec_init(&ctx_, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) {
            *error = std::string("libvpx: ") + vpx_codec_error(&ctx_);
            return false;
        }
        open_ = true;
        // Rows of a frame decode in parallel, not only its tile columns, which narrow frames lack
        vpx_codec_control(&ctx_, VP9D_SET_ROW_MT, 1);
        return true;
    }

    bool send(const uint8_t* data, int size, int64_t ptsUs) override {
        iter_ = nullptr;
        ptsUs_ = ptsUs;
        if (size <= 0) return true;
        return vpx_codec_decode(&ctx_, data, static_cast<unsigned>(size), nullptr, 0) == VPX_CODEC_OK;
    }

    bool receive(Picture* out) override {
        const vpx_image_t* img = vpx_codec_get_frame(&ctx_, &iter_);
        if (!img) return false;
        for (int i = 0; i < 3; i++) {
            out->planes[i] = img->planes[i];
            out->strides[i] = img->stride[i];
        }
        out->width = static_cast<int>(img->d_w);
        out->height = static_cast<int>(img->d_h);
        out->chromaShiftX = static_cast<int>(img->x_chroma_shift);
        out->chromaShiftY = static_cast<int>(img->y_chroma_shift);
        out->bitDepth = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? static_cast<int>(img->bit_depth) : 8;
        out->colorRange = img->range == VPX_CR_FULL_RANGE ? kRangeFull : kRangeLimited;
        out->rgb = img->cs == VPX_CS_SRGB;
        out->monochrome = false;
        out->ptsUs = ptsUs_;
        return true;
    }

    void drain() override {}
    void flush() override { iter_ = nullptr; }
    const char* name() const override { return "libvpx"; }

private:
    vpx_codec_ctx_t ctx_ = {};
    bool open_ = false;
    vpx_codec_iter_t iter_ = nullptr;
    int64_t ptsUs_ = 0;
};

// Driven as FFmpeg's libdav1d wrapper drives it. dav1d_get_picture with no new data since the
// last call drains frames held by frame threads, so it's only called right after sending data,
// or when draining.
class Dav1dDecoder : public VideoDecoder {
public:
    ~Dav1dDecoder() override {
        dav1d_data_unref(&data_);
        dav1d_picture_unref(&picture_);
        if (ctx_) dav1d_close(&ctx_);
    }

    bool open(int threads, std::string* error) {
        maskUnusableDav1dCpuFlags();
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = std::max(threads, 0);
        settings.max_frame_delay = 0;  // dav1d's choice for the thread count
        settings.all_layers = 0;       // Only the highest spatial layer
        if (dav1d_open(&ctx_, &settings) < 0) {
            *error = "dav1d: failed to open";
            return false;
        }
        return true;
    }

    bool send(const uint8_t* data, int size, int64_t ptsUs) override {
        draining_ = false;
        dav1d_data_unref(&data_);
        if (size <= 0) return true;
        uint8_t* buffer = dav1d_data_create(&data_, static_cast<size_t>(size));
        if (!buffer) return false;
        std::memcpy(buffer, data, static_cast<size_t>(size));
        data_.m.timestamp = ptsUs;
        return true;
    }

    bool receive(Picture* out) override {
        dav1d_picture_unref(&picture_);
        // A drain can stop at a frame it drops (an invisible one) with more behind it
        int drainRetries = 1;
        for (;;) {
            if (data_.sz) {
                const int res = dav1d_send_data(ctx_, &data_);
                if (res < 0 && res != DAV1D_ERR(EAGAIN)) {
                    dav1d_data_unref(&data_);
                    return false;
                }
            } else if (!draining_) {
                return false;
            }
            const int res = dav1d_get_picture(ctx_, &picture_);
            if (res == 0) break;
            if (res != DAV1D_ERR(EAGAIN)) return false;
            if (draining_ && drainRetries-- == 0) return false;
        }

        const Dav1dPicture& p = picture_;
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
        out->colorRange = seq && seq->color_range ? kRangeFull : kRangeLimited;
        // FFmpeg's test for GBR output
        out->rgb = seq && p.p.layout == DAV1D_PIXEL_LAYOUT_I444 && seq->mtrx == DAV1D_MC_IDENTITY &&
                   seq->pri == DAV1D_COLOR_PRI_BT709 && seq->trc == DAV1D_TRC_SRGB;
        out->ptsUs = p.m.timestamp;
        return true;
    }

    void drain() override { draining_ = true; }

    void flush() override {
        dav1d_data_unref(&data_);
        dav1d_picture_unref(&picture_);
        dav1d_flush(ctx_);
        draining_ = false;
    }

    const char* name() const override { return "dav1d"; }

private:
    Dav1dContext* ctx_ = nullptr;
    Dav1dData data_ = {};
    Dav1dPicture picture_ = {};
    bool draining_ = false;
};

} // namespace

std::unique_ptr<VideoDecoder> VideoDecoder::create(AlphaDemuxer::Codec codec, int threads, std::string* error) {
    if (codec == AlphaDemuxer::AV1) {
        auto decoder = std::make_unique<Dav1dDecoder>();
        if (!decoder->open(threads, error)) return nullptr;
        return decoder;
    }
    auto decoder = std::make_unique<VpxDecoder>();
    if (!decoder->open(threads, error)) return nullptr;
    return decoder;
}

std::string decoderLibraryVersions() {
    return std::string("libvpx ") + vpx_codec_version_str() + ", dav1d " + dav1d_version();
}

} // namespace webm
