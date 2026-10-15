#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct AVFormatContext;
struct AVIOContext;
struct AVPacket;

namespace webm {

/**
 * Reads an alpha WebM's colour and alpha bitstreams as packets, without decoding them, for
 * decoders that take one bitstream each: MediaCodec on Android, WebCodecs on the web.
 *
 * The colour stream is the first VP9 or AV1 track. The alpha stream is the second, or, for a
 * single VP9 track, the VP9 stream in each block's BlockAdditional (ID 1), which is how libvpx
 * writes a standard `yuva420p` WebM.
 */
class AlphaDemuxer {
public:
    enum Codec { VP9 = 0, AV1 = 1 };

    struct StreamInfo {
        Codec codec = VP9;
        int profile = 0;   // FFmpeg's profile for the track
        int bitDepth = 8;
        int colorRange = 0;  // AVColorRange
        int colorSpace = 2;  // AVColorSpace (2 = unspecified)
        int colorPrimaries = 2;
        int colorTrc = 2;
        std::vector<uint8_t> codecPrivate;  // Matroska CodecPrivate: av1C for AV1, usually empty for VP9
    };

    struct Info {
        int width = 0;
        int height = 0;
        int totalFrames = 0;
        double frameRate = 30.0;
        int64_t durationUs = 0;
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

    /**
     * Opens `io` as a WebM. The caller keeps ownership of `io` and frees it after the demuxer.
     * Returns null if the input can't be opened or has no VP9 or AV1 track.
     */
    static std::unique_ptr<AlphaDemuxer> open(AVIOContext* io);

    /**
     * The colour and alpha bitstreams of `io`, as (codec, profile) pairs, colour first: two
     * pairs for an alpha WebM, fewer otherwise. Reads `io` from where it is and leaves it at an
     * unspecified position; the caller keeps ownership.
     */
    static std::vector<int> colorAlphaTracks(AVIOContext* io);

    ~AlphaDemuxer();
    AlphaDemuxer(const AlphaDemuxer&) = delete;
    AlphaDemuxer& operator=(const AlphaDemuxer&) = delete;

    const Info& info() const { return info_; }

    /** The next colour or alpha packet; false at the end of the file or on a read error. */
    bool readPacket(Packet* out);

    void seekToStart();
    /** To the keyframe at or before `timeUs`, in the timebase of the packets' ptsUs. */
    void seekToTimeUs(int64_t timeUs);

private:
    AlphaDemuxer() = default;
    bool readLayout();
    void resetCounters();

    AVFormatContext* formatCtx_ = nullptr;
    AVPacket* packet_ = nullptr;
    int colorStreamIndex_ = -1;
    int alphaStreamIndex_ = -1;
    Info info_;

    int colorFrameIndex_ = 0;
    int alphaFrameIndex_ = 0;
    // A BlockAdditional file: each colour packet read leaves its alpha payload for the next read
    bool hasPendingAlpha_ = false;
    std::vector<uint8_t> pendingAlpha_;
    int64_t pendingAlphaPtsUs_ = 0;
};

/** Whether a VP9 frame (or superframe) starts with a keyframe, from its uncompressed header. */
bool isVp9Keyframe(const uint8_t* data, int size);

} // namespace webm
