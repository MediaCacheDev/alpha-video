#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace webm {

class ByteSource;

/**
 * Reads an alpha WebM's colour and alpha bitstreams as packets, without decoding them, for
 * decoders that take one bitstream each: the core's libvpx and dav1d decoders, and MediaCodec.
 *
 * The colour stream is the first VP9 or AV1 track. The alpha stream is the second, or, for a
 * single VP9 track, the VP9 stream in each block's BlockAdditional (ID 1), which is how libvpx
 * writes a standard `yuva420p` WebM.
 *
 * Built on libwebm's webm_parser. The web has an FFmpeg version (web/wasm/AlphaDemuxer); both
 * report the same info and packets, and seek to the same keyframes.
 */
class AlphaDemuxer {
public:
    enum Codec { VP9 = 0, AV1 = 1 };

    /** Colour numbers are H.273's, as in YuvToRgb.h (and FFmpeg's AVColor* enums). */
    struct StreamInfo {
        Codec codec = VP9;
        int profile = 0;   // From the bitstream
        int bitDepth = 8;
        int colorRange = 0;  // ColorRange
        int colorSpace = 2;  // ColorMatrix (2 = unspecified)
        int colorPrimaries = 2;
        int colorTrc = 2;
        std::vector<uint8_t> codecPrivate;  // Matroska CodecPrivate: av1C for AV1, empty for VP9
    };

    struct Info {
        int width = 0;
        int height = 0;
        int totalFrames = 0;
        double frameRate = 30.0;
        int64_t durationUs = 0;
        int64_t startUs = 0;  // The first colour packet's ptsUs
        bool hasAlpha = false;
        bool alphaInBlockAdditional = false;
        StreamInfo color;
        StreamInfo alpha;  // Valid when hasAlpha
    };

    struct Packet {
        int stream = 0;  // 0 colour, 1 alpha
        const uint8_t* data = nullptr;  // Valid until the next read, seek or destruction
        int size = 0;
        int64_t ptsUs = 0;
        int frameIndex = 0;
        bool key = false;
    };

    /** Opens `source` as a WebM. Returns null if it can't be read or has no VP9 or AV1 track. */
    static std::unique_ptr<AlphaDemuxer> open(std::unique_ptr<ByteSource> source);

    /**
     * The colour and alpha bitstreams of `source`, as (codec, profile) pairs, colour first: two
     * pairs for an alpha WebM, fewer otherwise.
     */
    static std::vector<int> colorAlphaTracks(std::unique_ptr<ByteSource> source);

    ~AlphaDemuxer();
    AlphaDemuxer(const AlphaDemuxer&) = delete;
    AlphaDemuxer& operator=(const AlphaDemuxer&) = delete;

    const Info& info() const;

    /** The next colour or alpha packet; false at the end of the file or when reading fails. */
    bool readPacket(Packet* out);

    /** Whether the last readPacket() returned false because reading failed (an I/O error, a
     *  failed or aborted download), rather than at the end of the file. */
    bool failed() const;

    /** Back to the first packet, with frameIndex counting from 0 again. */
    void seekToStart();
    /** To the keyframe at or before `timeUs`, in the timebase of the packets' ptsUs. Keeps the
     *  frameIndex counters going. */
    void seekToTimeUs(int64_t timeUs);

private:
    class Impl;
    explicit AlphaDemuxer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/** Whether a VP9 frame (or superframe) starts with a keyframe, from its uncompressed header. */
bool isVp9Keyframe(const uint8_t* data, int size);

} // namespace webm
