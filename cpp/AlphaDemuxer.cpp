#include "AlphaDemuxer.h"

#include "Av1SequenceHeader.h"
#include "ByteSource.h"
#include "YuvToRgb.h"

#include <webm/callback.h>
#include <webm/reader.h>
#include <webm/status.h>
#include <webm/webm_parser.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <string>

namespace webm {

namespace {

constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
constexpr uint64_t kNoPosition = std::numeric_limits<uint64_t>::max();
constexpr size_t kReadBufferSize = 64 * 1024;
constexpr uint64_t kMaxFrameSize = 64 * 1024 * 1024;
// Without a DefaultDuration, the frame rate comes from this many colour timestamps
constexpr size_t kFrameRateSamples = 32;
// Opening stops looking for the first alpha packet this far past the first cluster (FFmpeg's probesize)
constexpr uint64_t kProbeBytes = 5000000;

// webm_parser's Reader over a ByteSource. Buffered: the parser reads element headers a few bytes
// at a time. A source's kPending is the parser's kWouldBlock, from which it resumes.
class SourceReader : public Reader {
public:
    explicit SourceReader(ByteSource* source) : source_(source), buffer_(new uint8_t[kReadBufferSize]) {}

    Status Read(std::size_t numToRead, std::uint8_t* buffer, std::uint64_t* numActuallyRead) override {
        *numActuallyRead = 0;
        if (numToRead == 0) return Status(Status::kOkCompleted);
        if (pos_ < bufferPos_ || pos_ >= bufferPos_ + bufferSize_) {
            if (numToRead >= kReadBufferSize) {
                return advance(source_->read(pos_, buffer, numToRead), numToRead, numActuallyRead);
            }
            const int64_t n = source_->read(pos_, buffer_.get(), kReadBufferSize);
            if (n <= 0) return advance(n, numToRead, numActuallyRead);
            bufferPos_ = pos_;
            bufferSize_ = static_cast<uint64_t>(n);
        }
        const uint64_t n = std::min<uint64_t>(numToRead, bufferPos_ + bufferSize_ - pos_);
        std::memcpy(buffer, buffer_.get() + (pos_ - bufferPos_), n);
        return advance(static_cast<int64_t>(n), numToRead, numActuallyRead);
    }

    // Moves without reading, but not past the end, so that the parser sees a truncated element
    // as one.
    Status Skip(std::uint64_t numToSkip, std::uint64_t* numActuallySkipped) override {
        const int64_t size = source_->size();
        uint64_t n = numToSkip;
        if (size >= 0) n = std::min<uint64_t>(n, static_cast<uint64_t>(size) > pos_ ? size - pos_ : 0);
        pos_ += n;
        *numActuallySkipped = n;
        if (n == numToSkip) return Status(Status::kOkCompleted);
        return Status(n > 0 ? Status::kOkPartial : Status::kEndOfFile);
    }

    std::uint64_t Position() const override { return pos_; }

    void seek(uint64_t pos) {
        pos_ = pos;
        failed_ = false;
        pending_ = false;
    }
    bool failed() const { return failed_; }
    // Whether the last read found the source without the bytes yet
    bool pending() const { return pending_; }
    void clearPending() { pending_ = false; }

private:
    Status advance(int64_t n, std::size_t wanted, std::uint64_t* numActuallyRead) {
        if (n == ByteSource::kPending) {
            pending_ = true;
            return Status(Status::kWouldBlock);
        }
        if (n < 0) failed_ = true;
        if (n <= 0) return Status(Status::kEndOfFile);
        pos_ += static_cast<uint64_t>(n);
        *numActuallyRead = static_cast<uint64_t>(n);
        return Status(static_cast<std::size_t>(n) == wanted ? Status::kOkCompleted : Status::kOkPartial);
    }

    ByteSource* source_;
    std::unique_ptr<uint8_t[]> buffer_;
    uint64_t bufferPos_ = 0;
    uint64_t bufferSize_ = 0;
    uint64_t pos_ = 0;
    bool failed_ = false;
    bool pending_ = false;
};

// a / d rounded to nearest, halves away from zero (FFmpeg's AV_ROUND_NEAR_INF), for d > 0.
int64_t divideRounded(int64_t a, int64_t d) {
    return a >= 0 ? (a + d / 2) / d : -((-a + d / 2) / d);
}

// FFmpeg's av_reduce: the closest fraction to num/den whose terms are at most max. For positive
// num and den.
void reduceFraction(int64_t num, int64_t den, int64_t max, int64_t* outNum, int64_t* outDen) {
    int64_t a0n = 0, a0d = 1, a1n = 1, a1d = 0;
    const int64_t gcd = std::gcd(num, den);
    if (gcd) {
        num /= gcd;
        den /= gcd;
    }
    if (num <= max && den <= max) {
        a1n = num;
        a1d = den;
        den = 0;
    }
    while (den) {
        int64_t x = num / den;
        const int64_t nextDen = num - den * x;
        const int64_t a2n = x * a1n + a0n;
        const int64_t a2d = x * a1d + a0d;
        if (a2n > max || a2d > max) {
            if (a1n) x = (max - a0n) / a1n;
            if (a1d) x = std::min(x, (max - a0d) / a1d);
            if (den * (2 * x * a1d + a0d) > num * a1d) {
                a1n = x * a1n + a0n;
                a1d = x * a1d + a0d;
            }
            break;
        }
        a0n = a1n;
        a0d = a1d;
        a1n = a2n;
        a1d = a2d;
        num = den;
        den = nextDen;
    }
    *outNum = a1n;
    *outDen = a1d;
}

// From frame timestamps, snapped to a common rate within 1% of it.
double estimateFrameRate(const std::vector<int64_t>& ptsUs) {
    if (ptsUs.size() < 2) return 0;
    const auto [first, last] = std::minmax_element(ptsUs.begin(), ptsUs.end());
    if (*last <= *first) return 0;
    const double fps = (ptsUs.size() - 1) * 1e6 / static_cast<double>(*last - *first);
    static const double kCommonRates[] = {
        24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 48, 50, 60000.0 / 1001, 60, 10, 12, 15, 120};
    for (double rate : kCommonRates) {
        if (std::fabs(fps - rate) < rate * 0.01) return rate;
    }
    return fps;
}

struct BitReader {
    const uint8_t* data;
    int bitPos = 0;
    int bits(int n) {
        int v = 0;
        for (; n > 0; n--, bitPos++) v = (v << 1) | ((data[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
        return v;
    }
};

struct Vp9Header {
    int profile = 0;
    int bitDepth = 8;
    int colorSpace = kMatrixUnspecified;
    int colorRange = kRangeLimited;
    int width = 0;
    int height = 0;
};

// A VP9 keyframe's uncompressed header, as FFmpeg's vp9 decoder reads it (VP9 spec 6.2).
bool parseVp9KeyframeHeader(const uint8_t* data, size_t size, Vp9Header* out) {
    if (!data || size < 10) return false;  // The fields read below fit in 10 bytes
    BitReader r{data};
    if (r.bits(2) != 2) return false;  // frame_marker
    int profile = r.bits(1);
    profile |= r.bits(1) << 1;
    if (profile == 3) r.bits(1);
    if (r.bits(1)) return false;  // show_existing_frame
    if (r.bits(1)) return false;  // frame_type: not a keyframe
    r.bits(2);                    // show_frame, error_resilient_mode
    if (r.bits(24) != 0x498342) return false;  // sync code

    out->profile = profile;
    out->bitDepth = profile >= 2 ? (r.bits(1) ? 12 : 10) : 8;
    static const int kColorSpaces[8] = {kMatrixUnspecified, kMatrixBt470bg, kMatrixBt709, kMatrixSmpte170m,
                                        kMatrixSmpte240m, kMatrixBt2020Ncl, kMatrixReserved, kMatrixRgb};
    out->colorSpace = kColorSpaces[r.bits(3)];
    if (out->colorSpace != kMatrixRgb) {
        out->colorRange = r.bits(1) ? kRangeFull : kRangeLimited;
        if (profile & 1) r.bits(3);  // subsampling_x, subsampling_y, reserved_zero
    } else {
        out->colorRange = kRangeFull;
        if (profile & 1) r.bits(1);  // reserved_zero
    }
    out->width = r.bits(16) + 1;
    out->height = r.bits(16) + 1;
    return true;
}

// The sequence header from av1C's configOBUs, as FFmpeg's libdav1d reads it, or else from the
// first packet.
bool parseAv1TrackHeader(const std::vector<uint8_t>& av1C, const std::vector<uint8_t>& firstPacket,
                         Av1SequenceHeader* out) {
    if (av1C.size() > 4 && (av1C[0] & 0x80) && parseAv1SequenceHeader(av1C.data() + 4, av1C.size() - 4, out)) {
        return true;
    }
    return !firstPacket.empty() && parseAv1SequenceHeader(firstPacket.data(), firstPacket.size(), out);
}

// The track's Colour element, as FFmpeg's Matroska demuxer takes it.
void readContainerColour(const TrackEntry& entry, AlphaDemuxer::StreamInfo* s) {
    if (!entry.video.is_present() || !entry.video.value().colour.is_present()) return;
    const Colour& colour = entry.video.value().colour.value();
    const int matrix = static_cast<int>(colour.matrix_coefficients.value());
    const int primaries = static_cast<int>(colour.primaries.value());
    const int trc = static_cast<int>(colour.transfer_characteristics.value());
    const int range = static_cast<int>(colour.range.value());
    if (matrix != kMatrixReserved) s->colorSpace = matrix;
    if (primaries != 0 && primaries != 3) s->colorPrimaries = primaries;
    if (trc != 0 && trc != 3) s->colorTrc = trc;
    if (range == kRangeLimited || range == kRangeFull) s->colorRange = range;
}

} // namespace

bool isVp9Keyframe(const uint8_t* data, int size) {
    if (!data || size < 1) return false;
    const uint8_t b = data[0];
    if ((b >> 6) != 2) return false;  // frame_marker
    const int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
    // profile 3 has a reserved bit before show_existing_frame
    const int showExistingBit = profile == 3 ? 2 : 3;
    if ((b >> showExistingBit) & 1) return false;
    return ((b >> (showExistingBit - 1)) & 1) == 0;  // frame_type 0 is a keyframe
}

/**
 * Reads the file through webm_parser, which calls back per element. The callbacks pause it at the
 * end of each block it returns packets for, so packets are read one block at a time.
 *
 * Seeking, like FFmpeg's: to the colour keyframe at or before the time, from an index of the
 * keyframes read so far (complete up to where the file has been read through from the start)
 * and the Cues, read when first needed. Past the last keyframe known, the index grows by reading
 * on. Then every block before that keyframe's time is skipped.
 */
class AlphaDemuxer::Impl : public Callback {
public:
    explicit Impl(std::unique_ptr<ByteSource> source)
        : source_(std::move(source)), reader_(source_.get()) {}

    AlphaDemuxer::Progress tryOpen();
    const AlphaDemuxer::Info& info() const { return info_; }
    bool readPacket(AlphaDemuxer::Packet* out);
    bool failed() const { return reader_.failed(); }
    bool pending() const { return pending_; }
    void seekToStart();
    void seekToTimeUs(int64_t timeUs);

    Status OnElementBegin(const ElementMetadata& metadata, Action* action) override;
    Status OnSegmentBegin(const ElementMetadata& metadata, Action* action) override;
    Status OnSeek(const ElementMetadata& metadata, const Seek& seek) override;
    Status OnInfo(const ElementMetadata& metadata, const webm::Info& info) override;
    Status OnTrackEntry(const ElementMetadata& metadata, const TrackEntry& entry) override;
    Status OnClusterBegin(const ElementMetadata& metadata, const Cluster& cluster, Action* action) override;
    Status OnSimpleBlockBegin(const ElementMetadata& metadata, const SimpleBlock& block, Action* action) override;
    Status OnSimpleBlockEnd(const ElementMetadata& metadata, const SimpleBlock& block) override;
    Status OnBlockGroupBegin(const ElementMetadata& metadata, Action* action) override;
    Status OnBlockBegin(const ElementMetadata& metadata, const Block& block, Action* action) override;
    Status OnBlockGroupEnd(const ElementMetadata& metadata, const BlockGroup& group) override;
    Status OnFrame(const FrameMetadata& metadata, Reader* reader, std::uint64_t* bytesRemaining) override;
    Status OnCuePoint(const ElementMetadata& metadata, const CuePoint& cuePoint) override;

private:
    // Reads only the Cues element, for its cue points.
    class CuesCallback : public Callback {
    public:
        explicit CuesCallback(Impl* impl) : impl_(impl) {}
        Status OnElementBegin(const ElementMetadata& metadata, Action* action) override;
        Status OnCuePoint(const ElementMetadata&, const CuePoint& cuePoint) override {
            impl_->addCuePoint(cuePoint);
            return Status(Status::kOkCompleted);
        }

    private:
        Impl* impl_;
        uint64_t end_ = 0;
    };

    enum class Mode {
        kProbe,    // Opening: keeps the first packets, for the bitstream's info
        kPackets,  // Queues packets
        kScan,     // Indexes keyframes up to scanUntil_, without reading frames
    };

    struct Track {
        uint64_t number = 0;  // 0 when there's no such track
        AlphaDemuxer::Codec codec = AlphaDemuxer::VP9;
        TrackEntry entry;
    };

    struct OwnedPacket {
        int stream;
        std::vector<uint8_t> data;
        int64_t ptsUs;
        int frameIndex;
        bool key;
    };

    enum class Fed {
        kPaused,   // A callback paused the parser
        kPending,  // The source doesn't have the next bytes yet
        kEnded,    // The end of the file, or an error
    };

    Fed feed();
    void restartAt(int64_t position);
    bool probeComplete() const;
    void buildInfo();
    AlphaDemuxer::StreamInfo streamInfoOf(const Track& track, const std::vector<uint8_t>& firstPacket,
                                          Vp9Header* vp9) const;
    bool findKeyframe(int64_t ticks, std::map<int64_t, uint64_t>::const_iterator* out);
    std::map<int64_t, uint64_t>::const_iterator keyframeAtOrBefore(int64_t ticks) const;
    bool scan(uint64_t from, int64_t untilTicks);
    bool loadCues();
    void addCuePoint(const CuePoint& cuePoint);

    void beginBlock(uint64_t trackNumber, int16_t timecode);
    Status noteColourBlock();
    Status finishBlock(const std::vector<uint8_t>* blockAdditional);
    Status keepProbePackets(const std::vector<uint8_t>* blockAdditional);

    int64_t ticksToUs(int64_t ticks) const;
    int64_t usToTicks(int64_t us) const;

    std::unique_ptr<ByteSource> source_;
    SourceReader reader_;
    WebmParser parser_;
    bool ended_ = false;
    bool opened_ = false;
    bool pending_ = false;  // The last call stopped at a kPending read
    Mode mode_ = Mode::kProbe;
    AlphaDemuxer::Info info_;

    // From the headers
    int64_t segmentDataStart_ = -1;
    int64_t cuesPosition_ = -1;
    int64_t firstClusterPosition_ = -1;
    uint64_t timecodeScale_ = 1000000;  // Nanoseconds per tick
    double durationTicks_ = 0;
    Track colour_;
    Track alpha_;

    // The block being read
    uint64_t clusterPosition_ = 0;
    int64_t clusterTicks_ = 0;
    int stream_ = -1;  // 0 colour, 1 alpha, -1 neither
    int64_t blockTicks_ = 0;
    bool key_ = false;
    bool discard_ = false;  // Not returned; its frames are skipped
    bool inBlockGroup_ = false;
    std::vector<std::vector<uint8_t>> frames_;
    uint64_t framePosition_ = kNoPosition;
    // A block end paused the parser; the parser calls it again when it resumes
    bool pausedAtBlockEnd_ = false;

    // Opening
    bool haveColourPacket_ = false;
    bool haveAlphaPacket_ = false;
    std::vector<uint8_t> firstColourPacket_;
    std::vector<uint8_t> firstAlphaPacket_;
    bool firstColourHasBlockAdditional_ = false;
    int64_t startTicks_ = 0;
    std::vector<int64_t> colourPtsSamples_;

    // Reading
    std::deque<OwnedPacket> queue_;
    OwnedPacket current_{};
    int colourFrameIndex_ = 0;
    int alphaFrameIndex_ = 0;
    int64_t skipBefore_ = kNone;  // After a seek, blocks before this tick are skipped
    bool colourNeedsKey_ = false;  // After a seek, colour blocks are skipped until a keyframe

    // Seeking: colour keyframes' ticks -> their clusters' positions
    std::map<int64_t, uint64_t> keyframes_;
    bool readingFromStart_ = true;  // Every block since the first has been seen
    int64_t indexedUpTo_ = kNone;  // keyframes_ has every keyframe up to this tick
    bool cuesLoaded_ = false;
    int64_t scanUntil_ = 0;
    bool scanStopped_ = false;
};

// --- Reading -------------------------------------------------------------------------------

int64_t AlphaDemuxer::Impl::ticksToUs(int64_t ticks) const {
    if (timecodeScale_ % 1000 == 0) return ticks * static_cast<int64_t>(timecodeScale_ / 1000);
    return std::llround(static_cast<double>(ticks) * static_cast<double>(timecodeScale_) / 1000.0);
}

int64_t AlphaDemuxer::Impl::usToTicks(int64_t us) const {
    if (timecodeScale_ % 1000 == 0) return divideRounded(us, static_cast<int64_t>(timecodeScale_ / 1000));
    return std::llround(static_cast<double>(us) * 1000.0 / static_cast<double>(timecodeScale_));
}

// Feeds the parser until a callback pauses it, the source has no more bytes yet, or the file
// ends or can't be read.
AlphaDemuxer::Impl::Fed AlphaDemuxer::Impl::feed() {
    if (ended_) return Fed::kEnded;
    reader_.clearPending();
    const Status status = parser_.Feed(this, &reader_);
    if (status.code == Status::kWouldBlock) return reader_.pending() ? Fed::kPending : Fed::kPaused;
    ended_ = true;
    // Read through from the start: every keyframe is indexed
    if (status.completed_ok() && readingFromStart_) indexedUpTo_ = std::numeric_limits<int64_t>::max();
    return Fed::kEnded;
}

void AlphaDemuxer::Impl::restartAt(int64_t position) {
    queue_.clear();
    frames_.clear();
    framePosition_ = kNoPosition;
    pausedAtBlockEnd_ = false;
    inBlockGroup_ = false;
    readingFromStart_ = false;
    skipBefore_ = kNone;
    colourNeedsKey_ = false;
    if (position < 0) {
        ended_ = true;
        return;
    }
    reader_.seek(static_cast<uint64_t>(position));
    parser_.DidSeek();
    ended_ = false;
}

bool AlphaDemuxer::Impl::readPacket(AlphaDemuxer::Packet* out) {
    pending_ = false;
    while (queue_.empty()) {
        const Fed fed = feed();
        if (fed == Fed::kPaused) continue;
        pending_ = fed == Fed::kPending;
        return false;
    }
    current_ = std::move(queue_.front());
    queue_.pop_front();
    out->stream = current_.stream;
    out->data = current_.data.data();
    out->size = static_cast<int>(current_.data.size());
    out->ptsUs = current_.ptsUs;
    out->frameIndex = current_.frameIndex;
    out->key = current_.key;
    return true;
}

// --- Opening -------------------------------------------------------------------------------

bool AlphaDemuxer::Impl::probeComplete() const {
    if (!haveColourPacket_) return false;
    if (reader_.Position() > static_cast<uint64_t>(firstClusterPosition_) + kProbeBytes) return true;
    const bool needFrameRate = !colour_.entry.default_duration.is_present() ||
                               colour_.entry.default_duration.value() == 0;
    return (!alpha_.number || haveAlphaPacket_) &&
           (!needFrameRate || colourPtsSamples_.size() >= kFrameRateSamples);
}

AlphaDemuxer::Progress AlphaDemuxer::Impl::tryOpen() {
    if (opened_) return AlphaDemuxer::Progress::kDone;
    // The parser pauses once the first packets are kept, or at the first cluster if there's no track
    while (!probeComplete()) {
        const Fed fed = feed();
        if (fed == Fed::kPending) return AlphaDemuxer::Progress::kPending;
        if (fed == Fed::kEnded || !colour_.number) break;
    }
    if (!colour_.number) return AlphaDemuxer::Progress::kFailed;
    buildInfo();
    firstColourPacket_ = {};
    firstAlphaPacket_ = {};
    colourPtsSamples_ = {};
    mode_ = Mode::kPackets;
    seekToStart();
    opened_ = true;
    return AlphaDemuxer::Progress::kDone;
}

AlphaDemuxer::StreamInfo AlphaDemuxer::Impl::streamInfoOf(const Track& track, const std::vector<uint8_t>& firstPacket,
                                                          Vp9Header* vp9) const {
    AlphaDemuxer::StreamInfo s;
    s.codec = track.codec;
    readContainerColour(track.entry, &s);
    if (track.codec == AlphaDemuxer::VP9) {
        // VP9's range and matrix are the bitstream's; primaries and transfer the container's
        if (parseVp9KeyframeHeader(firstPacket.data(), firstPacket.size(), vp9)) {
            s.profile = vp9->profile;
            s.bitDepth = vp9->bitDepth;
            s.colorSpace = vp9->colorSpace;
            s.colorRange = vp9->colorRange;
        }
        return s;
    }

    s.codecPrivate = track.entry.codec_private.value();
    Av1SequenceHeader seq;
    if (parseAv1TrackHeader(s.codecPrivate, firstPacket, &seq)) {
        s.profile = seq.profile;
        s.bitDepth = seq.bitDepth;
        s.colorRange = seq.fullRange ? kRangeFull : kRangeLimited;
        s.colorSpace = seq.matrixCoefficients;
        s.colorPrimaries = seq.colorPrimaries;
        s.colorTrc = seq.transferCharacteristics;
    } else if (s.codecPrivate.size() >= 3) {
        const uint8_t flags = s.codecPrivate[2];
        s.bitDepth = (flags & 0x40) ? ((flags & 0x20) ? 12 : 10) : 8;
    }
    return s;
}

void AlphaDemuxer::Impl::buildInfo() {
    const TrackEntry& entry = colour_.entry;
    if (entry.video.is_present()) {
        info_.width = static_cast<int>(entry.video.value().pixel_width.value());
        info_.height = static_cast<int>(entry.video.value().pixel_height.value());
    }
    Vp9Header vp9;
    info_.color = streamInfoOf(colour_, firstColourPacket_, &vp9);
    if (vp9.width > 0) {
        info_.width = vp9.width;
        info_.height = vp9.height;
    }

    if (entry.default_duration.is_present() && entry.default_duration.value() > 0) {
        int64_t num = 0, den = 0;
        reduceFraction(1000000000, static_cast<int64_t>(entry.default_duration.value()), 30000, &num, &den);
        if (den > 0) info_.frameRate = static_cast<double>(num) / static_cast<double>(den);
    } else {
        std::vector<int64_t> ptsUs;
        for (int64_t ticks : colourPtsSamples_) ptsUs.push_back(ticksToUs(ticks));
        if (const double fps = estimateFrameRate(ptsUs); fps > 0) info_.frameRate = fps;
    }

    if (durationTicks_ > 0) {
        info_.durationUs = static_cast<int64_t>(durationTicks_ * static_cast<double>(timecodeScale_) / 1000.0);
        info_.totalFrames = static_cast<int>(std::lround(static_cast<double>(info_.durationUs) / 1e6 * info_.frameRate));
    } else {
        info_.totalFrames = 100;
    }
    info_.startUs = haveColourPacket_ ? ticksToUs(startTicks_) : 0;

    if (alpha_.number) {
        Vp9Header alphaVp9;
        info_.alpha = streamInfoOf(alpha_, firstAlphaPacket_, &alphaVp9);
    } else if (colour_.codec == AlphaDemuxer::VP9 && firstColourHasBlockAdditional_) {
        info_.alphaInBlockAdditional = true;
        info_.alpha = info_.color;
        // libvpx's alpha stream says limited range but holds alpha as-is, 0-255
        info_.alpha.colorRange = kRangeFull;
    }
    info_.hasAlpha = alpha_.number != 0 || info_.alphaInBlockAdditional;
}

// --- Seeking -------------------------------------------------------------------------------

void AlphaDemuxer::Impl::seekToStart() {
    restartAt(firstClusterPosition_);
    readingFromStart_ = true;
    colourFrameIndex_ = 0;
    alphaFrameIndex_ = 0;
}

void AlphaDemuxer::Impl::seekToTimeUs(int64_t timeUs) {
    pending_ = false;
    std::map<int64_t, uint64_t>::const_iterator keyframe;
    const bool found = findKeyframe(usToTicks(timeUs), &keyframe);
    if (pending_) return;
    if (!found) {
        restartAt(firstClusterPosition_);
        readingFromStart_ = true;
        return;
    }
    const int64_t keyTicks = keyframe->first;
    restartAt(static_cast<int64_t>(keyframe->second));
    skipBefore_ = keyTicks;
    colourNeedsKey_ = true;
}

std::map<int64_t, uint64_t>::const_iterator AlphaDemuxer::Impl::keyframeAtOrBefore(int64_t ticks) const {
    // Clamped to the first, as FFmpeg does
    auto it = keyframes_.upper_bound(std::max(ticks, keyframes_.begin()->first));
    return --it;
}

// False if no keyframe is known, or if reading stopped at a kPending read (pending_).
bool AlphaDemuxer::Impl::findKeyframe(int64_t ticks, std::map<int64_t, uint64_t>::const_iterator* out) {
    const bool pastIndexed = indexedUpTo_ == kNone || ticks > indexedUpTo_;
    if (pastIndexed && !cuesLoaded_ && !loadCues()) return false;
    // Past the last keyframe known, there may be later ones that aren't
    if (pastIndexed &&
        (keyframes_.empty() || std::next(keyframeAtOrBefore(ticks)) == keyframes_.end())) {
        const uint64_t from = keyframes_.empty() ? static_cast<uint64_t>(firstClusterPosition_)
                                                 : keyframeAtOrBefore(ticks)->second;
        if (!scan(from, ticks)) return false;
    }
    if (keyframes_.empty()) return false;
    *out = keyframeAtOrBefore(ticks);
    return true;
}

// Reads on from the cluster at `from` until a colour block after `untilTicks`, indexing keyframes.
// False if it stopped at a kPending read; the keyframes found up to there are kept.
bool AlphaDemuxer::Impl::scan(uint64_t from, int64_t untilTicks) {
    if (firstClusterPosition_ < 0) return true;
    restartAt(static_cast<int64_t>(from));
    mode_ = Mode::kScan;
    scanUntil_ = untilTicks;
    scanStopped_ = false;
    Fed fed = Fed::kPaused;
    while (!scanStopped_ && (fed = feed()) == Fed::kPaused) {}
    mode_ = Mode::kPackets;
    pending_ = fed == Fed::kPending;
    return !pending_;
}

// False if it stopped at a kPending read; the Cues are then read again next time.
bool AlphaDemuxer::Impl::loadCues() {
    cuesLoaded_ = true;
    if (cuesPosition_ < 0) return true;
    SourceReader reader(source_.get());
    reader.seek(static_cast<uint64_t>(cuesPosition_));
    WebmParser parser;
    parser.DidSeek();
    CuesCallback callback(this);
    parser.Feed(&callback, &reader);
    if (!reader.pending()) return true;
    cuesLoaded_ = false;
    pending_ = true;
    return false;
}

Status AlphaDemuxer::Impl::CuesCallback::OnElementBegin(const ElementMetadata& metadata, Action* action) {
    if (end_ == 0) {
        if (metadata.id != Id::kCues) return Status(Status::kWouldBlock);
        end_ = metadata.size == kUnknownElementSize ? kNoPosition
                                                    : metadata.position + metadata.header_size + metadata.size;
    } else if (metadata.position >= end_) {
        return Status(Status::kWouldBlock);
    }
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
}

void AlphaDemuxer::Impl::addCuePoint(const CuePoint& cuePoint) {
    if (!cuePoint.time.is_present() || segmentDataStart_ < 0) return;
    for (const Element<CueTrackPositions>& positions : cuePoint.cue_track_positions) {
        const CueTrackPositions& p = positions.value();
        if (p.track.value() != colour_.number || !p.cluster_position.is_present()) continue;
        keyframes_.emplace(static_cast<int64_t>(cuePoint.time.value()),
                           static_cast<uint64_t>(segmentDataStart_) + p.cluster_position.value());
    }
}

// --- Parser callbacks ----------------------------------------------------------------------

Status AlphaDemuxer::Impl::OnElementBegin(const ElementMetadata& metadata, Action* action) {
    if (metadata.id == Id::kChapters || metadata.id == Id::kTags) {
        *action = Action::kSkip;
    } else {
        if (metadata.id == Id::kCues) cuesLoaded_ = true;
        *action = Action::kRead;
    }
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnSegmentBegin(const ElementMetadata& metadata, Action* action) {
    if (segmentDataStart_ < 0) segmentDataStart_ = static_cast<int64_t>(metadata.position + metadata.header_size);
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnSeek(const ElementMetadata&, const Seek& seek) {
    if (seek.id.value() == Id::kCues && seek.position.is_present() && cuesPosition_ < 0 && segmentDataStart_ >= 0) {
        cuesPosition_ = segmentDataStart_ + static_cast<int64_t>(seek.position.value());
    }
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnInfo(const ElementMetadata&, const webm::Info& info) {
    if (info.timecode_scale.value() > 0) timecodeScale_ = info.timecode_scale.value();
    if (info.duration.is_present()) durationTicks_ = info.duration.value();
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnTrackEntry(const ElementMetadata&, const TrackEntry& entry) {
    if (entry.track_type.value() != TrackType::kVideo) return Status(Status::kOkCompleted);
    const std::string& codecId = entry.codec_id.value();
    if (codecId != "V_VP9" && codecId != "V_AV1") return Status(Status::kOkCompleted);
    Track* track = !colour_.number ? &colour_ : !alpha_.number ? &alpha_ : nullptr;
    if (track && entry.track_number.value() > 0) {
        track->number = entry.track_number.value();
        track->codec = codecId == "V_AV1" ? AlphaDemuxer::AV1 : AlphaDemuxer::VP9;
        track->entry = entry;
    }
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnClusterBegin(const ElementMetadata& metadata, const Cluster& cluster, Action* action) {
    if (firstClusterPosition_ < 0) {
        firstClusterPosition_ = static_cast<int64_t>(metadata.position);
        // No track to read: opening stops here
        if (!colour_.number) return Status(Status::kWouldBlock);
    }
    clusterPosition_ = metadata.position;
    clusterTicks_ = static_cast<int64_t>(cluster.timecode.value());
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
}

void AlphaDemuxer::Impl::beginBlock(uint64_t trackNumber, int16_t timecode) {
    stream_ = trackNumber == colour_.number ? 0 : (alpha_.number && trackNumber == alpha_.number) ? 1 : -1;
    blockTicks_ = clusterTicks_ + timecode;
    frames_.clear();
    framePosition_ = kNoPosition;
    discard_ = mode_ == Mode::kScan || stream_ < 0;
    // Like FFmpeg, blocks of any track end the skip after a seek
    if (mode_ != Mode::kScan && skipBefore_ != kNone) {
        if (blockTicks_ < skipBefore_) {
            discard_ = true;
        } else {
            skipBefore_ = kNone;
        }
    }
}

Status AlphaDemuxer::Impl::OnSimpleBlockBegin(const ElementMetadata&, const SimpleBlock& block, Action* action) {
    beginBlock(block.track_number, block.timecode);
    key_ = block.is_key_frame;
    *action = discard_ ? Action::kSkip : Action::kRead;
    return stream_ == 0 ? noteColourBlock() : Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnSimpleBlockEnd(const ElementMetadata&, const SimpleBlock&) {
    if (pausedAtBlockEnd_) {
        pausedAtBlockEnd_ = false;
        return Status(Status::kOkCompleted);
    }
    return finishBlock(nullptr);
}

Status AlphaDemuxer::Impl::OnBlockGroupBegin(const ElementMetadata&, Action* action) {
    inBlockGroup_ = false;
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnBlockBegin(const ElementMetadata&, const Block& block, Action* action) {
    beginBlock(block.track_number, block.timecode);
    inBlockGroup_ = true;
    *action = discard_ ? Action::kSkip : Action::kRead;
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnBlockGroupEnd(const ElementMetadata&, const BlockGroup& group) {
    if (pausedAtBlockEnd_) {
        pausedAtBlockEnd_ = false;
        return Status(Status::kOkCompleted);
    }
    if (!inBlockGroup_) return Status(Status::kOkCompleted);
    inBlockGroup_ = false;
    key_ = group.references.empty();
    if (stream_ == 0) {
        const Status status = noteColourBlock();
        if (!status.completed_ok()) return status;
    }
    const std::vector<uint8_t>* blockAdditional = nullptr;
    if (group.additions.is_present()) {
        for (const Element<BlockMore>& more : group.additions.value().block_mores) {
            if (more.value().id.value() == 1) {
                blockAdditional = &more.value().data.value();
                break;
            }
        }
    }
    return finishBlock(blockAdditional);
}

Status AlphaDemuxer::Impl::OnFrame(const FrameMetadata& metadata, Reader* reader, std::uint64_t* bytesRemaining) {
    if (discard_) return Skip(reader, bytesRemaining);
    if (metadata.position != framePosition_) {
        if (metadata.size > kMaxFrameSize) return Status(Status::kNotEnoughMemory);
        framePosition_ = metadata.position;
        frames_.emplace_back(static_cast<size_t>(metadata.size));
    }
    std::vector<uint8_t>& frame = frames_.back();
    while (*bytesRemaining > 0) {
        std::uint64_t n = 0;
        const Status status = reader->Read(static_cast<size_t>(*bytesRemaining),
                                           frame.data() + (frame.size() - *bytesRemaining), &n);
        *bytesRemaining -= n;
        if (!status.ok()) return status;
    }
    return Status(Status::kOkCompleted);
}

Status AlphaDemuxer::Impl::OnCuePoint(const ElementMetadata&, const CuePoint& cuePoint) {
    addCuePoint(cuePoint);
    return Status(Status::kOkCompleted);
}

// Indexes a colour block once its keyframe flag is known; stops a scan past its target.
Status AlphaDemuxer::Impl::noteColourBlock() {
    if (key_) keyframes_.emplace(blockTicks_, clusterPosition_);
    if (readingFromStart_ && (indexedUpTo_ == kNone || blockTicks_ > indexedUpTo_)) indexedUpTo_ = blockTicks_;
    if (mode_ == Mode::kScan && blockTicks_ > scanUntil_) {
        scanStopped_ = true;
        return Status(Status::kWouldBlock);
    }
    return Status(Status::kOkCompleted);
}

// Turns the block just read into packets and pauses the parser, so that readPacket returns them.
Status AlphaDemuxer::Impl::finishBlock(const std::vector<uint8_t>* blockAdditional) {
    if (discard_ || frames_.empty()) return Status(Status::kOkCompleted);
    if (stream_ == 0 && colourNeedsKey_) {
        if (!key_) return Status(Status::kOkCompleted);
        colourNeedsKey_ = false;
    }
    if (mode_ == Mode::kProbe) return keepProbePackets(blockAdditional);

    const int64_t ptsUs = ticksToUs(blockTicks_);
    for (std::vector<uint8_t>& frame : frames_) {
        if (stream_ == 1) {
            queue_.push_back({1, std::move(frame), ptsUs, alphaFrameIndex_++, key_});
            continue;
        }
        queue_.push_back({0, std::move(frame), ptsUs, colourFrameIndex_++, key_});
        if (info_.alphaInBlockAdditional) {
            // Empty when the block has no alpha
            std::vector<uint8_t> alpha = blockAdditional ? *blockAdditional : std::vector<uint8_t>();
            const bool key = isVp9Keyframe(alpha.data(), static_cast<int>(alpha.size()));
            queue_.push_back({1, std::move(alpha), ptsUs, alphaFrameIndex_++, key});
        }
    }
    frames_.clear();
    pausedAtBlockEnd_ = true;
    return Status(Status::kWouldBlock);
}

Status AlphaDemuxer::Impl::keepProbePackets(const std::vector<uint8_t>* blockAdditional) {
    if (stream_ == 0) {
        if (!haveColourPacket_) {
            haveColourPacket_ = true;
            firstColourPacket_ = frames_.front();
            firstColourHasBlockAdditional_ = blockAdditional && !blockAdditional->empty();
            startTicks_ = blockTicks_;
        }
        if (colourPtsSamples_.size() < kFrameRateSamples) colourPtsSamples_.push_back(blockTicks_);
    } else if (!haveAlphaPacket_) {
        haveAlphaPacket_ = true;
        firstAlphaPacket_ = frames_.front();
    }
    frames_.clear();
    if (!probeComplete()) return Status(Status::kOkCompleted);
    pausedAtBlockEnd_ = true;
    return Status(Status::kWouldBlock);
}

// --- AlphaDemuxer --------------------------------------------------------------------------

AlphaDemuxer::AlphaDemuxer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AlphaDemuxer::~AlphaDemuxer() = default;

std::unique_ptr<AlphaDemuxer> AlphaDemuxer::open(std::unique_ptr<ByteSource> source) {
    std::unique_ptr<AlphaDemuxer> demuxer = create(std::move(source));
    if (!demuxer || demuxer->tryOpen() != Progress::kDone) return nullptr;
    return demuxer;
}

std::unique_ptr<AlphaDemuxer> AlphaDemuxer::create(std::unique_ptr<ByteSource> source) {
    if (!source) return nullptr;
    return std::unique_ptr<AlphaDemuxer>(new AlphaDemuxer(std::make_unique<Impl>(std::move(source))));
}

std::vector<int> AlphaDemuxer::colorAlphaTracks(std::unique_ptr<ByteSource> source) {
    std::vector<int> tracks;
    const std::unique_ptr<AlphaDemuxer> demuxer = open(std::move(source));
    if (!demuxer) return tracks;
    const Info& info = demuxer->info();
    tracks = {info.color.codec, info.color.profile};
    if (info.hasAlpha) {
        tracks.push_back(info.alpha.codec);
        tracks.push_back(info.alpha.profile);
    }
    return tracks;
}

AlphaDemuxer::Progress AlphaDemuxer::tryOpen() { return impl_->tryOpen(); }
const AlphaDemuxer::Info& AlphaDemuxer::info() const { return impl_->info(); }
bool AlphaDemuxer::readPacket(Packet* out) { return impl_->readPacket(out); }
bool AlphaDemuxer::failed() const { return impl_->failed(); }
bool AlphaDemuxer::pending() const { return impl_->pending(); }
void AlphaDemuxer::seekToStart() { impl_->seekToStart(); }
void AlphaDemuxer::seekToTimeUs(int64_t timeUs) { impl_->seekToTimeUs(timeUs); }

} // namespace webm
