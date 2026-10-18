// Checks the core's demuxer (cpp/AlphaDemuxer, on libwebm) against one on FFmpeg's Matroska
// demuxer (ffmpeg/AlphaDemuxer, which the web used before): the same info, every packet the same,
// and the same packets after seeks.
//
//   demuxer_compare file.webm...
//
// The FFmpeg demuxer is compiled in namespace ffwebm, so that both fit in one program.

#include "AlphaDemuxer.h"
#include "ByteSource.h"

#define webm ffwebm
#include "ffmpeg/AlphaDemuxer.h"
#undef webm

extern "C" {
#include <libavformat/avformat.h>
}

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define EXPECT_EQ(a, b, what)                                                                   \
    do {                                                                                         \
        if (!((a) == (b))) {                                                                     \
            failures++;                                                                          \
            std::printf("    MISMATCH %s: libwebm %s, FFmpeg %s\n", what, std::to_string(a).c_str(), \
                        std::to_string(b).c_str());                                              \
        }                                                                                        \
    } while (0)

struct FFmpegInput {
    AVIOContext* io = nullptr;
    std::unique_ptr<ffwebm::AlphaDemuxer> demuxer;
    explicit FFmpegInput(const std::string& path) {
        if (avio_open(&io, path.c_str(), AVIO_FLAG_READ) < 0) return;
        demuxer = ffwebm::AlphaDemuxer::open(io);
    }
    ~FFmpegInput() {
        demuxer.reset();
        avio_closep(&io);
    }
};

struct Packet {
    int stream, size, frameIndex;
    int64_t ptsUs;
    bool key;
    std::vector<uint8_t> data;
};

template <class D, class P>
bool next(D& demuxer, Packet* out) {
    P p;
    if (!demuxer.readPacket(&p)) return false;
    *out = {p.stream, p.size, p.frameIndex, p.ptsUs, p.key, std::vector<uint8_t>(p.data, p.data + p.size)};
    return true;
}

bool nextOurs(webm::AlphaDemuxer& d, Packet* out) { return next<webm::AlphaDemuxer, webm::AlphaDemuxer::Packet>(d, out); }
bool nextTheirs(ffwebm::AlphaDemuxer& d, Packet* out) { return next<ffwebm::AlphaDemuxer, ffwebm::AlphaDemuxer::Packet>(d, out); }

template <class A, class B>
void compareStream(const A& a, const B& b, const char* name) {
    std::string n(name);
    EXPECT_EQ(int(a.codec), int(b.codec), (n + ".codec").c_str());
    EXPECT_EQ(a.profile, b.profile, (n + ".profile").c_str());
    EXPECT_EQ(a.bitDepth, b.bitDepth, (n + ".bitDepth").c_str());
    EXPECT_EQ(a.colorRange, b.colorRange, (n + ".colorRange").c_str());
    EXPECT_EQ(a.colorSpace, b.colorSpace, (n + ".colorSpace").c_str());
    EXPECT_EQ(a.colorPrimaries, b.colorPrimaries, (n + ".colorPrimaries").c_str());
    EXPECT_EQ(a.colorTrc, b.colorTrc, (n + ".colorTrc").c_str());
    EXPECT_EQ(a.codecPrivate.size(), b.codecPrivate.size(), (n + ".codecPrivate.size").c_str());
    EXPECT_EQ(int(a.codecPrivate == b.codecPrivate), 1, (n + ".codecPrivate").c_str());
}

// Compares up to `limit` packets (all if negative); returns how many matched.
int comparePackets(webm::AlphaDemuxer& ours, ffwebm::AlphaDemuxer& theirs, int limit, const char* when) {
    int count = 0;
    for (; limit < 0 || count < limit; count++) {
        Packet a, b;
        const bool gotA = nextOurs(ours, &a);
        const bool gotB = nextTheirs(theirs, &b);
        if (gotA != gotB) {
            failures++;
            std::printf("    MISMATCH %s packet %d: libwebm %s, FFmpeg %s\n", when, count, gotA ? "has one" : "ended",
                        gotB ? "has one" : "ended");
            return count;
        }
        if (!gotA) break;
        if (a.stream != b.stream || a.size != b.size || a.ptsUs != b.ptsUs || a.frameIndex != b.frameIndex ||
            a.key != b.key || a.data != b.data) {
            failures++;
            std::printf("    MISMATCH %s packet %d: libwebm {stream %d, %d bytes, pts %lld, #%d, key %d}, "
                        "FFmpeg {stream %d, %d bytes, pts %lld, #%d, key %d}%s\n",
                        when, count, a.stream, a.size, (long long)a.ptsUs, a.frameIndex, a.key, b.stream, b.size,
                        (long long)b.ptsUs, b.frameIndex, b.key, a.data == b.data ? "" : ", different bytes");
            return count;
        }
    }
    return count;
}

double readAllMs(const std::string& path, bool ours) {
    const auto start = std::chrono::steady_clock::now();
    if (ours) {
        auto d = webm::AlphaDemuxer::open(webm::openFile(path));
        webm::AlphaDemuxer::Packet p;
        while (d && d->readPacket(&p)) {}
    } else {
        FFmpegInput in(path);
        ffwebm::AlphaDemuxer::Packet p;
        while (in.demuxer && in.demuxer->readPacket(&p)) {}
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void compareFile(const std::string& path) {
    std::printf("%s\n", path.c_str());
    const int failuresBefore = failures;

    auto ours = webm::AlphaDemuxer::open(webm::openFile(path));
    FFmpegInput theirs(path);
    if (!ours || !theirs.demuxer) {
        EXPECT_EQ(int(bool(ours)), int(bool(theirs.demuxer)), "opens");
        return;
    }

    const auto& a = ours->info();
    const auto& b = theirs.demuxer->info();
    EXPECT_EQ(a.width, b.width, "width");
    EXPECT_EQ(a.height, b.height, "height");
    EXPECT_EQ(a.totalFrames, b.totalFrames, "totalFrames");
    EXPECT_EQ(a.frameRate, b.frameRate, "frameRate");
    EXPECT_EQ(a.durationUs, b.durationUs, "durationUs");
    EXPECT_EQ(a.hasAlpha, b.hasAlpha, "hasAlpha");
    EXPECT_EQ(a.alphaInBlockAdditional, b.alphaInBlockAdditional, "alphaInBlockAdditional");
    compareStream(a.color, b.color, "color");
    if (a.hasAlpha) compareStream(a.alpha, b.alpha, "alpha");

    {
        FFmpegInput tracksIn(path);
        AVIOContext* io = nullptr;
        avio_open(&io, path.c_str(), AVIO_FLAG_READ);
        const std::vector<int> theirTracks = ffwebm::AlphaDemuxer::colorAlphaTracks(io);
        avio_closep(&io);
        const std::vector<int> ourTracks = webm::AlphaDemuxer::colorAlphaTracks(webm::openFile(path));
        EXPECT_EQ(int(ourTracks == theirTracks), 1, "colorAlphaTracks");
    }

    const int packets = comparePackets(*ours, *theirs.demuxer, -1, "reading");

    // Seeks, from the end of the file: to the start, every 7th of the way, before the start and past the end
    const int64_t duration = a.durationUs > 0 ? a.durationUs : 5000000;
    std::vector<int64_t> targets = {0, -1000, duration + 1000000, duration / 2 + 1, 1000, 1500};
    for (int k = 1; k < 7; k++) targets.push_back(duration * k / 7);
    for (int64_t t : targets) {
        ours->seekToTimeUs(t);
        theirs.demuxer->seekToTimeUs(t);
        const std::string when = "after seekToTimeUs(" + std::to_string(t) + ")";
        comparePackets(*ours, *theirs.demuxer, 24, when.c_str());
    }
    // Backwards
    for (int k = 6; k >= 1; k--) {
        ours->seekToTimeUs(duration * k / 7);
        theirs.demuxer->seekToTimeUs(duration * k / 7);
        const std::string when = "after seeking back to " + std::to_string(duration * k / 7);
        comparePackets(*ours, *theirs.demuxer, 8, when.c_str());
    }
    ours->seekToStart();
    theirs.demuxer->seekToStart();
    comparePackets(*ours, *theirs.demuxer, -1, "after seekToStart");

    const double oursMs = readAllMs(path, true);
    const double theirsMs = readAllMs(path, false);
    std::printf("    %s: %d packets, %dx%d, %.3f fps, %lld us, colour {codec %d, profile %d, %d-bit, range %d, "
                "matrix %d, primaries %d, trc %d}, alpha %s; reading all: libwebm %.1f ms, FFmpeg %.1f ms\n",
                failures == failuresBefore ? "same" : "DIFFERENT", packets, a.width, a.height, a.frameRate,
                (long long)a.durationUs, a.color.codec, a.color.profile, a.color.bitDepth, a.color.colorRange,
                a.color.colorSpace, a.color.colorPrimaries, a.color.colorTrc,
                !a.hasAlpha ? "none" : a.alphaInBlockAdditional ? "BlockAdditional" : "track", oursMs, theirsMs);
}

} // namespace

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_ERROR);
    for (int i = 1; i < argc; i++) compareFile(argv[i]);
    std::printf("%s: %d mismatches\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
