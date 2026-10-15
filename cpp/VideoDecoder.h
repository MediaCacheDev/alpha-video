#pragma once

#include "AlphaDemuxer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace webm {

/** A decoded frame, in the decoder's own buffers: valid until the decoder's next call. */
struct Picture {
    const uint8_t* planes[3] = {};  // Y, U, V; G, B, R when rgb. U and V are null when monochrome.
    int strides[3] = {};
    int width = 0;
    int height = 0;
    int chromaShiftX = 1;  // 4:2:0 is 1, 1; 4:2:2 is 1, 0; 4:4:4 is 0, 0
    int chromaShiftY = 1;
    int bitDepth = 8;  // Deeper samples are 16 bits each
    int colorRange = 1;  // ColorRange (YuvToRgb.h)
    bool rgb = false;
    bool monochrome = false;
    int64_t ptsUs = 0;
};

/**
 * Decodes one VP9 or AV1 bitstream: libvpx for VP9, dav1d for AV1. Not thread-safe.
 *
 * send() a packet, then receive() until it returns false. Frames come out in order, perhaps
 * some packets late (dav1d's frame threads); drain() at the end of the stream lets receive()
 * return the frames still held.
 */
class VideoDecoder {
public:
    /** Null with `error` set if the decoder can't be opened. */
    static std::unique_ptr<VideoDecoder> create(AlphaDemuxer::Codec codec, int threads, std::string* error);

    virtual ~VideoDecoder() = default;

    /** False if the packet can't be decoded. */
    virtual bool send(const uint8_t* data, int size, int64_t ptsUs) = 0;
    virtual bool receive(Picture* out) = 0;
    virtual void drain() = 0;
    /** Drops the frames held and in progress, for a seek. */
    virtual void flush() = 0;

    /** "libvpx" or "dav1d" */
    virtual const char* name() const = 0;
};

/** The decoder libraries' versions, e.g. "libvpx v1.14.0, dav1d 1.5.1". */
std::string decoderLibraryVersions();

} // namespace webm
