/**
 * WebMAlphaPlayer.cpp
 * High-performance VP9+Alpha WebM player implementation
 * 
 * Reads WebM with AlphaDemuxer (libwebm's parser) and decodes with libvpx and dav1d directly.
 * No dependency on platform-specific wrappers.
 */

#include "WebMAlphaPlayer.h"
#include "AlphaDemuxer.h"
#include "ByteSource.h"
#include "RangeReader.h"
#include "VideoDecoder.h"

#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <deque>
#include <map>

#ifdef __APPLE__
#include <stdio.h>
#define LOG_TAG "WebMAlphaPlayer"
#define LOGI(...) do { fprintf(stderr, "[%s] ", LOG_TAG); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#define LOGE(...) do { fprintf(stderr, "[%s ERROR] ", LOG_TAG); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#elif defined(__ANDROID__)
#include <android/log.h>
#define LOG_TAG "WebMAlphaPlayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

#if !defined(LOGI)
#define LOGI(...) printf(__VA_ARGS__); printf("\n")
#endif
#if !defined(LOGE)
#define LOGE(...) fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")
#endif

// Player lifecycle and stats; only when the player's verbose flag is on. Errors always log.
#define VLOGI(...) do { if (verbose_) LOGI(__VA_ARGS__); } while (0)

// Frame/cache debugging dumps, compiled in only with WEBM_DEBUG_LOGS
#if defined(WEBM_DEBUG_LOGS)
#define DLOGI(...) LOGI(__VA_ARGS__)
#else
#define DLOGI(...) do {} while (0)
#endif

namespace webm {

//==============================================================================
// YUVAFrame Implementation
//==============================================================================

YUVAFrame::YUVAFrame()
    : yPlane(nullptr), uPlane(nullptr), vPlane(nullptr), aPlane(nullptr)
    , ySize(0), uSize(0), vSize(0), aSize(0)
    , width(0), height(0), pts(0), timeBase(0.0), frameIndex(0)
    , yStride(0), uvStride(0), aStride(0)
{}

YUVAFrame::~YUVAFrame() {
    std::free(yPlane);
    std::free(uPlane);
    std::free(vPlane);
    std::free(aPlane);
}

YUVAFrame::YUVAFrame(YUVAFrame&& other) noexcept
    : yPlane(other.yPlane), uPlane(other.uPlane)
    , vPlane(other.vPlane), aPlane(other.aPlane)
    , ySize(other.ySize), uSize(other.uSize)
    , vSize(other.vSize), aSize(other.aSize)
    , width(other.width), height(other.height)
    , pts(other.pts), timeBase(other.timeBase)
    , frameIndex(other.frameIndex)
    , yStride(other.yStride), uvStride(other.uvStride), aStride(other.aStride)
{
    other.yPlane = nullptr;
    other.uPlane = nullptr;
    other.vPlane = nullptr;
    other.aPlane = nullptr;
}

YUVAFrame& YUVAFrame::operator=(YUVAFrame&& other) noexcept {
    if (this != &other) {
        // Free existing memory
        std::free(yPlane);
        std::free(uPlane);
        std::free(vPlane);
        std::free(aPlane);
        
        // Move from other
        yPlane = other.yPlane;
        uPlane = other.uPlane;
        vPlane = other.vPlane;
        aPlane = other.aPlane;
        ySize = other.ySize;
        uSize = other.uSize;
        vSize = other.vSize;
        aSize = other.aSize;
        width = other.width;
        height = other.height;
        pts = other.pts;
        timeBase = other.timeBase;
        frameIndex = other.frameIndex;
        yStride = other.yStride;
        uvStride = other.uvStride;
        aStride = other.aStride;
        
        other.yPlane = nullptr;
        other.uPlane = nullptr;
        other.vPlane = nullptr;
        other.aPlane = nullptr;
    }
    return *this;
}

bool YUVAFrame::allocate(int w, int h) {
    width = w;
    height = h;
    
    // 4:2:0 chroma is half resolution, rounded up for odd sizes
    const int uvW = (w + 1) / 2;
    const int uvH = (h + 1) / 2;
    ySize = w * h;
    uSize = uvW * uvH;
    vSize = uvW * uvH;
    aSize = w * h;
    
    yStride = w;
    uvStride = uvW;
    aStride = w;
    
    yPlane = static_cast<uint8_t*>(std::malloc(ySize));
    uPlane = static_cast<uint8_t*>(std::malloc(uSize));
    vPlane = static_cast<uint8_t*>(std::malloc(vSize));
    aPlane = static_cast<uint8_t*>(std::malloc(aSize));
    
    return yPlane && uPlane && vPlane && aPlane;
}

// The planes and textures are 8-bit: deeper samples (10- and 12-bit AV1, 16 bits each) are
// rounded to 8 bits, which keeps the range, 64-940 in 10-bit becoming 16-235. Taking every
// stepX-th sample of every stepY-th row brings 4:2:2 and 4:4:4 chroma down to 4:2:0.
static void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src, int srcStride, int w, int h, int depth,
                      int stepX = 1, int stepY = 1) {
    if (depth <= 8 && stepX == 1) {
        for (int y = 0; y < h; ++y) memcpy(dst + y * dstStride, src + static_cast<ptrdiff_t>(y) * stepY * srcStride, w);
        return;
    }
    const int shift = depth > 8 ? depth - 8 : 0;
    const int half = shift ? 1 << (shift - 1) : 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<ptrdiff_t>(y) * stepY * srcStride;
        uint8_t* out = dst + y * dstStride;
        if (depth <= 8) {
            for (int x = 0; x < w; ++x) out[x] = row[x * stepX];
            continue;
        }
        const uint16_t* row16 = reinterpret_cast<const uint16_t*>(row);
        for (int x = 0; x < w; ++x) out[x] = static_cast<uint8_t>(std::min((row16[x * stepX] + half) >> shift, 255));
    }
}

// Fills a frame's Y, U and V planes from a decoded colour picture, and its alpha opaque.
static void copyColour(YUVAFrame& frame, const Picture& picture) {
    const int w = std::min(picture.width, frame.width);
    const int h = std::min(picture.height, frame.height);
    const int uvW = (w + 1) / 2;
    const int uvH = (h + 1) / 2;
    copyPlane(frame.yPlane, frame.yStride, picture.planes[0], picture.strides[0], w, h, picture.bitDepth);
    if (picture.monochrome) {
        for (int y = 0; y < uvH; ++y) {
            memset(frame.uPlane + y * frame.uvStride, 128, uvW);
            memset(frame.vPlane + y * frame.uvStride, 128, uvW);
        }
    } else {
        const int stepX = 2 >> picture.chromaShiftX;
        const int stepY = 2 >> picture.chromaShiftY;
        copyPlane(frame.uPlane, frame.uvStride, picture.planes[1], picture.strides[1], uvW, uvH, picture.bitDepth, stepX, stepY);
        copyPlane(frame.vPlane, frame.uvStride, picture.planes[2], picture.strides[2], uvW, uvH, picture.bitDepth, stepX, stepY);
    }
    memset(frame.aPlane, 255, frame.aSize);
}

static std::shared_ptr<YUVAFrame> frameFromPicture(const Picture& picture, int index) {
    auto frame = std::make_shared<YUVAFrame>();
    if (!frame->allocate(picture.width, picture.height)) {
        LOGE("Failed to allocate frame buffer for %dx%d at frame %d", picture.width, picture.height, index);
        return nullptr;
    }
    frame->frameIndex = index;
    frame->pts = picture.ptsUs;
    frame->timeBase = 1e-6;
    copyColour(*frame, picture);
    return frame;
}

// An alpha frame, 8-bit and 0-255, kept until the colour frame with its timestamp is decoded.
// The decoder's picture is overwritten by its next frame, so the plane is copied out.
struct AlphaPlane {
    int64_t ptsUs = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> samples;  // Empty for a block without alpha, which shows opaque
};

// A YUV alpha stream in limited range holds transparent as 16 and opaque as 235, so it is
// expanded to 0-255. An RGB one (alpha in G, plane 0), a full-range one and BlockAdditional alpha
// (`asIs`) are 0-255 already.
static AlphaPlane alphaPlaneOf(const Picture& picture, bool asIs) {
    AlphaPlane alpha;
    alpha.ptsUs = picture.ptsUs;
    alpha.width = picture.width;
    alpha.height = picture.height;
    alpha.samples.resize(static_cast<size_t>(picture.width) * picture.height);
    copyPlane(alpha.samples.data(), picture.width, picture.planes[0], picture.strides[0], picture.width,
              picture.height, picture.bitDepth);
    if (asIs || picture.rgb || picture.colorRange == kRangeFull) return alpha;
    static const auto expand = [] {
        std::array<uint8_t, 256> lut{};
        for (int v = 0; v < 256; v++) {
            lut[v] = static_cast<uint8_t>(std::clamp((v - 16) * 255 / 219.0 + 0.5, 0.0, 255.0));
        }
        return lut;
    }();
    for (uint8_t& v : alpha.samples) v = expand[v];
    return alpha;
}

static void copyAlpha(YUVAFrame& frame, const AlphaPlane& alpha) {
    if (alpha.samples.empty()) return;  // Opaque, as copyColour left it
    const int w = std::min(alpha.width, frame.width);
    const int h = std::min(alpha.height, frame.height);
    for (int y = 0; y < h; ++y) {
        memcpy(frame.aPlane + y * frame.aStride, alpha.samples.data() + static_cast<size_t>(y) * alpha.width, w);
    }
}

double YUVAFrame::getDurationMs() const {
    if (timeBase > 0) {
        return (pts * timeBase) * 1000.0;
    }
    return 0.0;
}

//==============================================================================
// FrameCache Implementation
//==============================================================================

FrameCache::FrameCache(size_t maxFrames) : maxSize_(maxFrames) {
    DLOGI("🗄️ FrameCache created with maxSize=%zu", maxSize_);
}

void FrameCache::put(const std::string& key, std::shared_ptr<YUVAFrame> frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = lruMap_.find(key);
    if (it != lruMap_.end()) {
        // Key exists - update and move to front
        cache_[key] = frame;
        lruList_.erase(it->second);
        lruList_.push_front(key);
        lruMap_[key] = lruList_.begin();
    } else {
        // New key - insert
        if (cache_.size() >= maxSize_) {
            // In stream mode, the prefetch loop should have already called evictFurthestBehind
            // But for preload mode or edge cases, use simple LRU eviction
            evictOldest();
        }
        cache_[key] = frame;
        lruList_.push_front(key);
        lruMap_[key] = lruList_.begin();
        
        // Log cache size periodically during preload
        if (cache_.size() % 50 == 0) {
            DLOGI("📊 Cache now has %zu frames (max=%zu)", cache_.size(), maxSize_);
        }
    }
}

std::shared_ptr<YUVAFrame> FrameCache::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return nullptr;
    }
    
    // Move to front (most recently used)
    auto lruIt = lruMap_[key];
    lruList_.erase(lruIt);
    lruList_.push_front(key);
    lruMap_[key] = lruList_.begin();
    
    return it->second;
}

void FrameCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    lruList_.clear();
    lruMap_.clear();
}

size_t FrameCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

bool FrameCache::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.find(key) != cache_.end();
}

void FrameCache::evict(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return;
    
    // Remove from LRU tracking
    auto lruIt = lruMap_.find(key);
    if (lruIt != lruMap_.end()) {
        lruList_.erase(lruIt->second);
        lruMap_.erase(lruIt);
    }
    
    cache_.erase(it);
}

void FrameCache::setMaxSize(size_t maxFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxSize_ = maxFrames;
    
    // Evict excess frames if needed
    while (cache_.size() > maxSize_) {
        evictOldest();
    }
}

void FrameCache::evictOldest() {
    if (lruList_.empty()) return;
    
    std::string oldestKey = lruList_.back();
    lruList_.pop_back();
    lruMap_.erase(oldestKey);
    cache_.erase(oldestKey);
}

size_t FrameCache::maxSize() const {
    return maxSize_;
}

bool FrameCache::isFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size() >= maxSize_;
}

bool FrameCache::evictFurthestBehind(int playbackPos, int totalFrames, const std::string& keyPrefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (cache_.empty() || totalFrames <= 0) return false;
    
    // Find the frame that is furthest behind playbackPos in circular sense
    std::string furthestKey;
    int furthestDist = 0;  // Most negative distance = furthest behind
    
    for (const auto& pair : cache_) {
        // Extract frame number from key (format: "frame_123")
        size_t underscorePos = pair.first.find('_');
        if (underscorePos == std::string::npos) continue;
        
        int frameNum = std::stoi(pair.first.substr(underscorePos + 1));
        
        // Calculate circular distance: positive = ahead, negative = behind
        int dist = frameNum - playbackPos;
        // Normalize to [-totalFrames/2, totalFrames/2] for wraparound
        if (dist > totalFrames / 2) dist -= totalFrames;
        if (dist < -totalFrames / 2) dist += totalFrames;
        
        // Track the frame with the most negative distance (furthest behind)
        if (furthestKey.empty() || dist < furthestDist) {
            furthestDist = dist;
            furthestKey = pair.first;
        }
    }
    
    if (!furthestKey.empty() && furthestDist < 0) {  // Evict if behind playback
        // Remove from LRU tracking
        auto lruIt = lruMap_.find(furthestKey);
        if (lruIt != lruMap_.end()) {
            lruList_.erase(lruIt->second);
            lruMap_.erase(lruIt);
        }
        cache_.erase(furthestKey);
        return true;
    }
    
    return false;
}

//==============================================================================
// DecoderStats Implementation
//==============================================================================

double DecoderStats::getAverageDecodeTimeMs() const {
    if (framesDecoded == 0) return 0.0;
    return static_cast<double>(decodeTimeUs) / framesDecoded / 1000.0;
}

double DecoderStats::getCacheHitRate() const {
    int64_t total = cacheHits + cacheMisses;
    if (total == 0) return 0.0;
    return static_cast<double>(cacheHits) / total * 100.0;
}

void DecoderStats::reset() {
    framesDecoded = 0;
    framesCached = 0;
    cacheHits = 0;
    cacheMisses = 0;
    decodeTimeUs = 0;
    peakMemoryBytes = 0;
}

//==============================================================================
// WebMAlphaPlayer Implementation
//==============================================================================

WebMAlphaPlayer::WebMAlphaPlayer() {
    frameCache_ = std::make_unique<FrameCache>(config_.cacheSize);
}

WebMAlphaPlayer::~WebMAlphaPlayer() {
    unload();
}

void WebMAlphaPlayer::setConfig(const PlayerConfig& config) {
    std::lock_guard<std::mutex> lock(configMutex_);
    VLOGI("⚙️ setConfig called: cacheSize=%d, preloadAllFrames=%d", config.cacheSize, config.preloadAllFrames);
    
    // Check if cache size is actually changing
    size_t oldCacheSize = config_.cacheSize;
    config_ = config;
    
    // The loop reads the atomic, never the struct, so the config's speed has to be mirrored
    // across or a rate passed at construction would be silently ignored.
    playbackSpeed_ = config.playbackSpeed > 0.01 ? config.playbackSpeed : 0.01;
    
    // Only recreate cache if size changed AND cache is empty
    // This preserves preloaded frames from decodeAllFrames()
    if (frameCache_) {
        size_t currentCacheFrames = frameCache_->size();
        if (currentCacheFrames > 0) {
            VLOGI("⚙️ Preserving existing cache with %zu frames (not recreating)", currentCacheFrames);
        } else if (config_.cacheSize != oldCacheSize) {
            VLOGI("⚙️ Recreating empty cache: %zu -> %d", oldCacheSize, config_.cacheSize);
            frameCache_ = std::make_unique<FrameCache>(config_.cacheSize);
        }
    }
}

PlayerConfig WebMAlphaPlayer::getConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

bool WebMAlphaPlayer::loadFromFile(const std::string& path) {
    unload();
    
    // Reset stop flag so decodeAllFrames doesn't exit immediately
    shouldStop_ = false;
    ioAbort_ = false;
    
    std::unique_ptr<ByteSource> source = openFile(path);
    if (!source) {
        emitEvent("error", std::string("Failed to open file: ") + std::strerror(errno));
        return false;
    }
    return finishLoad(std::move(source));
}

bool WebMAlphaPlayer::loadFromFileRange(int fd, int64_t offset, int64_t length) {
    unload();
    
    // Reset stop flag so decodeAllFrames doesn't exit immediately
    shouldStop_ = false;
    ioAbort_ = false;
    
    std::unique_ptr<ByteSource> source = openFileRange(fd, offset, length);
    if (!source) {
        emitEvent("error", "Failed to open file range");
        return false;
    }
    return finishLoad(std::move(source));
}

bool WebMAlphaPlayer::loadFromReader(std::shared_ptr<avsource::RangeReader> reader) {
    unload();
    
    shouldStop_ = false;
    ioAbort_ = false;
    
    if (!reader) {
        emitEvent("error", "No source to read");
        return false;
    }
    reader_ = reader;
    return finishLoad(openReader(std::move(reader), &ioAbort_));
}

bool WebMAlphaPlayer::finishLoad(std::unique_ptr<ByteSource> source) {
    demuxer_ = AlphaDemuxer::open(std::move(source));
    if (!demuxer_) {
        std::string readError = reader_ ? reader_->lastError() : std::string();
        emitEvent("error", readError.empty() ? "Failed to open video: not a WebM with a VP9 or AV1 track"
                                             : "Reading the video failed: " + readError);
        return false;
    }

    if (!openStreams()) {
        return false;
    }
    
    if (!initCodecs()) {
        return false;
    }
    
    loaded_ = true;
    emitEvent("loaded", std::to_string(totalFrames_) + " frames");
    
    // Reset first pass flag
    firstPassComplete_ = false;
    
    // Handle playback modes
    if (config_.mode == PlaybackMode::Stream) {
        // Stream mode: use smaller cache, sequential background decode, evict old frames
        int streamCacheSize = config_.streamCacheSize > 0 ? config_.streamCacheSize : 30;
        frameCache_->setMaxSize(streamCacheSize);
        VLOGI("📺 Stream mode enabled: cacheSize=%d, prefetchAhead=%d", 
             streamCacheSize, config_.streamPrefetchAhead);
        
        // Reset streaming decode position
        streamDecodePos_ = 0;
        prefetchTarget_ = 0;
        // Frame 0 goes through the frame callback once decoded, so a clip that isn't played
        // straight away still shows its first frame.
        pendingSeekFrame_ = 0;
        
        // Start background sequential decode thread immediately
        prefetchThread_ = std::make_unique<std::thread>(&WebMAlphaPlayer::prefetchLoop, this);
        
        // Wait briefly for initial frames to be decoded
        auto startWait = std::chrono::high_resolution_clock::now();
        int minFrames = std::min(5, totalFrames_);
        while (streamDecodePos_.load() < minFrames && !shouldStop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - startWait);
            if (elapsed.count() > 500) {
                VLOGI("📺 Initial decode timeout after 500ms, continuing with %d frames", 
                     streamDecodePos_.load());
                break;
            }
        }
        VLOGI("📺 Initial decode ready: %d frames", streamDecodePos_.load());
    } else if (config_.mode == PlaybackMode::StreamAndPreload) {
        // StreamAndPreload (hybrid) mode: start playing immediately, cache ALL frames
        // After first pass completes, loop from cache without re-decoding
        frameCache_->setMaxSize(totalFrames_ + 10);  // Cache all frames
        VLOGI("📺 StreamAndPreload mode enabled: cacheSize=%d (all frames), prefetchAhead=%d", 
             totalFrames_ + 10, config_.streamPrefetchAhead);
        
        // Reset streaming decode position
        streamDecodePos_ = 0;
        prefetchTarget_ = 0;
        pendingSeekFrame_ = 0;  // as in Stream mode: frame 0 is shown without play()
        
        // Start background sequential decode thread - will decode ALL frames
        prefetchThread_ = std::make_unique<std::thread>(&WebMAlphaPlayer::prefetchLoop, this);
        
        // DON'T wait for initial frames here - return immediately to avoid blocking
        // the global decode queue when multiple players are spawned simultaneously.
        // The playback loop will handle frame-not-ready gracefully by waiting/retrying.
        VLOGI("📺 StreamAndPreload started (non-blocking), decode thread running in background");
    } else {
        // Preload mode: decode all frames upfront before playback
        decodeAllFrames();
    }
    
    return true;
}

bool WebMAlphaPlayer::loadFromMemory(const uint8_t* data, size_t size) {
    unload();
    
    // Reset stop flag so decodeAllFrames doesn't exit immediately
    shouldStop_ = false;
    ioAbort_ = false;
    
    // A copy: the caller's buffer may go away
    return finishLoad(openMemory(data, size));
}

void WebMAlphaPlayer::unload() {
    // Protect against concurrent unload calls (e.g., during view detach/reattach)
    std::lock_guard<std::mutex> unloadLock(unloadMutex_);
    
    ioAbort_ = true;
    stop();
    
    // Signal prefetch thread to stop
    prefetchCV_.notify_all();
    
    // Wait for any in-progress decode operations (including synchronous decodeAllFrames)
    // Use a timeout to prevent ANR - if decode is stuck, shouldStop_ flag should
    // eventually cause it to exit. We proceed with cleanup after timeout.
    {
        // Try to acquire lock with 500ms timeout - enough for normal operations,
        // but short enough to not cause ANR (5 second threshold)
        if (!decodeMutex_.try_lock_for(std::chrono::milliseconds(500))) {
            VLOGI("unload: decodeMutex_ acquisition timed out after 500ms - proceeding anyway");
            // shouldStop_ is already set, decodeAllFrames will exit when it checks
        } else {
            // Lock acquired normally - immediately release since we just needed to wait
            decodeMutex_.unlock();
        }
    }
    
    // Wait for threads to finish - must check both pointer and joinable state
    // to avoid crash when unload is called multiple times
    if (decodeThread_) {
        if (decodeThread_->joinable()) {
            decodeThread_->join();
        }
        decodeThread_.reset();
    }
    if (playbackThread_) {
        if (playbackThread_->joinable()) {
            playbackThread_->join();
        }
        playbackThread_.reset();
    }
    if (prefetchThread_) {
        if (prefetchThread_->joinable()) {
            prefetchThread_->join();
        }
        prefetchThread_.reset();
    }
    prefetchTarget_ = -1;
    
    closeCodecs();
    demuxer_.reset();
    reader_.reset();
    
    if (frameCache_) {
        frameCache_->clear();
    }
    
    width_ = height_ = totalFrames_ = 0;
    duration_ = frameRate_ = 0.0;
    loaded_ = false;
    currentFrame_ = 0;
}

bool WebMAlphaPlayer::openStreams() {
    const AlphaDemuxer::Info& info = demuxer_->info();
    
    const AlphaDemuxer::StreamInfo* tracks[] = {
        &info.color, info.hasAlpha && !info.alphaInBlockAdditional ? &info.alpha : nullptr};
    for (int track = 0; track < 2; track++) {
        if (!tracks[track]) continue;
        std::string whyNot;
        if (!findDecoderPlugin(*tracks[track], info.width, info.height, &whyNot)) {
            emitError(kErrorNoDecoder, std::string(track ? "The alpha" : "The colour") + " track can't play: " + whyNot);
            return false;
        }
    }
    VLOGI("Colour: %s profile %d, %d-bit; alpha: %s",
          info.color.codec == AlphaDemuxer::AV1   ? "AV1"
          : info.color.codec == AlphaDemuxer::VP8 ? "VP8"
                                                  : "VP9",
          info.color.profile, info.color.bitDepth,
          !info.hasAlpha ? "none" : info.alphaInBlockAdditional ? "BlockAdditional" : "track");
    
    width_ = info.width;
    height_ = info.height;
    {
        std::lock_guard<std::mutex> lock(yuvToRgbMutex_);
        yuvToRgb_ = yuvToRgbFor(info.color.colorRange, info.color.colorSpace, width_, height_);
    }
    VLOGI("Colour: range=%d matrix=%d", info.color.colorRange, info.color.colorSpace);
    
    frameRate_ = info.frameRate > 0 ? info.frameRate : 30.0;
    duration_ = static_cast<double>(info.durationUs) / 1e6;
    totalFrames_ = info.totalFrames > 0 ? info.totalFrames : 100;
    startUs_ = info.startUs;
    alphaAsIs_ = info.alphaInBlockAdditional;
    
    return true;
}

bool WebMAlphaPlayer::initCodecs() {
    const AlphaDemuxer::Info& info = demuxer_->info();
    std::string error;
    colorDecoder_ = VideoDecoder::create(info.color, info.width, info.height, config_.decodeThreads, &error);
    if (!colorDecoder_) {
        emitEvent("error", "Failed to open the colour decoder (" + error + ")");
        return false;
    }
    std::string name = colorDecoder_->name();
    if (info.hasAlpha) {
        alphaDecoder_ = VideoDecoder::create(info.alpha, info.width, info.height, config_.decodeThreads, &error);
        if (!alphaDecoder_) {
            emitEvent("error", "Failed to open the alpha decoder (" + error + ")");
            return false;
        }
        if (name != alphaDecoder_->name()) name += std::string(", ") + alphaDecoder_->name();
    }
    VLOGI("Decoders: %s, %d threads each", name.c_str(), config_.decodeThreads);
    std::lock_guard<std::mutex> lock(decoderNameMutex_);
    decoderName_ = name;
    return true;
}

std::string WebMAlphaPlayer::getDecoderName() const {
    std::lock_guard<std::mutex> lock(decoderNameMutex_);
    return decoderName_;
}

void WebMAlphaPlayer::closeCodecs() {
    colorDecoder_.reset();
    alphaDecoder_.reset();
}

std::shared_ptr<YUVAFrame> WebMAlphaPlayer::decodeFrame(int index) {
    // Check cache first (with null safety)
    if (!frameCache_) {
        return nullptr;
    }
    
    std::string cacheKey = makeFrameKey(index);
    auto cachedFrame = frameCache_->get(cacheKey);
    if (cachedFrame) {
        stats_.cacheHits++;
        return cachedFrame;
    }
    stats_.cacheMisses++;
    
    // Acquire decode mutex - the demuxer and decoders are NOT thread-safe
    // This prevents concurrent access during preload (decodeAllFrames) or other decodes
    std::lock_guard<std::timed_mutex> decodeLock(decodeMutex_);
    
    // Double-check cache after acquiring lock - another thread may have decoded this frame
    cachedFrame = frameCache_->get(cacheKey);
    if (cachedFrame) {
        stats_.cacheHits++;
        return cachedFrame;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Decode from the keyframe before the frame, matching frames by timestamp: the demuxer lands
    // on that keyframe, and decoders with frame delay (dav1d) return frames late.
    demuxer_->seekToTimeUs(startUs_ + static_cast<int64_t>(std::llround(index / frameRate_ * 1e6)));
    colorDecoder_->flush();
    if (alphaDecoder_) {
        alphaDecoder_->flush();
    }
    
    std::shared_ptr<YUVAFrame> result;
    AlphaPlane alphaForIndex;
    bool haveAlpha = false;
    Picture picture;
    
    auto reachesIndex = [&](int64_t ptsUs) { return frameIndexForPts(ptsUs) >= index; };
    auto takeColour = [&] {
        while (!result && colorDecoder_->receive(&picture)) {
            if (!reachesIndex(picture.ptsUs)) continue;
            result = frameFromPicture(picture, index);
            if (!result) return false;
        }
        return true;
    };
    auto takeAlpha = [&] {
        while (!haveAlpha && alphaDecoder_->receive(&picture)) {
            if (!reachesIndex(picture.ptsUs)) continue;
            alphaForIndex = alphaPlaneOf(picture, alphaAsIs_);
            haveAlpha = true;
        }
    };
    auto done = [&] { return result && (!alphaDecoder_ || haveAlpha); };
    
    AlphaDemuxer::Packet packet;
    bool failed = false;
    while (!done() && !failed && !shouldStop_) {
        if (!demuxer_->readPacket(&packet)) {
            if (!result) {
                colorDecoder_->drain();
                failed = !takeColour();
            }
            if (alphaDecoder_ && !haveAlpha) {
                alphaDecoder_->drain();
                takeAlpha();
            }
            break;
        }
        if (packet.stream == 0 && !result) {
            if (colorDecoder_->send(packet.data, packet.size, packet.ptsUs)) failed = !takeColour();
        } else if (packet.stream == 1 && alphaDecoder_ && !haveAlpha) {
            if (packet.size == 0) {
                // A block without alpha: its frame shows opaque
                haveAlpha = reachesIndex(packet.ptsUs);
            } else if (alphaDecoder_->send(packet.data, packet.size, packet.ptsUs)) {
                takeAlpha();
            }
        }
    }
    
    // A frame whose alpha stream ended early stays opaque, as copyColour left it
    if (result && haveAlpha) copyAlpha(*result, alphaForIndex);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    stats_.decodeTimeUs += duration.count();
    stats_.framesDecoded++;
    
    // Cache the frame (with null safety)
    if (result && frameCache_ && !shouldStop_) {
        frameCache_->put(cacheKey, result);
        stats_.framesCached++;
    }
    
    return result;
}

void WebMAlphaPlayer::decodeAllFrames() {
    // Hold decode mutex for the entire operation - unload() will wait for this
    std::lock_guard<std::timed_mutex> decodeLock(decodeMutex_);
    
    // Check if we should stop before starting (unload might have been called)
    if (shouldStop_) {
        VLOGI("decodeAllFrames: Aborted - shouldStop_ already set");
        return;
    }
    preloading_ = true;
    
    emitEvent("preload_start", std::to_string(totalFrames_) + " frames");
    
    // Ensure cache can hold all frames
    if (totalFrames_ > config_.cacheSize) {
        VLOGI("Resizing frame cache from %d to %d to fit all frames", config_.cacheSize, totalFrames_ + 10);
        config_.cacheSize = totalFrames_ + 10;
        frameCache_ = std::make_unique<FrameCache>(config_.cacheSize);
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Reset to beginning
    demuxer_->seekToStart();
    colorDecoder_->flush();
    if (alphaDecoder_) {
        alphaDecoder_->flush();
    }
    
    int frameIndex = 0;
    int alphaFrameIndex = 0;  // Track alpha frames separately (NOT static!)
    
    // Two-stream files: getFrame() can hand out a cached frame while this runs, so a colour frame
    // is cached only once the alpha frame with its index is in. Whichever decodes first waits here.
    std::map<int, std::shared_ptr<YUVAFrame>> colourAwaitingAlpha;
    std::map<int, AlphaPlane> alphaAwaitingColour;
    auto cacheFrame = [&](int index, const std::shared_ptr<YUVAFrame>& frame) {
        frameCache_->put(makeFrameKey(index), frame);
        stats_.framesDecoded++;
        stats_.framesCached++;
    };
    auto addColour = [&](int index, const std::shared_ptr<YUVAFrame>& frame) {
        auto alpha = alphaAwaitingColour.find(index);
        if (alpha != alphaAwaitingColour.end()) {
            copyAlpha(*frame, alpha->second);
            alphaAwaitingColour.erase(alpha);
        } else if (alphaDecoder_) {
            colourAwaitingAlpha[index] = frame;
            return;
        }
        cacheFrame(index, frame);
    };
    auto addAlpha = [&](int index, AlphaPlane alpha) {
        auto colour = colourAwaitingAlpha.find(index);
        if (colour == colourAwaitingAlpha.end()) {
            alphaAwaitingColour[index] = std::move(alpha);
            return;
        }
        copyAlpha(*colour->second, alpha);
        cacheFrame(index, colour->second);
        colourAwaitingAlpha.erase(colour);
    };
    
    Picture picture;
    auto takeColourFrames = [&] {
        while (!shouldStop_ && colorDecoder_->receive(&picture)) {
            if (frameIndex == 0) {
                VLOGI("First decoded frame: %dx%d, %d-bit, chroma shift %d,%d%s", picture.width, picture.height,
                      picture.bitDepth, picture.chromaShiftX, picture.chromaShiftY, picture.rgb ? ", RGB" : "");
                // Update stored dimensions if needed
                width_ = picture.width;
                height_ = picture.height;
            }
            auto yuvFrame = frameFromPicture(picture, frameIndex);
            if (!yuvFrame) continue;
            // Check before cache access - unload() may have been called
            if (shouldStop_ || !frameCache_) break;
            addColour(frameIndex, yuvFrame);
            frameIndex++;
        }
    };
    auto takeAlphaFrames = [&] {
        while (!shouldStop_ && frameCache_ && alphaDecoder_->receive(&picture)) {
            if (alphaFrameIndex == 0) {
                VLOGI("First alpha frame: %dx%d, %d-bit, range=%d", picture.width, picture.height, picture.bitDepth,
                      picture.colorRange);
            }
            addAlpha(alphaFrameIndex++, alphaPlaneOf(picture, alphaAsIs_));
        }
    };
    
    AlphaDemuxer::Packet packet;
    while (!shouldStop_) {
        if (!demuxer_->readPacket(&packet)) {
            // End of file or error - drain the decoders
            colorDecoder_->drain();
            takeColourFrames();
            if (alphaDecoder_) {
                alphaDecoder_->drain();
                takeAlphaFrames();
            }
            break;
        }
        
        if (packet.stream == 0) {
            if (colorDecoder_->send(packet.data, packet.size, packet.ptsUs)) takeColourFrames();
        } else if (alphaDecoder_) {
            if (packet.size == 0) {
                // A block without alpha: its frame shows opaque
                AlphaPlane opaque;
                opaque.ptsUs = packet.ptsUs;
                addAlpha(alphaFrameIndex++, std::move(opaque));
            } else if (alphaDecoder_->send(packet.data, packet.size, packet.ptsUs)) {
                takeAlphaFrames();
            }
        }
    }
    
    // A colour frame whose alpha stream ended early shows opaque, as copyColour left it
    if (!shouldStop_ && frameCache_) {
        for (auto& [index, frame] : colourAwaitingAlpha) cacheFrame(index, frame);
    }
    
    // Update total frames to actual count
    totalFrames_ = frameIndex;
    preloading_ = false;
    
    // getFrame() returned nothing while this ran, so a paused view (at load, or after a seek)
    // has nothing on screen yet.
    if (!playing_ && !shouldStop_ && frameIndex > 0) {
        int shown = std::min(currentFrame_.load(), frameIndex - 1);
        currentFrame_ = shown;
        emitFrame(getCachedFrame(shown));
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Log final cache stats (check frameCache_ not destroyed)
    if (frameCache_ && !shouldStop_) {
        VLOGI("📊 Preload finished: decoded %d frames, cache size=%zu (max=%d)", 
             frameIndex, frameCache_->size(), config_.cacheSize);
        
        emitEvent("preload_complete", 
                  std::to_string(frameIndex) + " frames in " + 
                  std::to_string(duration.count()) + "ms");
    } else {
        VLOGI("📊 Preload interrupted: decoded %d frames before stop", frameIndex);
    }
}

void WebMAlphaPlayer::play() {
    if (!loaded_) return;
    
    // If already playing, nothing to do
    if (playing_) return;
    
    playing_ = true;
    shouldStop_ = false;
    pendingSeekFrame_ = -1;  // The playback loop shows the sought frame from here on
    
    // Only create a new thread if one isn't already running
    // (pause just sets playing_ = false, thread keeps running). After a clip that doesn't loop
    // has ended, its thread has returned but is still joinable.
    if (playbackThread_ && playbackThread_->joinable() && !playbackLoopRunning_) {
        playbackThread_->join();
        playbackThread_.reset();
    }
    if (!playbackThread_ || !playbackThread_->joinable()) {
        playbackLoopRunning_ = true;
        playbackThread_ = std::make_unique<std::thread>(&WebMAlphaPlayer::playbackLoop, this);
    }
    
    emitEvent("play");
}

void WebMAlphaPlayer::pause() {
    playing_ = false;
    emitEvent("pause");
}

void WebMAlphaPlayer::stop() {
    shouldStop_ = true;
    playing_ = false;
    currentFrame_ = 0;
    
    decodeCV_.notify_all();
    
    if (playbackThread_ && playbackThread_->joinable()) {
        playbackThread_->join();
    }
    playbackThread_.reset();
    
    emitEvent("stop");
}

void WebMAlphaPlayer::seekTo(double timeSeconds) {
    if (!(timeSeconds > 0)) timeSeconds = 0;  // Also catches NaN
    // The epsilon keeps a time computed as frame / fps from rounding down to the frame before.
    seekToFrame(static_cast<int>(std::floor(timeSeconds * frameRate_ + 1e-6)));
}

void WebMAlphaPlayer::setPlaybackSpeed(double speed) {
    // A rate at or below zero would make the frame interval infinite or negative. Pausing is
    // `pause()`; this clamps to something slow rather than wedging the playback thread.
    const double clamped = speed > 0.01 ? speed : 0.01;
    playbackSpeed_ = clamped;
    config_.playbackSpeed = clamped;
    VLOGI("🎬 Playback speed set to %.3f", clamped);
}

double WebMAlphaPlayer::getPlaybackSpeed() const {
    return playbackSpeed_.load();
}

void WebMAlphaPlayer::seekToFrame(int frameIndex) {
    if (!loaded_ || totalFrames_ <= 0) return;
    frameIndex = std::max(0, std::min(frameIndex, totalFrames_ - 1));
    currentFrame_ = frameIndex;
    
    if (config_.mode == PlaybackMode::Preload) {
        auto frame = getFrame(frameIndex);
        if (frame) emitFrame(frame);
    } else if (auto frame = getCachedFrame(frameIndex)) {
        pendingSeekFrame_ = -1;
        emitFrame(frame);
    } else {
        // The prefetch thread owns the decoder, so it decodes the frame and, if still paused by
        // then, shows it. StreamAndPreload decodes the whole clip in order, so it only waits.
        pendingSeekFrame_ = frameIndex;
        int decodePos = streamDecodePos_.load();
        int prefetchAhead = config_.streamPrefetchAhead > 0 ? config_.streamPrefetchAhead : 15;
        bool decodingTowardsIt = frameIndex >= decodePos && frameIndex - decodePos < prefetchAhead;
        if (config_.mode == PlaybackMode::Stream && !decodingTowardsIt) {
            seekRequest_ = frameIndex;
        }
        requestPrefetch(frameIndex);
    }
    
    emitEvent("seek", std::to_string(frameIndex));
}

int WebMAlphaPlayer::frameIndexForPts(int64_t ptsUs) const {
    return static_cast<int>(std::llround((ptsUs - startUs_) / 1e6 * frameRate_));
}

void WebMAlphaPlayer::playbackLoop() {
    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    
    VLOGI("🎬 Playback loop started: frameRate=%.2f, speed=%.2f", 
         frameRate_, playbackSpeed_.load());
    
    int framesEmitted = 0;
    auto loopStartTime = std::chrono::high_resolution_clock::now();
    
    while (!shouldStop_ && loaded_) {
        if (!playing_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        
        /**
         * Read per tick, not once before the loop.
         *
         * The rate can change while playing — that is the whole point of `setPlaybackSpeed` —
         * and a duration hoisted out of the loop would pin the speed to whatever it was when
         * playback happened to start. Only the interval changes; the frame index still advances
         * by one per tick below, so speeding up never moves the playhead.
         */
        const double frameDurationMs = 1000.0 / (frameRate_ * playbackSpeed_.load());
        
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration<double, std::milli>(now - lastFrameTime);
        
        if (elapsed.count() >= frameDurationMs) {
            const int shownFrame = currentFrame_.load();
            int frameIdx = shownFrame;
            auto frame = getFrame(frameIdx);
            
            if (frame) {
                emitFrame(frame);
                framesEmitted++;
                
                // Log FPS every 30 frames
                if (framesEmitted % 30 == 0) {
                    auto totalElapsed = std::chrono::duration<double>(now - loopStartTime).count();
                    double actualFps = framesEmitted / totalElapsed;
                    VLOGI("🎬 Playback stats: frame=%d, emitted=%d, actualFPS=%.1f, targetFPS=%.1f",
                         frameIdx, framesEmitted, actualFps, frameRate_);
                }
            } else if (preloading_ || config_.mode == PlaybackMode::Stream || config_.mode == PlaybackMode::StreamAndPreload) {
                // Frame not decoded yet - don't advance, wait for the preload or prefetch
                VLOGI("🎬 Frame %d not ready, waiting for prefetch (decode at %d)", frameIdx, streamDecodePos_.load());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;  // Try again without advancing frame
            }
            
            // In streaming modes, request async prefetch and evict old frames
            // (evictOldFrames is a no-op in StreamAndPreload mode)
            if (config_.mode == PlaybackMode::Stream || config_.mode == PlaybackMode::StreamAndPreload) {
                requestPrefetch(frameIdx);  // Non-blocking - signals background thread
                evictOldFrames(frameIdx);
            }
            
            // Advance frame
            frameIdx++;
            if (frameIdx >= totalFrames_) {
                if (config_.loopPlayback) {
                    frameIdx = 0;
                    // In streaming modes, request prefetch from start on loop
                    // (In StreamAndPreload, frames are already cached so this is just a hint)
                    if (config_.mode == PlaybackMode::Stream || config_.mode == PlaybackMode::StreamAndPreload) {
                        requestPrefetch(0);  // Non-blocking
                    }
                    // Reset FPS tracking on loop
                    framesEmitted = 0;
                    loopStartTime = now;
                    emitEvent("loop");
                } else {
                    playing_ = false;
                    emitEvent("ended");
                    break;
                }
            }
            // A seek during this tick has already moved the playhead; keep it.
            int expected = shownFrame;
            currentFrame_.compare_exchange_strong(expected, frameIdx);
            lastFrameTime = now;
        } else {
            // Sleep until next frame is due (avoid busy waiting)
            auto sleepTime = frameDurationMs - elapsed.count();
            if (sleepTime > 1.0) {
                std::this_thread::sleep_for(std::chrono::microseconds(
                    static_cast<int64_t>((sleepTime - 1.0) * 1000)
                ));
            }
        }
    }
    
    VLOGI("🎬 Playback loop ended: shouldStop=%d, loaded=%d", shouldStop_.load(), loaded_.load());
    playbackLoopRunning_ = false;
}

void WebMAlphaPlayer::streamingPrefetch(int currentFrame) {
    if (!frameCache_ || !loaded_) return;
    
    int prefetchAhead = config_.streamPrefetchAhead > 0 ? config_.streamPrefetchAhead : 15;
    int endFrame = std::min(currentFrame + prefetchAhead, totalFrames_);
    
    // Handle loop wraparound
    if (config_.loopPlayback && endFrame < currentFrame + prefetchAhead) {
        // Prefetch remaining frames from start
        int wrapFrames = (currentFrame + prefetchAhead) - totalFrames_;
        for (int i = 0; i < wrapFrames && !shouldStop_; i++) {
            std::string key = makeFrameKey(i);
            if (!frameCache_->contains(key)) {
                decodeFrame(i);
            }
        }
    }
    
    // Prefetch frames ahead of current position
    for (int i = currentFrame + 1; i < endFrame && !shouldStop_; i++) {
        std::string key = makeFrameKey(i);
        if (!frameCache_->contains(key)) {
            decodeFrame(i);
        }
    }
}

void WebMAlphaPlayer::evictOldFrames(int currentFrame) {
    if (!frameCache_ || !loaded_ || totalFrames_ <= 0) return;
    
    // In StreamAndPreload (hybrid) mode, never evict frames - we want to keep all of them
    if (config_.mode == PlaybackMode::StreamAndPreload) {
        return;
    }
    
    // Check each cached frame and evict if behind playback
    // This keeps exactly prefetchAhead frames in cache (current + ahead)
    for (int i = 0; i < totalFrames_ && !shouldStop_; i++) {
        std::string key = makeFrameKey(i);
        if (!frameCache_->contains(key)) continue;
        
        // Calculate circular distance from currentFrame to frame i
        // Positive = ahead of playback, negative = behind playback
        int dist = i - currentFrame;
        if (config_.loopPlayback) {
            // Normalize to [-totalFrames/2, totalFrames/2] for proper wraparound
            if (dist > totalFrames_ / 2) dist -= totalFrames_;
            if (dist < -totalFrames_ / 2) dist += totalFrames_;
        }
        
        // Evict frames that are behind playback (already played)
        if (dist < 0) {
            frameCache_->evict(key);
        }
    }
}

void WebMAlphaPlayer::requestPrefetch(int currentFrame) {
    // Non-blocking: just update the target and signal the prefetch thread
    prefetchTarget_ = currentFrame;
    prefetchCV_.notify_one();
}

void WebMAlphaPlayer::prefetchLoop() {
    VLOGI("📺 Prefetch thread started (sequential decode): shouldStop=%d, loaded=%d, mode=%d", 
         shouldStop_.load(), loaded_.load(), static_cast<int>(config_.mode));
    
    // Acquire decode mutex - we'll hold it for sequential decoding
    std::lock_guard<std::timed_mutex> decodeLock(decodeMutex_);
    
    AlphaDemuxer::Packet packet;
    Picture picture;
    
    int frameIndex = streamDecodePos_.load();
    int prefetchAhead = config_.streamPrefetchAhead > 0 ? config_.streamPrefetchAhead : 15;
    bool isHybridMode = (config_.mode == PlaybackMode::StreamAndPreload);
    auto passStart = std::chrono::high_resolution_clock::now();
    
    VLOGI("📺 Starting sequential decode from frame %d, prefetchAhead=%d, hybrid=%d", 
         frameIndex, prefetchAhead, isHybridMode);
    
    // After a seek: the frames before seekTarget are decoded only to reach it, and the frame
    // index is re-read from the first frame's timestamp, since the demuxer lands on a keyframe.
    int seekTarget = 0;
    bool resyncFromPts = false;
    
    // Two-stream files: a colour frame isn't cached until the alpha frame with its timestamp is
    // decoded, so a streamed frame never shows without its alpha. Colour packets read while
    // looking for that alpha frame are held and decoded afterwards; alpha frames decoded ahead of
    // their colour frame wait in alphaAhead.
    struct HeldPacket {
        std::vector<uint8_t> data;
        int64_t ptsUs;
    };
    std::deque<HeldPacket> heldColourPackets;
    std::deque<AlphaPlane> alphaAhead;
    auto dropHeld = [&] {
        heldColourPackets.clear();
        alphaAhead.clear();
    };
    // <0: the alpha frame is for an earlier colour frame, 0: this one, >0: a later one.
    auto compareToColour = [&](const AlphaPlane& alpha, int64_t colourPtsUs) {
        double gapFrames = (alpha.ptsUs - colourPtsUs) / 1e6 * frameRate_;
        return gapFrames < -0.5 ? -1 : gapFrames > 0.5 ? 1 : 0;
    };
    auto keepAlphaFrames = [&] {
        Picture alphaPicture;
        while (alphaDecoder_->receive(&alphaPicture)) alphaAhead.push_back(alphaPlaneOf(alphaPicture, alphaAsIs_));
        while (alphaAhead.size() > 8) alphaAhead.pop_front();
    };
    // An alpha packet: decoded, or an empty one (a block without alpha) kept as an opaque frame
    auto takeAlphaPacket = [&](const AlphaDemuxer::Packet& p) {
        if (p.size == 0) {
            AlphaPlane opaque;
            opaque.ptsUs = p.ptsUs;
            alphaAhead.push_back(std::move(opaque));
        } else if (alphaDecoder_->send(p.data, p.size, p.ptsUs)) {
            keepAlphaFrames();
        }
    };
    // Decoders with frame delay (dav1d's frame threads) hold the last frames until they're
    // drained at the end of the file.
    bool colourDrained = false;
    bool alphaDrained = false;
    // After the demuxer has moved
    auto restartDecoding = [&] {
        colorDecoder_->flush();
        if (alphaDecoder_) {
            alphaDecoder_->flush();
        }
        dropHeld();
        colourDrained = false;
        alphaDrained = false;
    };
    auto attachAlpha = [&](YUVAFrame& frame, int64_t colourPtsUs) {
        AlphaDemuxer::Packet p;
        bool attached = false;
        while (!attached && !shouldStop_) {
            while (!alphaAhead.empty()) {
                int order = compareToColour(alphaAhead.front(), colourPtsUs);
                if (order > 0) break;
                if (order == 0) {
                    copyAlpha(frame, alphaAhead.front());
                    attached = true;
                }
                alphaAhead.pop_front();
                if (attached) break;
            }
            if (attached || !alphaAhead.empty()) break;
            if (!demuxer_->readPacket(&p)) {
                if (alphaDrained) break;
                alphaDrained = true;
                alphaDecoder_->drain();
                keepAlphaFrames();
                continue;
            }
            if (p.stream == 0) {
                heldColourPackets.push_back({std::vector<uint8_t>(p.data, p.data + p.size), p.ptsUs});
            } else {
                takeAlphaPacket(p);
            }
        }
    };
    
    // Turns every colour frame the decoder has ready into a cached frame
    auto takeColourFrames = [&](int playbackPos) {
        while (!shouldStop_ && colorDecoder_->receive(&picture)) {
            if (resyncFromPts) {
                resyncFromPts = false;
                frameIndex = frameIndexForPts(picture.ptsUs);
            }
            if (frameIndex < seekTarget) {
                frameIndex++;
                continue;
            }
            seekTarget = 0;
            
            // Check if we already have this frame cached
            std::string key = makeFrameKey(frameIndex);
            if (frameCache_ && !frameCache_->contains(key)) {
                const int64_t colourPtsUs = picture.ptsUs;
                if (auto yuvFrame = frameFromPicture(picture, frameIndex)) {
                    if (alphaDecoder_) attachAlpha(*yuvFrame, colourPtsUs);
                    
                    // Put frame in cache - eviction is handled by playback thread
                    // via evictOldFrames() after consuming each frame
                    frameCache_->put(key, yuvFrame);
                    stats_.framesDecoded++;
                    stats_.framesCached++;
                }
            }
            
            int sought = frameIndex;
            if (!playing_ && pendingSeekFrame_.compare_exchange_strong(sought, -1)) {
                emitFrame(getCachedFrame(frameIndex));
            }
            
            frameIndex++;
            streamDecodePos_ = frameIndex;
            
            // Log progress periodically
            if (frameIndex % 30 == 0) {
                VLOGI("📺 Decoded frame %d, playback at %d, cache size=%zu", 
                     frameIndex, playbackPos, frameCache_ ? frameCache_->size() : 0);
            }
        }
    };
    
    while (!shouldStop_ && loaded_) {
        int requested = seekRequest_.exchange(-1);
        if (requested >= 0) {
            demuxer_->seekToTimeUs(startUs_ + static_cast<int64_t>(std::llround(requested / frameRate_ * 1e6)));
            restartDecoding();
            frameIndex = requested;
            streamDecodePos_ = requested;
            seekTarget = requested;
            resyncFromPts = true;
            VLOGI("📺 Seek: decoding from the keyframe before frame %d", requested);
        }
        
        int playbackPos = prefetchTarget_.load();
        if (playbackPos < 0) playbackPos = 0;
        
        // In hybrid mode, once we've decoded all frames, we're done - no need to re-decode on loop
        if (isHybridMode && firstPassComplete_) {
            // All frames cached, just sleep and wait for stop signal
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        
        // In stream mode (not hybrid), check if we're far enough ahead and should PAUSE
        // Use circular arithmetic to handle wraparound at loop boundaries
        if (!isHybridMode) {
            // Calculate how far ahead decode is from playback (circular)
            int aheadBy = frameIndex - playbackPos;
            if (config_.loopPlayback && totalFrames_ > 0) {
                // Normalize to [0, totalFrames) range for circular comparison
                if (aheadBy < 0) aheadBy += totalFrames_;
                if (aheadBy > totalFrames_ / 2) {
                    // We're actually behind (wrapped around), not ahead
                    aheadBy = aheadBy - totalFrames_;
                }
            }
            
            // If we're sufficiently ahead, PAUSE and wait for playback to catch up
            if (aheadBy >= prefetchAhead) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }
        
        // In hybrid mode, decode to the end
        if (isHybridMode && frameIndex >= totalFrames_) {
            if (!firstPassComplete_) {
                firstPassComplete_ = true;
                VLOGI("📺 StreamAndPreload: First pass complete! All %d frames cached", totalFrames_);
                auto passMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - passStart).count();
                emitEvent("preload_complete", std::to_string(totalFrames_) + " frames in " +
                          std::to_string(passMs) + "ms");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        
        // Handle reaching end of file (stream mode only - hybrid handled above)
        if (frameIndex >= totalFrames_) {
            if (config_.loopPlayback) {
                // Stream mode: Seek back to start for looping
                demuxer_->seekToStart();
                restartDecoding();
                frameIndex = 0;
                streamDecodePos_ = 0;
                seekTarget = 0;
                VLOGI("📺 Prefetch reached EOF, looping back to frame 0 (cache preserved for playback)");
            } else {
                // Not looping, wait for playback to end
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
        }
        
        // Read and decode next frame sequentially (no seeking!)
        if (!heldColourPackets.empty()) {
            HeldPacket held = std::move(heldColourPackets.front());
            heldColourPackets.pop_front();
            if (colorDecoder_->send(held.data.data(), static_cast<int>(held.data.size()), held.ptsUs)) {
                takeColourFrames(playbackPos);
            }
            continue;
        }
        if (!demuxer_->readPacket(&packet)) {
            if (shouldStop_ || ioAbort_) break;
            // The input itself failed, e.g. the network went away: looping would replay only the
            // part that arrived, so stop decoding instead.
            if (demuxer_->failed()) {
                LOGE("📺 Read failed at frame %d, stopping decode", frameIndex);
                std::string why = reader_ ? reader_->lastError() : "";
                emitEvent("error", "Reading the video failed" + (why.empty() ? std::string() : ": " + why));
                break;
            }
            // End of file: take the frames the decoder still holds, then let the frame count
            // checks above loop or finish. A file with fewer frames than estimated loops here.
            if (!colourDrained) {
                colourDrained = true;
                colorDecoder_->drain();
                takeColourFrames(playbackPos);
                continue;
            }
            if (config_.loopPlayback) {
                demuxer_->seekToStart();
                restartDecoding();
                frameIndex = 0;
                streamDecodePos_ = 0;
                seekTarget = 0;
                VLOGI("📺 End of file, looping back to frame 0 (cache preserved)");
            } else {
                // Not looping: wait for playback to end
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            continue;
        }
        
        if (packet.stream == 0) {
            if (colorDecoder_->send(packet.data, packet.size, packet.ptsUs)) takeColourFrames(playbackPos);
        } else if (alphaDecoder_) {
            takeAlphaPacket(packet);
        }
    }
    
    VLOGI("📺 Prefetch thread stopped at frame %d", frameIndex);
}

std::shared_ptr<YUVAFrame> WebMAlphaPlayer::getFrame(int index) {
    if (!loaded_ || index < 0 || index >= totalFrames_ || !frameCache_) {
        return nullptr;
    }
    
    // Try cache first
    std::string key = makeFrameKey(index);
    auto frame = frameCache_->get(key);
    
    if (frame) {
        return frame;
    }
    
    // While the preload pass runs it holds the decoder and will cache this frame shortly.
    // Decoding on demand would wait for the whole pass, on the UI thread when called at load.
    if (preloading_) {
        return nullptr;
    }
    
    // In streaming modes (Stream or StreamAndPreload), don't block - return null and let prefetch thread catch up
    // The playback loop will show the last valid frame or skip
    if (config_.mode == PlaybackMode::Stream || config_.mode == PlaybackMode::StreamAndPreload) {
        // Request prefetch to catch up
        requestPrefetch(index);
        return nullptr;
    }
    
    // In preload mode, decode on demand (shouldn't happen if preloadAllFrames is true)
    return decodeFrame(index);
}

std::shared_ptr<YUVAFrame> WebMAlphaPlayer::getCachedFrame(int index) {
    if (!loaded_ || index < 0 || index >= totalFrames_ || !frameCache_) {
        return nullptr;
    }
    
    // Cache-only lookup - never triggers decoding
    // Safe to call from any thread
    std::string key = makeFrameKey(index);
    return frameCache_->get(key);
}

std::shared_ptr<YUVAFrame> WebMAlphaPlayer::getCurrentFrameData() {
    return getFrame(currentFrame_);
}

bool WebMAlphaPlayer::isLoaded() const { return loaded_; }
bool WebMAlphaPlayer::isPlaying() const { return playing_; }
int WebMAlphaPlayer::getCurrentFrame() const { return currentFrame_; }
double WebMAlphaPlayer::getCurrentTime() const { 
    return currentFrame_ / frameRate_; 
}
double WebMAlphaPlayer::getDuration() const { return duration_; }
int WebMAlphaPlayer::getTotalFrames() const { return totalFrames_; }
int WebMAlphaPlayer::getWidth() const { return width_; }
int WebMAlphaPlayer::getHeight() const { return height_; }
double WebMAlphaPlayer::getFrameRate() const { return frameRate_; }

YuvToRgb WebMAlphaPlayer::getYuvToRgb() const {
    std::lock_guard<std::mutex> lock(yuvToRgbMutex_);
    return yuvToRgb_;
}

void WebMAlphaPlayer::setFrameCallback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void WebMAlphaPlayer::setEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    eventCallback_ = std::move(callback);
}

DecoderStatsSnapshot WebMAlphaPlayer::getStats() const {
    return stats_.snapshot();
}

void WebMAlphaPlayer::resetStats() {
    stats_.reset();
}

void WebMAlphaPlayer::emitEvent(const std::string& event, const std::string& data) {
    if (event == "error") {
        std::lock_guard<std::mutex> lock(lastErrorCodeMutex_);
        lastErrorCode_.clear();
    }
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (eventCallback_) {
        eventCallback_(event, data);
    }
}

void WebMAlphaPlayer::emitError(const std::string& code, const std::string& message) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    {
        std::lock_guard<std::mutex> codeLock(lastErrorCodeMutex_);
        lastErrorCode_ = code;
    }
    if (eventCallback_) {
        eventCallback_("error", message);
    }
}

std::string WebMAlphaPlayer::getLastErrorCode() const {
    std::lock_guard<std::mutex> lock(lastErrorCodeMutex_);
    return lastErrorCode_;
}

void WebMAlphaPlayer::emitFrame(std::shared_ptr<YUVAFrame> frame) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (frameCallback_ && frame) {
        frameCallback_(frame);
    }
}

std::string WebMAlphaPlayer::makeFrameKey(int index) const {
    return std::to_string(index);
}

std::string WebMAlphaPlayer::getLibraryVersions() {
    return decoderLibraryVersions();
}

} // namespace webm
