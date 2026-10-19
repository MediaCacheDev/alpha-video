#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace webm {

class ByteSource;

/**
 * Reads an alpha WebM's colour and alpha bitstreams as packets, without decoding them, for
 * decoders that take one bitstream each: the core's libvpx and dav1d decoders, MediaCodec, and on
 * the web WebCodecs and the software decoders in Workers.
 *
 * The colour stream is the first VP8, VP9 or AV1 track. The alpha stream is the second, or, for a
 * single VP8 or VP9 track, the stream in each block's BlockAdditional (ID 1), which is how libvpx
 * writes a standard `yuva420p` WebM.
 *
 * Built on libwebm's webm_parser. Android, iOS and the web (compiled to WebAssembly) all use it;
 * tests/core/demuxer_compare checks it against FFmpeg's reading of each file.
 */
class AlphaDemuxer {
public:
    enum Codec { VP9 = 0, AV1 = 1, VP8 = 2 };

    /** Colour numbers are H.273's, as in YuvToRgb.h (and FFmpeg's AVColor* enums). */
    struct StreamInfo {
        Codec codec = VP9;
        int profile = 0;   // From the bitstream
        int bitDepth = 8;
        int colorRange = 0;  // ColorRange
        int colorSpace = 2;  // ColorMatrix (2 = unspecified)
        int colorPrimaries = 2;
        int colorTrc = 2;
        std::vector<uint8_t> codecPrivate;  // Matroska CodecPrivate: av1C for AV1, empty for VP8 and VP9
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

    enum class Progress { kDone, kPending, kFailed };

    /** Opens `source` as a WebM. Returns null if it can't be read or has no VP8, VP9 or AV1 track. */
    static std::unique_ptr<AlphaDemuxer> open(std::unique_ptr<ByteSource> source);

    /**
     * A demuxer for a source that may answer ByteSource::kPending, not yet open: call tryOpen()
     * until it no longer returns kPending. Null if `source` is.
     */
    static std::unique_ptr<AlphaDemuxer> create(std::unique_ptr<ByteSource> source);

    /**
     * Reads the headers and the first packets. kPending: the source didn't have the bytes yet;
     * call again once it has. kFailed: not a WebM with a VP8, VP9 or AV1 track, or unreadable.
     * info() is valid once it returns kDone.
     */
    Progress tryOpen();

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

    /**
     * Whether the last readPacket() or seekToTimeUs() stopped because the source answered
     * ByteSource::kPending. Call it again once the bytes are there: a read carries on where it
     * stopped, and a seek starts over (seeking elsewhere instead is fine too).
     */
    bool pending() const;

    /** Back to the first packet, with frameIndex counting from 0 again. */
    void seekToStart();
    /** To the keyframe at or before `timeUs`, in the timebase of the packets' ptsUs. Keeps the
     *  frameIndex counters going. May read the Cues, or scan for keyframes, so may be pending(). */
    void seekToTimeUs(int64_t timeUs);

private:
    class Impl;
    explicit AlphaDemuxer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/** Whether a VP9 frame (or superframe) starts with a keyframe, from its uncompressed header. */
bool isVp9Keyframe(const uint8_t* data, int size);

/** Whether a VP8 frame is a keyframe, from its frame tag. */
bool isVp8Keyframe(const uint8_t* data, int size);

} // namespace webm
