// Tests of the C++ core against the test-card fixtures in tests/fixtures.
//
// The fixtures come from the test-card generator in the demo app (alpha-video-test,
// testing/testcard/generate.ts). Card holds the part of its layout these tests read; it must match
// testing/testcard/layout.ts there. Every frame carries its own index as a row of black/white cells
// in the colour planes and again in the alpha plane, so a test can tell which frame it got.

#include "WebMAlphaPlayer.h"
#include "AlphaDemuxer.h"
#include "Av1SequenceHeader.h"
#include "ByteSource.h"
#include "RangeSourceClient.h"
#include "VideoDecoder.h"

#include "../../codecs/av1/Dav1dDecoderPlugin.h"
#include "../../codecs/vpx/VpxDecoderPlugin.h"

#include <dav1d/dav1d.h>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace webm;

// ---------------------------------------------------------------- runner

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::ostringstream message_;                                                    \
            message_ << __FILE__ << ":" << __LINE__ << ": " << #cond << " " << __VA_ARGS__; \
            throw Failure(message_.str());                                                  \
        }                                                                                   \
    } while (0)

struct Test {
    std::string name;
    std::function<void()> run;
};

static std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

// Known bugs: a test whose name contains `match` is expected to fail, with a message containing
// `symptom`, until `bug` is fixed. Failing any other way is a real failure, and a known failure
// that passes is reported so the entry gets removed with the fix.
struct KnownFailure {
    const char* match;
    const char* symptom;
    const char* bug;
};
static const std::vector<KnownFailure> kKnownFailures = {};

static const KnownFailure* knownFailure(const std::string& name) {
    for (const auto& k : kKnownFailures) {
        if (name.find(k.match) != std::string::npos) return &k;
    }
    return nullptr;
}

// ---------------------------------------------------------------- the card

struct Rect {
    int x, y, w, h;
};

struct Card {
    int width = 0, height = 0, unit = 0;
    int frames = 60;
    double fps = 30;

    static Card forSize(int w, int h) {
        Card c;
        c.width = w;
        c.height = h;
        c.unit = std::max(8, std::min(w, h) / 16);
        return c;
    }
    Rect cell(int col, int row) const { return {col * unit, row * unit, unit, unit}; }
    Rect codeCell(int bit) const { return cell(2 + bit, 1); }
    Rect invertedCell(int bit) const { return cell(2 + bit, 2); }
    Rect alphaCodeCell(int bit) const { return cell(2 + bit, 3); }
};

static const int kCodeBits = 12;

struct AlphaPatch {
    int col;
    int alpha;
};
// Row 5: white at alpha 0, 64, 128, 191, 255, then red at 64, 128, 191, 255.
static const AlphaPatch kAlphaRamp[] = {{2, 0}, {3, 64}, {4, 128}, {5, 191}, {6, 255}, {7, 64}, {8, 128}, {9, 191}, {10, 255}};

struct ColourPatch {
    const char* name;
    int col;
    int r, g, b;
};
// Row 7, all opaque.
static const ColourPatch kColours[] = {
    {"black", 2, 0, 0, 0},       {"white", 3, 255, 255, 255},  {"grey 128", 4, 128, 128, 128},
    {"red 191", 5, 191, 0, 0},   {"green 191", 6, 0, 191, 0},  {"blue 191", 7, 0, 0, 191},
    {"skin", 8, 224, 172, 105},  {"grey 64", 9, 64, 64, 64},   {"grey 192", 10, 192, 192, 192},
};

// The largest error a correct decode shows on these fixtures (FFmpeg's own conversion, measured by
// testing/reference.ts in the demo: at most 3 levels) plus a margin of 3.
static const double kLevelTolerance = 6;

struct Fixture {
    std::string name;  // e.g. card_383x217/yuva.webm
    std::string path;
    Card card;
};

static std::vector<Fixture> fixtures() {
    std::vector<Fixture> all;
    for (auto [w, h] : {std::pair{383, 217}, std::pair{256, 256}}) {
        std::string card = "card_" + std::to_string(w) + "x" + std::to_string(h);
        for (const char* file : {"yuva.webm", "dual_vp9.webm", "dual_vp9_p0.webm", "dual_av1.webm", "dual_av1_10bit.webm",
                                 "yuva_vp8.webm", "dual_vp8.webm"}) {
            all.push_back({card + "/" + file, std::string(FIXTURES_DIR) + "/" + card + "/" + file, Card::forSize(w, h)});
        }
    }
    return all;
}

/** Mean of a plane over the middle half of `r` (in card pixels); `scale` 2 for 4:2:0 chroma. */
static double planeMean(const uint8_t* plane, int stride, int planeW, int planeH, Rect r, int scale = 1) {
    int x0 = (r.x + r.w / 4) / scale, x1 = std::max(x0 + 1, (r.x + 3 * r.w / 4) / scale);
    int y0 = (r.y + r.h / 4) / scale, y1 = std::max(y0 + 1, (r.y + 3 * r.h / 4) / scale);
    x1 = std::min(x1, planeW);
    y1 = std::min(y1, planeH);
    double sum = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) sum += plane[y * stride + x];
    }
    return sum / ((x1 - x0) * (y1 - y0));
}

struct Codes {
    int colour;  // -1 when the inverted row doesn't match
    int alpha;
};

static Codes readCodes(const YUVAFrame& f, const Card& card) {
    int code = 0, inverted = 0, alpha = 0;
    for (int bit = 0; bit < kCodeBits; bit++) {
        code = (code << 1) | (planeMean(f.yPlane, f.yStride, f.width, f.height, card.codeCell(bit)) > 125.5);
        inverted = (inverted << 1) | (planeMean(f.yPlane, f.yStride, f.width, f.height, card.invertedCell(bit)) > 125.5);
        alpha = (alpha << 1) | (planeMean(f.aPlane, f.aStride, f.width, f.height, card.alphaCodeCell(bit)) > 125.5);
    }
    bool valid = (code ^ inverted) == (1 << kCodeBits) - 1;
    return {valid ? code : -1, alpha};
}

static void checkFrame(const std::shared_ptr<YUVAFrame>& f, const Card& card, int expected) {
    CHECK(f != nullptr, "no frame " << expected);
    Codes codes = readCodes(*f, card);
    CHECK(codes.colour == expected, "colour code " << codes.colour << ", expected frame " << expected);
    CHECK(codes.alpha == expected, "alpha code " << codes.alpha << ", expected frame " << expected);
}

// Android has no /tmp; run it there with TMPDIR set to a writable directory
static std::string tempPath(const std::string& name) {
    const char* dir = getenv("TMPDIR");
    return std::string(dir && *dir ? dir : "/tmp") + "/core_tests_" + name + "_" + std::to_string(getpid());
}

static PlayerConfig onDemandConfig() {
    PlayerConfig config;
    config.mode = PlaybackMode::Preload;
    config.preloadAllFrames = false;  // getFrame() decodes on demand, in any order
    config.loopPlayback = false;
    return config;
}

static std::unique_ptr<WebMAlphaPlayer> load(const Fixture& fx, const PlayerConfig& config) {
    auto player = std::make_unique<WebMAlphaPlayer>();
    player->setConfig(config);
    CHECK(player->loadFromFile(fx.path), "loading " << fx.path);
    return player;
}

/** Collects frames delivered through the frame callback, and lets a test wait for one. */
struct FrameWaiter {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int> codes;
    /** Frames whose alpha code didn't match their colour code, as "colour/alpha". */
    std::vector<std::string> alphaMismatches;
    Card card;

    void attach(WebMAlphaPlayer& player) {
        player.setFrameCallback([this](std::shared_ptr<YUVAFrame> f) {
            Codes c = readCodes(*f, card);
            std::lock_guard<std::mutex> lock(mutex);
            codes.push_back(c.colour);
            if (c.alpha != c.colour) alphaMismatches.push_back(std::to_string(c.colour) + "/" + std::to_string(c.alpha));
            changed.notify_all();
        });
    }
    bool waitFor(int code, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return std::find(codes.begin(), codes.end(), code) != codes.end(); });
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        codes.clear();
    }
};

/** A clip served slowly, a KB at a time, through SlowUpstream connections. */
struct SlowRemote {
    std::vector<uint8_t> data;
    std::chrono::microseconds delay{2000};
    std::mutex mutex;
    std::condition_variable resumed;
    std::vector<int64_t> opens;
    int64_t sent = 0;
    /** Reads stop short of this offset until release(); -1 for none. */
    int64_t holdAt = -1;

    explicit SlowRemote(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        holdAt = -1;
        resumed.notify_all();
    }
    int64_t bytesSent() {
        std::lock_guard<std::mutex> lock(mutex);
        return sent;
    }
    std::vector<int64_t> openPositions() {
        std::lock_guard<std::mutex> lock(mutex);
        return opens;
    }
};

class SlowUpstream : public webm::Upstream {
public:
    explicit SlowUpstream(std::shared_ptr<SlowRemote> remote) : remote_(std::move(remote)) {}

    bool open(int64_t position, webm::SourceInfo& info, std::string&) override {
        std::lock_guard<std::mutex> lock(remote_->mutex);
        remote_->opens.push_back(position);
        pos_ = std::min<int64_t>(position, static_cast<int64_t>(remote_->data.size()));
        info.size = static_cast<int64_t>(remote_->data.size());
        info.etag = "1";
        return true;
    }
    int64_t read(uint8_t* out, int64_t size, std::string&) override {
        std::this_thread::sleep_for(remote_->delay);
        std::unique_lock<std::mutex> lock(remote_->mutex);
        remote_->resumed.wait(lock, [&] { return remote_->holdAt < 0 || pos_ < remote_->holdAt; });
        const int64_t end = remote_->holdAt >= 0 ? remote_->holdAt : static_cast<int64_t>(remote_->data.size());
        const int64_t n = std::min<int64_t>({size, 1024, end - pos_});
        std::memcpy(out, remote_->data.data() + pos_, static_cast<size_t>(n));
        pos_ += n;
        remote_->sent += n;
        return n;
    }
    void close() override {}

private:
    std::shared_ptr<SlowRemote> remote_;
    int64_t pos_ = 0;
};

/** The add-on's reader over remote, with blocks small enough that the test cards (5-25 KB) span many. */
static std::shared_ptr<webm::SourceReader> slowReader(std::shared_ptr<SlowRemote> remote, int64_t blockSize = 4096) {
    webm_rs::ReaderOptions options;
    options.blockSize = blockSize;
    options.readAheadBytes = blockSize;
    options.reconnectGap = 0;
    std::string error;
    auto reader = webm_rs::RangeReader::open(range_source_plugin(), std::make_unique<SlowUpstream>(std::move(remote)), nullptr,
                                             "", options, error);
    CHECK(reader, "RangeReader::open: " << error);
    return reader;
}

/** The basic input over remote: a download into a temporary file. */
static std::shared_ptr<webm::SourceReader> slowDownload(std::shared_ptr<SlowRemote> remote) {
    std::string error;
    auto reader = webm_rs::DownloadReader::open(std::make_unique<SlowUpstream>(std::move(remote)), "/tmp", error);
    CHECK(reader, "DownloadReader::open: " << error);
    return reader;
}

/**
 * A file whose bytes arrive only when asked for, as on the web: a read of bytes not yet loaded
 * answers kPending and records them, and the test loads them before calling the demuxer again.
 */
struct HeldBytes {
    // Small and odd, so pieces end inside element headers and frames
    static constexpr int64_t kPiece = 97;
    std::vector<uint8_t> data;
    std::vector<bool> loaded;
    int64_t wanted = -1;
    int pendings = 0;

    explicit HeldBytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        loaded.assign((data.size() + kPiece - 1) / kPiece, false);
    }
    /** Loads the piece the last pending read wanted; false if nothing was wanted. */
    bool loadWanted() {
        if (wanted < 0) return false;
        loaded[wanted / kPiece] = true;
        wanted = -1;
        return true;
    }
};

class HeldSource : public ByteSource {
public:
    explicit HeldSource(HeldBytes* bytes) : bytes_(bytes) {}
    int64_t read(int64_t pos, uint8_t* out, int64_t size) override {
        const int64_t total = static_cast<int64_t>(bytes_->data.size());
        if (pos >= total) return 0;
        if (!bytes_->loaded[pos / HeldBytes::kPiece]) {
            bytes_->wanted = pos;
            bytes_->pendings++;
            return kPending;
        }
        const int64_t n = std::min({size, total - pos, (pos / HeldBytes::kPiece + 1) * HeldBytes::kPiece - pos});
        std::memcpy(out, bytes_->data.data() + pos, n);
        return n;
    }
    int64_t size() const override { return static_cast<int64_t>(bytes_->data.size()); }

private:
    HeldBytes* bytes_;
};

/** A packet's identity, to compare two demuxers' output. */
static std::string describe(const AlphaDemuxer::Packet& p) {
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < p.size; i++) hash = (hash ^ p.data[i]) * 1099511628211ull;
    std::ostringstream s;
    s << p.stream << "@" << p.ptsUs << (p.key ? "k" : "") << "#" << p.frameIndex << ":" << p.size << ":" << hash;
    return s.str();
}

// ---------------------------------------------------------------- tests

static void registerTests() {
    for (const Fixture& fx : fixtures()) {
        auto add = [&](const std::string& name, std::function<void(const Fixture&)> fn) {
            tests().push_back({name + " [" + fx.name + "]", [fx, fn] { fn(fx); }});
        };

        add("clip info", [](const Fixture& fx) {
            auto p = load(fx, onDemandConfig());
            CHECK(p->getWidth() == fx.card.width, p->getWidth());
            CHECK(p->getHeight() == fx.card.height, p->getHeight());
            CHECK(p->getTotalFrames() == fx.card.frames, p->getTotalFrames());
            CHECK(std::abs(p->getFrameRate() - fx.card.fps) < 0.01, p->getFrameRate());
            CHECK(std::abs(p->getDuration() - fx.card.frames / fx.card.fps) < 0.05, p->getDuration());
            std::string decoder = fx.name.find("av1") != std::string::npos ? "dav1d" : "libvpx";
            CHECK(p->getDecoderName() == decoder, p->getDecoderName());
        });

        add("demuxer: layout and packets", [](const Fixture& fx) {
            int fd = ::open(fx.path.c_str(), O_RDONLY);
            CHECK(fd >= 0, fx.path);
            std::vector<int> tracks = AlphaDemuxer::colorAlphaTracks(openFileRange(fd, 0, -1));
            const bool av1 = fx.name.find("av1") != std::string::npos;
            const AlphaDemuxer::Codec codec = av1                                       ? AlphaDemuxer::AV1
                                              : fx.name.find("vp8") != std::string::npos ? AlphaDemuxer::VP8
                                                                                         : AlphaDemuxer::VP9;
            const bool blockAdditional = fx.name.find("/yuva") != std::string::npos;
            CHECK(tracks.size() == 4, tracks.size());
            CHECK(tracks[0] == codec && tracks[2] == tracks[0], tracks[0] << " " << tracks[2]);

            std::unique_ptr<ByteSource> source = openFileRange(fd, 0, -1);
            ::close(fd);
            {
                std::unique_ptr<AlphaDemuxer> demuxer = AlphaDemuxer::open(std::move(source));
                CHECK(demuxer != nullptr, "open");
                const AlphaDemuxer::Info& info = demuxer->info();
                CHECK(info.width == fx.card.width && info.height == fx.card.height, info.width << "x" << info.height);
                CHECK(info.totalFrames == fx.card.frames, info.totalFrames);
                CHECK(info.hasAlpha && info.alphaInBlockAdditional == blockAdditional, info.alphaInBlockAdditional);
                CHECK(info.color.codec == codec && info.alpha.codec == codec, info.color.codec << " " << info.alpha.codec);
                CHECK(info.color.bitDepth == (fx.name.find("10bit") != std::string::npos ? 10 : 8), info.color.bitDepth);
                CHECK(info.alpha.bitDepth == info.color.bitDepth, info.alpha.bitDepth);
                if (blockAdditional) CHECK(info.alpha.colorRange == kRangeFull, info.alpha.colorRange);
                if (av1) CHECK(!info.color.codecPrivate.empty(), "AV1 has no av1C");

                auto readAll = [&](std::vector<int64_t>& colourKeys, std::vector<int64_t>& alphaKeys) {
                    int counts[2] = {0, 0};
                    AlphaDemuxer::Packet p;
                    while (demuxer->readPacket(&p)) {
                        CHECK(p.size > 0, "empty packet, stream " << p.stream << " frame " << p.frameIndex);
                        CHECK(p.frameIndex == counts[p.stream], p.frameIndex);
                        if (counts[0] + counts[1] == 0) CHECK(p.stream == 0 && p.key, "the first packet is a colour keyframe");
                        if (p.key) (p.stream == 0 ? colourKeys : alphaKeys).push_back(p.ptsUs);
                        counts[p.stream]++;
                    }
                    CHECK(counts[0] == fx.card.frames && counts[1] == fx.card.frames, counts[0] << " colour, " << counts[1] << " alpha");
                };
                std::vector<int64_t> colourKeys, alphaKeys;
                readAll(colourKeys, alphaKeys);
                CHECK(!colourKeys.empty() && colourKeys == alphaKeys, colourKeys.size() << " colour keys, " << alphaKeys.size() << " alpha");

                demuxer->seekToStart();
                std::vector<int64_t> colourKeys2, alphaKeys2;
                readAll(colourKeys2, alphaKeys2);
                CHECK(colourKeys2 == colourKeys, "keyframes after seekToStart");
            }
        });

        add("demuxer: a source that answers pending gives the same packets and seeks", [](const Fixture& fx) {
            std::unique_ptr<AlphaDemuxer> plain = AlphaDemuxer::open(openFile(fx.path));
            CHECK(plain != nullptr, "open");
            HeldBytes bytes(fx.path);
            std::unique_ptr<AlphaDemuxer> held = AlphaDemuxer::create(std::make_unique<HeldSource>(&bytes));
            AlphaDemuxer::Progress progress;
            while ((progress = held->tryOpen()) == AlphaDemuxer::Progress::kPending) CHECK(bytes.loadWanted(), "pending, wanting nothing");
            CHECK(progress == AlphaDemuxer::Progress::kDone, "tryOpen failed");
            const AlphaDemuxer::Info& a = plain->info();
            const AlphaDemuxer::Info& b = held->info();
            CHECK(a.width == b.width && a.height == b.height && a.totalFrames == b.totalFrames && a.frameRate == b.frameRate &&
                      a.durationUs == b.durationUs && a.hasAlpha == b.hasAlpha && a.alphaInBlockAdditional == b.alphaInBlockAdditional &&
                      a.color.bitDepth == b.color.bitDepth && a.color.colorSpace == b.color.colorSpace,
                  "info differs");

            // Up to `count` packets, each loading what it waits for
            auto readHeld = [&](int count) {
                std::vector<std::string> out;
                AlphaDemuxer::Packet p;
                while (static_cast<int>(out.size()) < count) {
                    if (held->readPacket(&p)) {
                        out.push_back(describe(p));
                    } else if (held->pending()) {
                        CHECK(bytes.loadWanted(), "pending, wanting nothing");
                    } else {
                        CHECK(!held->failed(), "failed");
                        break;
                    }
                }
                return out;
            };
            auto readPlain = [&](int count) {
                std::vector<std::string> out;
                AlphaDemuxer::Packet p;
                while (static_cast<int>(out.size()) < count && plain->readPacket(&p)) out.push_back(describe(p));
                return out;
            };
            auto seekHeld = [&](int64_t us) {
                held->seekToTimeUs(us);
                while (held->pending()) {
                    CHECK(bytes.loadWanted(), "pending, wanting nothing");
                    held->seekToTimeUs(us);
                }
            };

            const int everything = std::numeric_limits<int>::max();
            CHECK(readHeld(everything) == readPlain(everything), "packets differ");
            CHECK(bytes.pendings * HeldBytes::kPiece * 2 > static_cast<int64_t>(bytes.data.size()), bytes.pendings << " pending reads");

            // Fresh demuxers, so seeks find the keyframes from the Cues and scans, not from a read-through
            plain = AlphaDemuxer::open(openFile(fx.path));
            std::fill(bytes.loaded.begin(), bytes.loaded.end(), false);
            held = AlphaDemuxer::create(std::make_unique<HeldSource>(&bytes));
            while (held->tryOpen() == AlphaDemuxer::Progress::kPending) bytes.loadWanted();
            const int64_t frameUs = static_cast<int64_t>(1e6 / fx.card.fps);
            for (int frame : {37, 5, 59, 0, 31}) {
                const int64_t us = frame * frameUs;
                plain->seekToTimeUs(us);
                seekHeld(us);
                CHECK(readHeld(6) == readPlain(6), "after seeking to frame " << frame);
            }
        });

        add("every frame in order", [](const Fixture& fx) {
            auto p = load(fx, onDemandConfig());
            for (int i = 0; i < fx.card.frames; i++) checkFrame(p->getFrame(i), fx.card, i);
        });

        add("every frame in random order", [](const Fixture& fx) {
            auto p = load(fx, onDemandConfig());
            std::vector<int> order(fx.card.frames);
            for (int i = 0; i < fx.card.frames; i++) order[i] = i;
            std::shuffle(order.begin(), order.end(), std::mt19937(1234));
            for (int i : order) checkFrame(p->getFrame(i), fx.card, i);
        });

        add("seekTo maps times to frames", [](const Fixture& fx) {
            auto p = load(fx, onDemandConfig());
            const std::pair<double, int> cases[] = {
                {0, 0}, {1.0 / 30, 1}, {29.0 / 30, 29}, {1.0, 30}, {0.999, 29}, {59.0 / 30, 59},
                {-1, 0}, {NAN, 0}, {100, 59},
            };
            for (auto [time, frame] : cases) {
                p->seekTo(time);
                CHECK(p->getCurrentFrame() == frame, "seekTo(" << time << ") gave " << p->getCurrentFrame());
            }
        });

        add("preload decodes every frame", [](const Fixture& fx) {
            PlayerConfig config = onDemandConfig();
            config.preloadAllFrames = true;
            auto p = std::make_unique<WebMAlphaPlayer>();
            p->setConfig(config);
            std::mutex m;
            std::condition_variable cv;
            bool done = false;
            p->setEventCallback([&](const std::string& event, const std::string&) {
                if (event != "preload_complete") return;
                std::lock_guard<std::mutex> lock(m);
                done = true;
                cv.notify_all();
            });
            CHECK(p->loadFromFile(fx.path), fx.path);
            {
                std::unique_lock<std::mutex> lock(m);
                CHECK(cv.wait_for(lock, std::chrono::seconds(20), [&] { return done; }), "no preload_complete");
            }
            for (int i = 0; i < fx.card.frames; i++) checkFrame(p->getCachedFrame(i), fx.card, i);
        });

        add("stream mode seeks while paused", [](const Fixture& fx) {
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            auto p = std::make_unique<WebMAlphaPlayer>();
            p->setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(*p);
            CHECK(p->loadFromFile(fx.path), fx.path);
            for (int target : {59, 0, 30, 29, 31, 17, 45, 1}) {
                waiter.clear();
                p->seekToFrame(target);
                CHECK(waiter.waitFor(target, std::chrono::seconds(5)), "seekToFrame(" << target << ") showed no such frame");
            }
            std::lock_guard<std::mutex> lock(waiter.mutex);
            CHECK(waiter.alphaMismatches.empty(), "frames with the wrong alpha (colour/alpha code): "
                  << waiter.alphaMismatches.size() << ", first " << waiter.alphaMismatches.front());
        });

        add("stream mode shows frame 0 without play()", [](const Fixture& fx) {
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            auto p = std::make_unique<WebMAlphaPlayer>();
            p->setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(*p);
            CHECK(p->loadFromFile(fx.path), fx.path);
            CHECK(waiter.waitFor(0, std::chrono::seconds(5)), "frame 0 never reached the frame callback");
            std::lock_guard<std::mutex> lock(waiter.mutex);
            CHECK(waiter.alphaMismatches.empty(), "frame 0 with the wrong alpha: " << waiter.alphaMismatches.front());
        });

        add("stream mode plays every frame with its alpha", [](const Fixture& fx) {
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            config.playbackSpeed = 4;
            auto p = std::make_unique<WebMAlphaPlayer>();
            p->setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(*p);
            CHECK(p->loadFromFile(fx.path), fx.path);
            p->play();
            CHECK(waiter.waitFor(fx.card.frames - 1, std::chrono::seconds(10)), "playback never reached the last frame");
            std::lock_guard<std::mutex> lock(waiter.mutex);
            CHECK(waiter.alphaMismatches.empty(), "frames with the wrong alpha (colour/alpha code): "
                  << waiter.alphaMismatches.size() << ", first " << waiter.alphaMismatches.front());
            CHECK(waiter.codes.size() >= static_cast<size_t>(fx.card.frames / 2), "only " << waiter.codes.size() << " frames shown");
        });

        add("stream mode loops back to frame 0", [](const Fixture& fx) {
            // Past the last frame, which a decoder with frame delay gives only once drained
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = true;
            config.playbackSpeed = 4;
            auto p = std::make_unique<WebMAlphaPlayer>();
            p->setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(*p);
            CHECK(p->loadFromFile(fx.path), fx.path);
            p->play();
            CHECK(waiter.waitFor(fx.card.frames - 1, std::chrono::seconds(10)), "playback never reached the last frame");
            waiter.clear();
            CHECK(waiter.waitFor(5, std::chrono::seconds(5)), "playback didn't loop to frame 5");
            std::lock_guard<std::mutex> lock(waiter.mutex);
            CHECK(waiter.alphaMismatches.empty(), "frames with the wrong alpha (colour/alpha code): "
                  << waiter.alphaMismatches.size() << ", first " << waiter.alphaMismatches.front());
        });

        add("alpha plane range", [](const Fixture& fx) {
            auto p = load(fx, onDemandConfig());
            auto f = p->getFrame(0);
            CHECK(f != nullptr, "no frame");
            for (const auto& patch : kAlphaRamp) {
                double a = planeMean(f->aPlane, f->aStride, f->width, f->height, fx.card.cell(patch.col, 5));
                CHECK(std::abs(a - patch.alpha) <= kLevelTolerance, "alpha " << a << " where the card has " << patch.alpha);
            }
        });

        add("shader colour", [](const Fixture& fx) {
            // What the Android and iOS shaders compute (AlphaVideoView.java, AlphaVideoPlayer.mm):
            // the player's YuvToRgb applied to the plane samples.
            auto p = load(fx, onDemandConfig());
            auto f = p->getFrame(0);
            CHECK(f != nullptr, "no frame");
            const YuvToRgb m = p->getYuvToRgb();
            int cw = f->uvStride, chh = (f->height + 1) / 2;
            for (const auto& patch : kColours) {
                Rect r = fx.card.cell(patch.col, 7);
                double yuv[4] = {
                    planeMean(f->yPlane, f->yStride, f->width, f->height, r) / 255,
                    planeMean(f->uPlane, f->uvStride, cw, chh, r, 2) / 255,
                    planeMean(f->vPlane, f->uvStride, cw, chh, r, 2) / 255,
                    1,
                };
                double rgb[3];
                for (int c = 0; c < 3; c++) {
                    double sum = 0;
                    for (int k = 0; k < 4; k++) sum += m.m[k * 4 + c] * yuv[k];
                    rgb[c] = std::clamp(sum, 0.0, 1.0) * 255;
                }
                int expected[3] = {patch.r, patch.g, patch.b};
                for (int c = 0; c < 3; c++) {
                    CHECK(std::abs(rgb[c] - expected[c]) <= kLevelTolerance,
                          patch.name << " channel " << c << " is " << rgb[c] << ", expected " << expected[c]);
                }
            }
        });

        add("chroma planes cover odd sizes", [](const Fixture& fx) {
            auto p = load(fx, onDemandConfig());
            auto f = p->getFrame(0);
            CHECK(f != nullptr, "no frame");
            CHECK(f->uvStride >= (f->width + 1) / 2, "chroma plane is " << f->uvStride << " wide for a " << f->width << " px frame");
            CHECK(f->uSize >= ((f->width + 1) / 2) * ((f->height + 1) / 2), "chroma plane holds " << f->uSize << " bytes");
        });

        add("file range inside a larger file", [](const Fixture& fx) {
            std::ifstream in(fx.path, std::ios::binary);
            std::vector<char> clip((std::istreambuf_iterator<char>(in)), {});
            std::string path = tempPath("range") + ".bin";
            {
                std::ofstream out(path, std::ios::binary);
                std::string before(1000, 'x'), after(500, 'y');
                out << before;
                out.write(clip.data(), clip.size());
                out << after;
            }
            int fd = open(path.c_str(), O_RDONLY);
            CHECK(fd >= 0, path);
            WebMAlphaPlayer p;
            p.setConfig(onDemandConfig());
            bool loaded = p.loadFromFileRange(fd, 1000, static_cast<int64_t>(clip.size()));
            close(fd);
            unlink(path.c_str());
            CHECK(loaded, "loadFromFileRange");
            for (int i : {0, 31, 59}) checkFrame(p.getFrame(i), fx.card, i);
        });

        add("plays while a range reader fetches the file", [](const Fixture& fx) {
            auto remote = std::make_shared<SlowRemote>(fx.path);
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            WebMAlphaPlayer p;
            p.setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(p);
            bool loaded = p.loadFromReader(slowReader(remote));
            if (loaded) p.play();
            bool firstFrame = loaded && waiter.waitFor(0, std::chrono::seconds(5));
            const int64_t sentByFirstFrame = remote->bytesSent();
            bool lastFrame = firstFrame && waiter.waitFor(fx.card.frames - 1, std::chrono::seconds(20));
            p.unload();
            CHECK(loaded, "loadFromReader");
            CHECK(firstFrame, "frame 0 never arrived");
            CHECK(sentByFirstFrame < static_cast<int64_t>(remote->data.size()),
                  "frame 0 only arrived after all " << remote->data.size() << " bytes");
            CHECK(lastFrame, "playback never reached the last frame");
        });

        add("a seek through a range reader jumps to the Cues", [](const Fixture& fx) {
            auto remote = std::make_shared<SlowRemote>(fx.path);
            remote->delay = std::chrono::microseconds(4000);
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            config.streamPrefetchAhead = 2;
            WebMAlphaPlayer p;
            p.setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(p);
            CHECK(p.loadFromReader(slowReader(remote, 512)), "loadFromReader");
            CHECK(waiter.waitFor(0, std::chrono::seconds(5)), "frame 0 never arrived");
            const int target = fx.card.frames - 5;
            p.seekToFrame(target);
            bool shown = waiter.waitFor(target, std::chrono::seconds(10));
            std::vector<int64_t> opens = remote->openPositions();
            p.unload();
            CHECK(shown, "seekToFrame(" << target << ") showed no such frame");
            const int64_t size = static_cast<int64_t>(remote->data.size());
            CHECK(opens.size() >= 2 && *std::max_element(opens.begin(), opens.end()) > size / 2,
                  opens.size() << " opens, none in the second half of " << size << " bytes");
        });

        add("plays while the basic input downloads the file", [](const Fixture& fx) {
            auto remote = std::make_shared<SlowRemote>(fx.path);
            // Everything but the last byte, until frame 0 shows
            remote->holdAt = static_cast<int64_t>(remote->data.size()) - 1;
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            WebMAlphaPlayer p;
            p.setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(p);
            bool loaded = p.loadFromReader(slowDownload(remote));
            if (loaded) p.play();
            bool firstFrame = loaded && waiter.waitFor(0, std::chrono::seconds(5));
            remote->release();
            bool lastFrame = firstFrame && waiter.waitFor(fx.card.frames - 1, std::chrono::seconds(20));
            std::vector<int64_t> opens = remote->openPositions();
            p.unload();
            CHECK(loaded, "loadFromReader");
            CHECK(firstFrame, "frame 0 didn't show before the file's last byte arrived");
            CHECK(lastFrame, "playback never reached the last frame");
            CHECK(opens.size() == 1, opens.size() << " opens");
        });

        add("a seek through the basic input waits for its bytes", [](const Fixture& fx) {
            auto remote = std::make_shared<SlowRemote>(fx.path);
            remote->delay = std::chrono::microseconds(4000);
            PlayerConfig config;
            config.mode = PlaybackMode::Stream;
            config.loopPlayback = false;
            config.streamPrefetchAhead = 2;
            WebMAlphaPlayer p;
            p.setConfig(config);
            FrameWaiter waiter;
            waiter.card = fx.card;
            waiter.attach(p);
            CHECK(p.loadFromReader(slowDownload(remote)), "loadFromReader");
            CHECK(waiter.waitFor(0, std::chrono::seconds(5)), "frame 0 never arrived");
            const int target = fx.card.frames - 5;
            p.seekToFrame(target);
            bool shown = waiter.waitFor(target, std::chrono::seconds(20));
            std::vector<int64_t> opens = remote->openPositions();
            p.unload();
            CHECK(shown, "seekToFrame(" << target << ") showed no such frame");
            CHECK(opens.size() == 1, opens.size() << " opens");
        });
    }

    tests().push_back({"yuvToRgbFor maps black, white and the matrices", [] {
        auto apply = [](const YuvToRgb& m, double y, double u, double v, int c) {
            double in[4] = {y / 255, u / 255, v / 255, 1};
            double sum = 0;
            for (int k = 0; k < 4; k++) sum += m.m[k * 4 + c] * in[k];
            return sum * 255;
        };
        auto near = [](double a, double b) { return std::abs(a - b) < 0.05; };
        const int tv = kRangeLimited, pc = kRangeFull, unspecified = kMatrixUnspecified;
        const int bt709 = kMatrixBt709, bt601 = kMatrixSmpte170m;
        YuvToRgb limited601 = yuvToRgbFor(tv, bt601, 383, 217);
        YuvToRgb full601 = yuvToRgbFor(pc, bt601, 383, 217);
        for (int c = 0; c < 3; c++) {
            CHECK(near(apply(limited601, 16, 128, 128, c), 0), "limited black, channel " << c);
            CHECK(near(apply(limited601, 235, 128, 128, c), 255), "limited white, channel " << c);
            CHECK(near(apply(full601, 0, 128, 128, c), 0), "full black, channel " << c);
            CHECK(near(apply(full601, 255, 128, 128, c), 255), "full white, channel " << c);
        }
        // Limited-range BT.601 red (R'G'B' 255,0,0) is Y'CbCr 81, 90, 240; BT.709 red is 63, 102, 240.
        CHECK(std::abs(apply(limited601, 81, 90, 240, 0) - 255) < 1.5, apply(limited601, 81, 90, 240, 0));
        CHECK(std::abs(apply(limited601, 81, 90, 240, 1)) < 1.5, apply(limited601, 81, 90, 240, 1));
        YuvToRgb limited709 = yuvToRgbFor(tv, bt709, 1920, 1080);
        CHECK(std::abs(apply(limited709, 63, 102, 240, 0) - 255) < 1.5, apply(limited709, 63, 102, 240, 0));
        CHECK(std::abs(apply(limited709, 63, 102, 240, 2)) < 1.5, apply(limited709, 63, 102, 240, 2));
        // Untagged: BT.601 up to 720x576, BT.709 above
        YuvToRgb untaggedHd = yuvToRgbFor(kRangeUnspecified, unspecified, 1920, 1080);
        YuvToRgb untaggedSd = yuvToRgbFor(kRangeUnspecified, unspecified, 640, 480);
        CHECK(std::memcmp(&untaggedHd, &limited709, sizeof(YuvToRgb)) == 0, "untagged HD is limited BT.709");
        CHECK(std::memcmp(&untaggedSd, &limited601, sizeof(YuvToRgb)) == 0, "untagged SD is limited BT.601");
    }});

    tests().push_back({"demuxer: a file without alpha starts at its first keyframe", [] {
        int fd = ::open((std::string(FIXTURES_DIR) + "/vp9_10bit.webm").c_str(), O_RDONLY);
        CHECK(fd >= 0, "vp9_10bit.webm");
        std::unique_ptr<ByteSource> source = openFileRange(fd, 0, -1);
        ::close(fd);
        std::unique_ptr<AlphaDemuxer> demuxer = AlphaDemuxer::open(std::move(source));
        CHECK(demuxer != nullptr, "open");
        CHECK(!demuxer->info().hasAlpha, "hasAlpha");
        AlphaDemuxer::Packet p;
        CHECK(demuxer->readPacket(&p), "no packets");
        CHECK(p.stream == 0 && p.key && p.ptsUs == 0, "first packet: stream " << p.stream << ", key " << p.key << ", ptsUs " << p.ptsUs);
        int count = 1;
        while (demuxer->readPacket(&p)) count++;
        CHECK(count == 3, count << " packets");
        CHECK(!demuxer->failed(), "the end of the file reads as a failure");
    }});

    tests().push_back({"AV1 sequence headers match dav1d's", [] {
        std::vector<std::string> files;
        for (const Fixture& f : fixtures()) {
            if (f.name.find("av1") != std::string::npos) files.push_back(f.path);
        }
        const std::string headersDir = std::string(FIXTURES_DIR) + "/av1_headers";
        if (DIR* dir = opendir(headersDir.c_str())) {
            while (dirent* entry = readdir(dir)) {
                const std::string name = entry->d_name;
                if (name.size() > 5 && name.compare(name.size() - 5, 5, ".webm") == 0) files.push_back(headersDir + "/" + name);
            }
            closedir(dir);
        }
        CHECK(files.size() >= 16, files.size() << " AV1 files; run tests/fixtures/av1_headers/make.sh");

        std::set<std::string> seen;
        int headers = 0;
        // `data` holds OBUs; ours must agree with dav1d's on whether there is a valid header, and on its fields
        auto compare = [&](const std::string& what, const uint8_t* data, size_t size) {
            Dav1dSequenceHeader d;
            const bool dav1dOk = dav1d_parse_sequence_header(&d, data, size) == 0;
            Av1SequenceHeader ours;
            const bool oursOk = parseAv1SequenceHeader(data, size, &ours);
            CHECK(oursOk == dav1dOk, what << ": parsed " << oursOk << ", dav1d " << dav1dOk);
            if (!dav1dOk) return;
            headers++;
            CHECK(ours.profile == d.profile, what << ": profile " << ours.profile << " vs " << int(d.profile));
            CHECK(ours.bitDepth == 8 + 2 * d.hbd, what << ": bit depth " << ours.bitDepth << " vs hbd " << int(d.hbd));
            CHECK(ours.monochrome == (d.monochrome != 0), what << ": monochrome");
            CHECK(ours.subsamplingX == d.ss_hor && ours.subsamplingY == d.ss_ver,
                  what << ": subsampling " << ours.subsamplingX << "," << ours.subsamplingY << " vs " << int(d.ss_hor) << "," << int(d.ss_ver));
            CHECK(ours.colorPrimaries == d.pri && ours.transferCharacteristics == d.trc && ours.matrixCoefficients == d.mtrx,
                  what << ": colour " << ours.colorPrimaries << "/" << ours.transferCharacteristics << "/" << ours.matrixCoefficients
                       << " vs " << d.pri << "/" << d.trc << "/" << d.mtrx);
            CHECK(ours.fullRange == (d.color_range != 0), what << ": range");
            CHECK(ours.maxWidth == d.max_width && ours.maxHeight == d.max_height, what << ": size");
            seen.insert("profile " + std::to_string(d.profile));
            seen.insert("bit depth " + std::to_string(ours.bitDepth));
            seen.insert("subsampling " + std::to_string(d.ss_hor) + std::to_string(d.ss_ver));
            if (d.monochrome) seen.insert("monochrome");
            if (d.mtrx == DAV1D_MC_IDENTITY) seen.insert("identity matrix");
            if (d.color_description_present) seen.insert("colour description");
            if (d.timing_info_present) seen.insert("timing info");
            if (d.decoder_model_info_present) seen.insert("decoder model info");
            if (d.reduced_still_picture_header) seen.insert("reduced still picture header");
        };

        for (const std::string& path : files) {
            int fd = ::open(path.c_str(), O_RDONLY);
            CHECK(fd >= 0, path);
            std::unique_ptr<AlphaDemuxer> demuxer = AlphaDemuxer::open(openFileRange(fd, 0, -1));
            ::close(fd);
            CHECK(demuxer != nullptr, path);
            const AlphaDemuxer::Info info = demuxer->info();
            std::vector<const AlphaDemuxer::StreamInfo*> streams = {&info.color};
            if (info.hasAlpha) streams.push_back(&info.alpha);
            std::vector<std::vector<uint8_t>> firstPackets(streams.size());
            AlphaDemuxer::Packet p;
            while (demuxer->readPacket(&p)) {
                if (p.stream < static_cast<int>(streams.size()) && firstPackets[p.stream].empty()) {
                    firstPackets[p.stream].assign(p.data, p.data + p.size);
                }
            }
            for (size_t i = 0; i < streams.size(); i++) {
                CHECK(streams[i]->codec == AlphaDemuxer::AV1, path << " stream " << i << " isn't AV1");
                const std::string what = path.substr(path.rfind('/', path.rfind('/') - 1) + 1) + " stream " + std::to_string(i);
                compare(what + " first packet", firstPackets[i].data(), firstPackets[i].size());
                // av1C's configOBUs are optional (rav1e leaves them out); the demuxer then reads the first packet
                const std::vector<uint8_t>& av1C = streams[i]->codecPrivate;
                if (av1C.size() <= 4) {
                    seen.insert("av1C without OBUs");
                    continue;
                }
                compare(what + " av1C", av1C.data() + 4, av1C.size() - 4);
                // Cut short anywhere, a header parses only if dav1d's does too, and the same
                for (size_t n = 0; n < av1C.size() - 4; n++) compare(what + " av1C cut to " + std::to_string(n), av1C.data() + 4, n);
            }
        }
        for (const char* branch : {"profile 0", "profile 1", "profile 2", "bit depth 8", "bit depth 10", "bit depth 12",
                                   "subsampling 11", "subsampling 10", "subsampling 00", "monochrome", "identity matrix",
                                   "colour description", "timing info", "decoder model info", "reduced still picture header",
                                   "av1C without OBUs"}) {
            CHECK(seen.count(branch), "no file has " << branch);
        }
        CHECK(headers > 0, "no headers");
    }});

    tests().push_back({"the library versions name libvpx and dav1d", [] {
        const std::string versions = WebMAlphaPlayer::getLibraryVersions();
        CHECK(versions.find("libvpx v") == 0 && versions.find(", dav1d ") != std::string::npos, versions);
    }});

    tests().push_back({"10-bit VP9 is refused with an error event", [] {
        WebMAlphaPlayer p;
        std::string error;
        p.setEventCallback([&](const std::string& event, const std::string& data) {
            if (event == "error") error = data;
        });
        CHECK(!p.loadFromFile(std::string(FIXTURES_DIR) + "/vp9_10bit.webm"), "a 10-bit VP9 file loaded");
        CHECK(error.find("10-bit") != std::string::npos, "error: " << error);
        CHECK(!p.isLoaded(), "isLoaded() after a failed load");
    }});

    tests().push_back({"FrameCache evicts the least recently used", [] {
        FrameCache cache(3);
        for (const char* key : {"a", "b", "c"}) cache.put(key, std::make_shared<YUVAFrame>());
        CHECK(cache.get("a") != nullptr, "a");
        cache.put("d", std::make_shared<YUVAFrame>());
        CHECK(!cache.contains("b"), "b should have been evicted");
        CHECK(cache.contains("a") && cache.contains("c") && cache.contains("d"), "a, c and d kept");
        cache.setMaxSize(2);
        CHECK(cache.size() == 2, cache.size());
        CHECK(!cache.contains("c"), "c is now the oldest");
    }});
}

int main(int argc, char** argv) {
    std::string error;
    if (!registerDecoderPlugins(alpha_video_vpx_plugins, &error) ||
        !registerDecoderPlugins(alpha_video_av1_plugins, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    registerTests();
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0, known = 0, fixed = 0;
    for (const Test& t : tests()) {
        if (filter && t.name.find(filter) == std::string::npos) continue;
        const KnownFailure* bug = knownFailure(t.name);
        std::string error;
        try {
            t.run();
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (error.empty() && !bug) {
            passed++;
        } else if (error.empty()) {
            fixed++;
            printf("NOW PASSES  %s\n            (remove it from kKnownFailures: %s)\n", t.name.c_str(), bug->bug);
        } else if (bug && error.find(bug->symptom) != std::string::npos) {
            known++;
            printf("known bug   %s\n            %s\n", t.name.c_str(), error.c_str());
        } else {
            failed++;
            printf("FAIL        %s\n            %s\n", t.name.c_str(), error.c_str());
        }
    }
    printf("\n%d passed, %d failed, %d known bugs, %d known bugs now passing\n", passed, failed, known, fixed);
    return failed || fixed ? 1 : 0;
}
