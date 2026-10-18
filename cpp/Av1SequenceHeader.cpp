#include "Av1SequenceHeader.h"

namespace {

const int kObuSequenceHeader = 1;

// Reads bits most significant first; past the end it reads zeros and sets `overrun`.
struct Bits {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;  // in bits
    bool overrun = false;

    uint32_t f(int n) {
        uint32_t v = 0;
        for (; n > 0; n--, pos++) {
            int bit = 0;
            if (pos < size * 8) {
                bit = (data[pos >> 3] >> (7 - (pos & 7))) & 1;
            } else {
                overrun = true;
            }
            v = (v << 1) | static_cast<uint32_t>(bit);
        }
        return v;
    }

    bool flag() { return f(1) != 0; }

    // uvlc() (4.10.3)
    void skipUvlc() {
        int leadingZeros = 0;
        while (!flag() && !overrun) leadingZeros++;
        if (leadingZeros < 32) f(leadingZeros);
    }
};

// leb128() (4.10.5); false if it runs past `end` or over 8 bytes
bool readLeb128(const uint8_t*& p, const uint8_t* end, uint64_t* value) {
    *value = 0;
    for (int i = 0; i < 8; i++) {
        if (p >= end) return false;
        const uint8_t byte = *p++;
        *value |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) return true;
    }
    return false;
}

// color_config() (5.5.2)
void readColorConfig(Bits& b, Av1SequenceHeader* out) {
    const bool highBitdepth = b.flag();
    if (out->profile == 2 && highBitdepth) {
        out->bitDepth = b.flag() ? 12 : 10;
    } else {
        out->bitDepth = highBitdepth ? 10 : 8;
    }
    out->monochrome = out->profile == 1 ? false : b.flag();
    if (b.flag()) {  // color_description_present_flag
        out->colorPrimaries = static_cast<int>(b.f(8));
        out->transferCharacteristics = static_cast<int>(b.f(8));
        out->matrixCoefficients = static_cast<int>(b.f(8));
    }
    if (out->monochrome) {
        out->fullRange = b.flag();
        out->subsamplingX = out->subsamplingY = 1;
        return;
    }
    // sRGB: BT.709 primaries, the sRGB transfer, the identity matrix
    if (out->colorPrimaries == 1 && out->transferCharacteristics == 13 && out->matrixCoefficients == 0) {
        out->fullRange = true;
        out->subsamplingX = out->subsamplingY = 0;
    } else {
        out->fullRange = b.flag();
        if (out->profile == 0) {
            out->subsamplingX = out->subsamplingY = 1;
        } else if (out->profile == 1) {
            out->subsamplingX = out->subsamplingY = 0;
        } else if (out->bitDepth == 12) {
            out->subsamplingX = static_cast<int>(b.f(1));
            out->subsamplingY = out->subsamplingX ? static_cast<int>(b.f(1)) : 0;
        } else {
            out->subsamplingX = 1;
            out->subsamplingY = 0;
        }
        if (out->subsamplingX && out->subsamplingY) b.f(2);  // chroma_sample_position
    }
    b.f(1);  // separate_uv_delta_q
}

// sequence_header_obu() (5.5.1)
bool readSequenceHeader(Bits& b, Av1SequenceHeader* out) {
    *out = Av1SequenceHeader();
    out->profile = static_cast<int>(b.f(3));
    if (out->profile > 2) return false;
    const bool stillPicture = b.flag();
    const bool reducedStillPictureHeader = b.flag();
    if (reducedStillPictureHeader && !stillPicture) return false;
    if (reducedStillPictureHeader) {
        b.f(5);  // seq_level_idx[0]
    } else {
        bool decoderModelInfoPresent = false;
        int bufferDelayLength = 0;
        if (b.flag()) {  // timing_info_present_flag
            b.f(32);     // num_units_in_display_tick
            b.f(32);     // time_scale
            if (b.flag()) b.skipUvlc();  // equal_picture_interval, num_ticks_per_picture_minus_1
            decoderModelInfoPresent = b.flag();
            if (decoderModelInfoPresent) {
                bufferDelayLength = static_cast<int>(b.f(5)) + 1;
                b.f(32);  // num_units_in_decoding_tick
                b.f(5);   // buffer_removal_time_length_minus_1
                b.f(5);   // frame_presentation_time_length_minus_1
            }
        }
        const bool initialDisplayDelayPresent = b.flag();
        const int operatingPoints = static_cast<int>(b.f(5)) + 1;
        for (int i = 0; i < operatingPoints; i++) {
            b.f(12);  // operating_point_idc
            if (b.f(5) > 7) b.f(1);  // seq_level_idx, seq_tier
            if (decoderModelInfoPresent && b.flag()) {
                b.f(bufferDelayLength);  // decoder_buffer_delay
                b.f(bufferDelayLength);  // encoder_buffer_delay
                b.f(1);                  // low_delay_mode_flag
            }
            if (initialDisplayDelayPresent && b.flag()) b.f(4);  // initial_display_delay_minus_1
        }
    }
    const int widthBits = static_cast<int>(b.f(4)) + 1;
    const int heightBits = static_cast<int>(b.f(4)) + 1;
    out->maxWidth = static_cast<int>(b.f(widthBits)) + 1;
    out->maxHeight = static_cast<int>(b.f(heightBits)) + 1;
    if (!reducedStillPictureHeader && b.flag()) b.f(7);  // frame_id_numbers_present_flag, its lengths
    b.f(3);  // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    if (!reducedStillPictureHeader) {
        b.f(4);  // enable_interintra_compound, enable_masked_compound, enable_warped_motion, enable_dual_filter
        const bool enableOrderHint = b.flag();
        if (enableOrderHint) b.f(2);  // enable_jnt_comp, enable_ref_frame_mvs
        int forceScreenContentTools = 2;  // SELECT_SCREEN_CONTENT_TOOLS
        if (!b.flag()) forceScreenContentTools = static_cast<int>(b.f(1));
        if (forceScreenContentTools > 0 && !b.flag()) b.f(1);  // seq_choose_integer_mv, seq_force_integer_mv
        if (enableOrderHint) b.f(3);  // order_hint_bits_minus_1
    }
    b.f(3);  // enable_superres, enable_cdef, enable_restoration
    readColorConfig(b, out);
    b.f(1);  // film_grain_params_present
    return !b.overrun;
}

} // namespace

bool parseAv1SequenceHeader(const uint8_t* data, size_t size, Av1SequenceHeader* out) {
    if (!data || !out) return false;
    const uint8_t* p = data;
    const uint8_t* const end = data + size;
    while (p < end) {
        // obu_header() (5.3.2)
        const uint8_t header = *p++;
        if (header & 0x80) return false;  // obu_forbidden_bit
        const int type = (header >> 3) & 0xf;
        const bool hasExtension = header & 0x04;
        const bool hasSizeField = header & 0x02;
        if (hasExtension && p++ >= end) return false;
        uint64_t obuSize = static_cast<uint64_t>(end - p);
        if (hasSizeField && (!readLeb128(p, end, &obuSize) || obuSize > static_cast<uint64_t>(end - p))) {
            return false;
        }
        if (type == kObuSequenceHeader) {
            Bits bits{p, static_cast<size_t>(obuSize)};
            return readSequenceHeader(bits, out);
        }
        p += obuSize;
    }
    return false;
}
