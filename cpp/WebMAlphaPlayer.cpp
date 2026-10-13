/**
 * WebMAlphaPlayer.cpp
 * High-performance VP9+Alpha WebM player implementation
 * 
 * Uses FFmpeg C API directly for maximum performance.
 * No dependency on platform-specific wrappers.
 */

#include "WebMAlphaPlayer.h"
#include "FileRangeIO.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>

#ifdef __APPLE__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#define LOG_TAG "WebMAlphaPlayer"
#define LOGI(...) do { fprintf(stderr, "[%s] ", LOG_TAG); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#define LOGE(...) do { fprintf(stderr, "[%s ERROR] ", LOG_TAG); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#elif defined(__ANDROID__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#define LOG_TAG "WebMAlphaPlayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
// Desktop/other platforms
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
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

// Static initialization flag
static std::once_flag ffmpegInitFlag;
static bool ffmpegInitialized = false;

//==============================================================================
// Memory buffer context for custom AVIO reading
//==============================================================================

struct MemoryBufferContext {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

// Custom read callback for memory buffer
static int memoryReadPacket(void* opaque, uint8_t* buf, int buf_size) {
    MemoryBufferContext* ctx = static_cast<MemoryBufferContext*>(opaque);
    if (!ctx || !ctx->data) return AVERROR(EINVAL);
    
    size_t remaining = ctx->size - ctx->pos;
    if (remaining == 0) return AVERROR_EOF;
    
    size_t toRead = std::min(static_cast<size_t>(buf_size), remaining);
    memcpy(buf, ctx->data + ctx->pos, toRead);
    ctx->pos += toRead;
    
    return static_cast<int>(toRead);
}

// Custom seek callback for memory buffer
static int64_t memorySeek(void* opaque, int64_t offset, int whence) {
    MemoryBufferContext* ctx = static_cast<MemoryBufferContext*>(opaque);
    if (!ctx) return AVERROR(EINVAL);
    
    int64_t newPos;
    switch (whence) {
        case SEEK_SET:
            newPos = offset;
            break;
        case SEEK_CUR:
            newPos = static_cast<int64_t>(ctx->pos) + offset;
            break;
        case SEEK_END:
            newPos = static_cast<int64_t>(ctx->size) + offset;
            break;
        case AVSEEK_SIZE:
            return static_cast<int64_t>(ctx->size);
        default:
            return AVERROR(EINVAL);
    }
    
    if (newPos < 0 || static_cast<size_t>(newPos) > ctx->size) {
        return AVERROR(EINVAL);
    }
    
    ctx->pos = static_cast<size_t>(newPos);
    return newPos;
}

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
    // Use av_freep for FFmpeg-aligned memory
    if (yPlane) { av_freep(&yPlane); }
    if (uPlane) { av_freep(&uPlane); }
    if (vPlane) { av_freep(&vPlane); }
    if (aPlane) { av_freep(&aPlane); }
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
        if (yPlane) { av_freep(&yPlane); }
        if (uPlane) { av_freep(&uPlane); }
        if (vPlane) { av_freep(&vPlane); }
        if (aPlane) { av_freep(&aPlane); }
        
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

YuvToRgb yuvToRgbFor(int colorRange, int colorSpace, int width, int height) {
    // Luma and chroma weights of the matrix
    double kr = 0.299, kb = 0.114;  // BT.601
    bool sd = width <= 720 && height <= 576;
    if (colorSpace == AVCOL_SPC_BT709 || (colorSpace == AVCOL_SPC_UNSPECIFIED && !sd)) {
        kr = 0.2126; kb = 0.0722;
    } else if (colorSpace == AVCOL_SPC_BT2020_NCL || colorSpace == AVCOL_SPC_BT2020_CL) {
        kr = 0.2627; kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;
    const double rv = 2 * (1 - kr);
    const double bu = 2 * (1 - kb);
    const double gu = 2 * kb * (1 - kb) / kg;
    const double gv = 2 * kr * (1 - kr) / kg;

    // Y' = ys * (y - yo), U' = cs * (u - co), from the 0..1 texture samples
    const bool full = colorRange == AVCOL_RANGE_JPEG;
    const double ys = full ? 1.0 : 255.0 / 219.0, yo = full ? 0.0 : 16.0 / 255.0;
    const double cs = full ? 1.0 : 255.0 / 224.0, co = 128.0 / 255.0;

    // Rows R, G, B of [y u v 1], stored column-major
    const double rows[3][4] = {
        {ys, 0, rv * cs, -ys * yo - rv * cs * co},
        {ys, -gu * cs, -gv * cs, -ys * yo + (gu + gv) * cs * co},
        {ys, bu * cs, 0, -ys * yo - bu * cs * co},
    };
    YuvToRgb out{};
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 3; row++) out.m[col * 4 + row] = static_cast<float>(rows[row][col]);
    }
    out.m[15] = 1;
    return out;
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
    
    // Use av_malloc for SIMD-aligned memory (32-byte alignment)
    yPlane = static_cast<uint8_t*>(av_malloc(ySize));
    uPlane = static_cast<uint8_t*>(av_malloc(uSize));
    vPlane = static_cast<uint8_t*>(av_malloc(vSize));
    aPlane = static_cast<uint8_t*>(av_malloc(aSize));
    
    return yPlane && uPlane && vPlane && aPlane;
}

// Static counter for logging - only log first frame
static int copyFrameLogCount = 0;

void YUVAFrame::copyFromAVFrame(AVFrame* colorFrame, AVFrame* alphaFrame) {
    if (!colorFrame) return;
    
    // Safety check - ensure all planes are allocated
    if (!yPlane || !uPlane || !vPlane || !aPlane) {
        LOGE("copyFromAVFrame: plane buffers not allocated!");
        return;
    }
    
    // Log the pixel format for debugging (first frame only)
    bool shouldLog = (copyFrameLogCount++ == 0);
    if (shouldLog) {
        DLOGI("copyFromAVFrame: format=%d (YUVA420P=%d, YUV420P=%d), linesize[0]=%d, linesize[1]=%d, linesize[2]=%d, linesize[3]=%d, w=%d, h=%d",
             colorFrame->format, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P,
             colorFrame->linesize[0], colorFrame->linesize[1], colorFrame->linesize[2], colorFrame->linesize[3],
             colorFrame->width, colorFrame->height);
    }
    
    // Use actual frame dimensions (might differ from allocated)
    int frameW = colorFrame->width;
    int frameH = colorFrame->height;
    
    // Ensure we don't overflow our buffers
    int copyW = std::min(frameW, width);
    int copyH = std::min(frameH, height);
    
    // Copy Y plane (line by line to handle stride differences)
    for (int y = 0; y < copyH; ++y) {
        memcpy(yPlane + y * yStride, 
               colorFrame->data[0] + y * colorFrame->linesize[0],
               copyW);
    }
    
    // Copy U plane (half resolution for YUV420, rounded up)
    int uvWidth = (copyW + 1) / 2;
    int uvHeight = (copyH + 1) / 2;
    for (int y = 0; y < uvHeight; ++y) {
        memcpy(uPlane + y * uvStride,
               colorFrame->data[1] + y * colorFrame->linesize[1],
               uvWidth);
    }
    
    // Copy V plane
    for (int y = 0; y < uvHeight; ++y) {
        memcpy(vPlane + y * uvStride,
               colorFrame->data[2] + y * colorFrame->linesize[2],
               uvWidth);
    }
    
    // Handle alpha - check for YUVA420P format first (alpha in data[3] of same frame)
    bool hasBuiltInAlpha = (colorFrame->format == AV_PIX_FMT_YUVA420P) && 
                           colorFrame->data[3] && colorFrame->linesize[3] > 0;
    
    if (hasBuiltInAlpha) {
        // YUVA420P - alpha is in the same frame's data[3]
        if (shouldLog) {
            DLOGI("Using built-in alpha from YUVA420P frame, linesize[3]=%d, aStride=%d, copyW=%d, copyH=%d", 
                 colorFrame->linesize[3], aStride, copyW, copyH);
            // Log first row of alpha from source
            uint8_t* srcAlpha = colorFrame->data[3];
            DLOGI("Source alpha row 0 bytes 0-7: %02x %02x %02x %02x %02x %02x %02x %02x",
                 srcAlpha[0], srcAlpha[1], srcAlpha[2], srcAlpha[3],
                 srcAlpha[4], srcAlpha[5], srcAlpha[6], srcAlpha[7]);
            // Log middle of frame (where glow should be visible)
            int midRow = copyH / 2;
            int midCol = copyW / 2;
            uint8_t* midAlpha = colorFrame->data[3] + midRow * colorFrame->linesize[3] + midCol;
            DLOGI("Source alpha center (row %d, col %d): %02x %02x %02x %02x",
                 midRow, midCol, midAlpha[0], midAlpha[1], midAlpha[2], midAlpha[3]);
        }
        for (int y = 0; y < copyH; ++y) {
            memcpy(aPlane + y * aStride,
                   colorFrame->data[3] + y * colorFrame->linesize[3],
                   copyW);
        }
    } else if (alphaFrame && alphaFrame->data[0]) {
        copyAlphaFromAVFrame(alphaFrame);
    } else {
        // No alpha - fill with opaque (255)
        memset(aPlane, 255, aSize);
    }
}

void YUVAFrame::copyAlphaFromAVFrame(const AVFrame* alphaFrame) {
    // A YUV alpha stream in limited range holds transparent as 16 and opaque as 235, so it is
    // expanded to 0-255. An RGB one (gbrp, alpha in G, plane 0) is full range already.
    int alphaW = std::min(alphaFrame->width, width);
    int alphaH = std::min(alphaFrame->height, height);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(alphaFrame->format));
    bool limitedRange = alphaFrame->color_range != AVCOL_RANGE_JPEG && desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
    if (!limitedRange) {
        for (int y = 0; y < alphaH; ++y) {
            memcpy(aPlane + y * aStride, alphaFrame->data[0] + y * alphaFrame->linesize[0], alphaW);
        }
        return;
    }
    static const auto expand = [] {
        std::array<uint8_t, 256> lut{};
        for (int v = 0; v < 256; v++) {
            lut[v] = static_cast<uint8_t>(std::clamp((v - 16) * 255 / 219.0 + 0.5, 0.0, 255.0));
        }
        return lut;
    }();
    for (int y = 0; y < alphaH; ++y) {
        const uint8_t* src = alphaFrame->data[0] + y * alphaFrame->linesize[0];
        uint8_t* dst = aPlane + y * aStride;
        for (int x = 0; x < alphaW; ++x) dst[x] = expand[src[x]];
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
    initializeFFmpeg();
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
    
    // Use FFmpeg's native file handling - it's optimized and handles cleanup properly
    formatCtx_ = avformat_alloc_context();
    if (!formatCtx_) {
        emitEvent("error", "Failed to allocate format context");
        return false;
    }
    
    // Let FFmpeg handle file opening - it uses optimized I/O internally
    int ret = avformat_open_input(&formatCtx_, path.c_str(), nullptr, nullptr);
    
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        emitEvent("error", std::string("Failed to open file: ") + err);
        return false;
    }
    
    return finishLoad();
}

bool WebMAlphaPlayer::loadFromFileRange(int fd, int64_t offset, int64_t length) {
    unload();
    
    // Reset stop flag so decodeAllFrames doesn't exit immediately
    shouldStop_ = false;
    
    AVIOContext* io = openFileRangeIO(fd, offset, length);
    if (!io) {
        emitEvent("error", "Failed to open file range");
        return false;
    }
    return openCustomInput(io) && finishLoad();
}

bool WebMAlphaPlayer::loadFromDownload(const std::string& path) {
    unload();
    
    shouldStop_ = false;
    ioAbort_ = false;
    
    AVIOContext* io = openDownloadIO(path, &ioAbort_);
    if (!io) {
        emitEvent("error", "Failed to open " + path);
        return false;
    }
    return openCustomInput(io) && finishLoad();
}

bool WebMAlphaPlayer::openCustomInput(AVIOContext* io) {
    rangeIO_ = io;
    
    formatCtx_ = avformat_alloc_context();
    if (!formatCtx_) {
        emitEvent("error", "Failed to allocate format context");
        return false;
    }
    formatCtx_->pb = rangeIO_;
    formatCtx_->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    int ret = avformat_open_input(&formatCtx_, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        emitEvent("error", std::string("Failed to open video: ") + err);
        return false;
    }
    return true;
}

bool WebMAlphaPlayer::finishLoad() {
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
        
        // Start background sequential decode thread - will decode ALL frames
        prefetchThread_ = std::make_unique<std::thread>(&WebMAlphaPlayer::prefetchLoop, this);
        
        // DON'T wait for initial frames here - return immediately to avoid blocking
        // the global decode queue when multiple players are spawned simultaneously.
        // The playback loop will handle frame-not-ready gracefully by waiting/retrying.
        VLOGI("📺 StreamAndPreload started (non-blocking), decode thread running in background");
    } else {
        // Preload mode (default): decode all frames upfront before playback
        decodeAllFrames();
    }
    
    return true;
}

bool WebMAlphaPlayer::loadFromMemory(const uint8_t* data, size_t size) {
    unload();
    
    // Reset stop flag so decodeAllFrames doesn't exit immediately
    shouldStop_ = false;
    
    // Copy data (we need to own it)
    mappedData_ = static_cast<uint8_t*>(av_malloc(size));
    if (!mappedData_) {
        emitEvent("error", "Failed to allocate memory for video data");
        return false;
    }
    memcpy(mappedData_, data, size);
    mappedSize_ = size;
    useMmap_ = false;
    
    // Create memory buffer context for callbacks
    MemoryBufferContext* memCtx = new MemoryBufferContext();
    memCtx->data = mappedData_;
    memCtx->size = mappedSize_;
    memCtx->pos = 0;
    bufferCtx_ = memCtx;
    
    // Allocate I/O buffer for FFmpeg (must be av_malloc'd, FFmpeg takes ownership)
    const int ioBufferSize = 32768;  // 32KB buffer
    avioBuffer_ = static_cast<uint8_t*>(av_malloc(ioBufferSize));
    if (!avioBuffer_) {
        emitEvent("error", "Failed to allocate I/O buffer");
        return false;
    }
    
    // Create custom AVIOContext with read callback
    avioCtx_ = avio_alloc_context(
        avioBuffer_, ioBufferSize,
        0,              // write_flag = 0 (read only)
        memCtx,         // opaque context
        memoryReadPacket, // read callback
        nullptr,        // write callback (not needed)
        memorySeek      // seek callback
    );
    
    if (!avioCtx_) {
        emitEvent("error", "Failed to create AVIO context");
        return false;
    }
    
    // Create format context
    formatCtx_ = avformat_alloc_context();
    if (!formatCtx_) {
        emitEvent("error", "Failed to allocate format context");
        return false;
    }
    
    formatCtx_->pb = avioCtx_;
    formatCtx_->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    int ret = avformat_open_input(&formatCtx_, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        emitEvent("error", std::string("Failed to open video: ") + err);
        return false;
    }
    
    return finishLoad();
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
    
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    
    // Clean up custom I/O context (must be done AFTER closing format context)
    cleanupIO();
    
    unmapFile();
    
    if (frameCache_) {
        frameCache_->clear();
    }
    
    width_ = height_ = totalFrames_ = 0;
    duration_ = frameRate_ = 0.0;
    colorStreamIndex_ = alphaStreamIndex_ = -1;
    loaded_ = false;
    currentFrame_ = 0;
}

void WebMAlphaPlayer::cleanupIO() {
    // Clean up AVIO context (buffer is NOT freed by avio_context_free if we allocated it)
    if (avioCtx_) {
        // Note: avioBuffer_ was passed to avio_alloc_context and may have been freed
        // by avformat_close_input, or may be still held. We set it to null to be safe.
        // The buffer is managed by FFmpeg after avio_alloc_context is called.
        av_freep(&avioCtx_->buffer);  // Free buffer if FFmpeg didn't
        avio_context_free(&avioCtx_);
        avioCtx_ = nullptr;
    }
    avioBuffer_ = nullptr;  // Was freed above or by FFmpeg
    
    // Clean up our custom buffer context
    if (bufferCtx_) {
        delete static_cast<MemoryBufferContext*>(bufferCtx_);
        bufferCtx_ = nullptr;
    }
    
    closeFileRangeIO(&rangeIO_);
}

bool WebMAlphaPlayer::openStreams() {
    int ret = avformat_find_stream_info(formatCtx_, nullptr);
    if (ret < 0) {
        emitEvent("error", "Failed to find stream info");
        return false;
    }
    
    // Log stream info
    VLOGI("Found %d streams in file", formatCtx_->nb_streams);
    
    // Find video streams (WebM alpha might have 2 video streams, or 1 YUVA420P stream)
    for (unsigned i = 0; i < formatCtx_->nb_streams; ++i) {
        AVStream* stream = formatCtx_->streams[i];
        VLOGI("Stream %d: type=%d (video=%d), codec_id=%d", 
             i, stream->codecpar->codec_type, AVMEDIA_TYPE_VIDEO, stream->codecpar->codec_id);
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            VLOGI("  -> Video stream %d: codec_id=%d, pix_fmt=%d, %dx%d", 
                 i, stream->codecpar->codec_id, stream->codecpar->format,
                 stream->codecpar->width, stream->codecpar->height);
            if (colorStreamIndex_ < 0) {
                colorStreamIndex_ = i;
            } else if (alphaStreamIndex_ < 0) {
                alphaStreamIndex_ = i;
            }
        }
    }
    
    if (colorStreamIndex_ < 0) {
        emitEvent("error", "No video stream found");
        return false;
    }
    
    VLOGI("Using colorStreamIndex=%d, alphaStreamIndex=%d", colorStreamIndex_, alphaStreamIndex_);
    
    // Check for alpha side data in the stream
    AVStream* colorStream = formatCtx_->streams[colorStreamIndex_];
    
    // Log codec parameters
    VLOGI("Codec params: codec_id=%d, format=%d, profile=%d, level=%d",
         colorStream->codecpar->codec_id,
         colorStream->codecpar->format,
         colorStream->codecpar->profile,
         colorStream->codecpar->level);
    
    // Check for VP9 alpha (BlockAdditional in WebM)
    // VP9 Profile 1 or 3 can have alpha
    bool mayHaveAlpha = (colorStream->codecpar->codec_id == AV_CODEC_ID_VP9);
    if (mayHaveAlpha) {
        VLOGI("VP9 detected - checking for alpha capability");
        VLOGI("VP9 profile=%d (profile 1/3 support alpha in theory)", colorStream->codecpar->profile);
    }
    
    // Check stream side data for alpha indication
    for (int i = 0; i < colorStream->codecpar->nb_coded_side_data; i++) {
        const AVPacketSideData& sd = colorStream->codecpar->coded_side_data[i];
        VLOGI("Stream side_data[%d]: type=%d, size=%d", i, sd.type, (int)sd.size);
    }
    width_ = colorStream->codecpar->width;
    height_ = colorStream->codecpar->height;
    {
        std::lock_guard<std::mutex> lock(yuvToRgbMutex_);
        yuvToRgb_ = yuvToRgbFor(colorStream->codecpar->color_range, colorStream->codecpar->color_space, width_, height_);
    }
    VLOGI("Colour: range=%s matrix=%s",
          av_color_range_name(colorStream->codecpar->color_range),
          av_color_space_name(colorStream->codecpar->color_space));
    
    // Calculate frame rate
    if (colorStream->avg_frame_rate.den > 0) {
        frameRate_ = av_q2d(colorStream->avg_frame_rate);
    } else if (colorStream->r_frame_rate.den > 0) {
        frameRate_ = av_q2d(colorStream->r_frame_rate);
    } else {
        frameRate_ = 30.0;  // Default
    }
    
    // Calculate duration
    if (formatCtx_->duration > 0) {
        duration_ = static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
    } else if (colorStream->duration > 0) {
        timeBase_ = av_q2d(colorStream->time_base);
        duration_ = colorStream->duration * timeBase_;
    }
    
    // Estimate total frames
    totalFrames_ = static_cast<int>(duration_ * frameRate_);
    if (totalFrames_ <= 0) {
        totalFrames_ = colorStream->nb_frames > 0 ? colorStream->nb_frames : 100;
    }
    
    timeBase_ = av_q2d(colorStream->time_base);
    startPts_ = colorStream->start_time != AV_NOPTS_VALUE ? colorStream->start_time : 0;
    
    return true;
}

bool WebMAlphaPlayer::initCodecs() {
    // Find VP9 decoder (prefer libvpx-vp9 for alpha support)
    const AVCodec* vp9Codec = avcodec_find_decoder_by_name("libvpx-vp9");
    if (!vp9Codec) {
        VLOGI("libvpx-vp9 not found, falling back to generic VP9 decoder");
        vp9Codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
    } else {
        VLOGI("Using libvpx-vp9 decoder (supports alpha)");
    }
    
    if (!vp9Codec) {
        emitEvent("error", "VP9 codec not found");
        return false;
    }
    
    VLOGI("VP9 decoder: name=%s, id=%d, capabilities=0x%x", 
         vp9Codec->name, vp9Codec->id, vp9Codec->capabilities);
    
    // List supported pixel formats for the decoder
    if (vp9Codec->pix_fmts) {
        VLOGI("VP9 decoder supported pixel formats:");
        for (int i = 0; vp9Codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
            VLOGI("  pix_fmt[%d] = %d (YUVA420P=%d, YUV420P=%d)", 
                 i, vp9Codec->pix_fmts[i], AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P);
        }
    } else {
        VLOGI("VP9 decoder has no pix_fmts list (accepts any)");
    }
    
    // Init color codec
    colorCodecCtx_ = avcodec_alloc_context3(vp9Codec);
    if (!colorCodecCtx_) {
        emitEvent("error", "Failed to allocate color codec context");
        return false;
    }
    
    AVStream* colorStream = formatCtx_->streams[colorStreamIndex_];
    avcodec_parameters_to_context(colorCodecCtx_, colorStream->codecpar);
    
    // Set threading options
    colorCodecCtx_->thread_count = config_.decodeThreads;
    colorCodecCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    
    // Note: We do NOT set pix_fmt before opening the codec.
    // libvpx-vp9 will automatically detect alpha from BlockAdditional data
    // and output YUVA420P if alpha is present, or YUV420P if not.
    // Setting pix_fmt beforehand can cause crashes on some devices.
    
    if (avcodec_open2(colorCodecCtx_, vp9Codec, nullptr) < 0) {
        emitEvent("error", "Failed to open color codec");
        return false;
    }
    
    // Log the pixel format the codec will output
    VLOGI("Codec opened - pix_fmt=%d (YUVA420P=%d, YUV420P=%d)", 
         colorCodecCtx_->pix_fmt, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P);
    
    // copyFromAVFrame handles both YUVA420P (with alpha) and YUV420P (alpha filled with 255).
    
    // Init alpha codec if present
    if (alphaStreamIndex_ >= 0) {
        alphaCodecCtx_ = avcodec_alloc_context3(vp9Codec);
        if (!alphaCodecCtx_) {
            emitEvent("error", "Failed to allocate alpha codec context");
            return false;
        }
        
        AVStream* alphaStream = formatCtx_->streams[alphaStreamIndex_];
        avcodec_parameters_to_context(alphaCodecCtx_, alphaStream->codecpar);
        
        alphaCodecCtx_->thread_count = config_.decodeThreads;
        alphaCodecCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        
        if (avcodec_open2(alphaCodecCtx_, vp9Codec, nullptr) < 0) {
            emitEvent("error", "Failed to open alpha codec");
            return false;
        }
    }
    
    return true;
}

void WebMAlphaPlayer::closeCodecs() {
    if (colorCodecCtx_) {
        avcodec_free_context(&colorCodecCtx_);
        colorCodecCtx_ = nullptr;
    }
    if (alphaCodecCtx_) {
        avcodec_free_context(&alphaCodecCtx_);
        alphaCodecCtx_ = nullptr;
    }
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
    
    // Acquire decode mutex - FFmpeg contexts are NOT thread-safe
    // This prevents concurrent access during preload (decodeAllFrames) or other decodes
    std::lock_guard<std::timed_mutex> decodeLock(decodeMutex_);
    
    // Double-check cache after acquiring lock - another thread may have decoded this frame
    cachedFrame = frameCache_->get(cacheKey);
    if (cachedFrame) {
        stats_.cacheHits++;
        return cachedFrame;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Seek to frame position
    int64_t timestamp = static_cast<int64_t>(index / frameRate_ / timeBase_);
    av_seek_frame(formatCtx_, colorStreamIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
    
    // Flush decoder buffers after seek
    avcodec_flush_buffers(colorCodecCtx_);
    if (alphaCodecCtx_) {
        avcodec_flush_buffers(alphaCodecCtx_);
    }
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* colorFrame = av_frame_alloc();
    AVFrame* alphaFrame = av_frame_alloc();
    
    std::shared_ptr<YUVAFrame> result;
    int currentDecodedFrame = -1;
    
    while (av_read_frame(formatCtx_, packet) >= 0 && !shouldStop_) {
        if (packet->stream_index == colorStreamIndex_) {
            int ret = avcodec_send_packet(colorCodecCtx_, packet);
            if (ret >= 0) {
                ret = avcodec_receive_frame(colorCodecCtx_, colorFrame);
                if (ret >= 0) {
                    currentDecodedFrame++;
                    
                    if (currentDecodedFrame >= index) {
                        // This is the frame we want - use actual decoded dimensions
                        int frameW = colorFrame->width;
                        int frameH = colorFrame->height;
                        
                        result = std::make_shared<YUVAFrame>();
                        if (!result->allocate(frameW, frameH)) {
                            LOGE("Failed to allocate frame buffer for %dx%d", frameW, frameH);
                            result.reset();
                            break;
                        }
                        result->frameIndex = index;
                        result->pts = colorFrame->pts;
                        result->timeBase = timeBase_;
                        result->copyFromAVFrame(colorFrame, alphaFrame);
                        
                        break;
                    }
                }
            }
        } else if (packet->stream_index == alphaStreamIndex_ && alphaCodecCtx_) {
            avcodec_send_packet(alphaCodecCtx_, packet);
            avcodec_receive_frame(alphaCodecCtx_, alphaFrame);
        }
        
        av_packet_unref(packet);
    }
    
    av_packet_free(&packet);
    av_frame_free(&colorFrame);
    av_frame_free(&alphaFrame);
    
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
    av_seek_frame(formatCtx_, colorStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(colorCodecCtx_);
    if (alphaCodecCtx_) {
        avcodec_flush_buffers(alphaCodecCtx_);
    }
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* colorFrame = av_frame_alloc();
    AVFrame* alphaFrame = av_frame_alloc();
    
    int frameIndex = 0;
    int alphaFrameIndex = 0;  // Track alpha frames separately (NOT static!)
    bool hasMoreColorFrames = true;
    bool hasMoreAlphaFrames = (alphaCodecCtx_ != nullptr);
    
    while ((hasMoreColorFrames || hasMoreAlphaFrames) && !shouldStop_) {
        int ret = av_read_frame(formatCtx_, packet);
        if (ret < 0) {
            // End of file or error - flush decoders
            avcodec_send_packet(colorCodecCtx_, nullptr);
            if (alphaCodecCtx_) {
                avcodec_send_packet(alphaCodecCtx_, nullptr);
            }
            
            while (avcodec_receive_frame(colorCodecCtx_, colorFrame) >= 0 && !shouldStop_) {
                int frameW = colorFrame->width;
                int frameH = colorFrame->height;
                
                auto yuvFrame = std::make_shared<YUVAFrame>();
                if (!yuvFrame->allocate(frameW, frameH)) {
                    LOGE("Failed to allocate frame buffer for %dx%d during flush", frameW, frameH);
                    continue;
                }
                yuvFrame->frameIndex = frameIndex;
                yuvFrame->pts = colorFrame->pts;
                yuvFrame->timeBase = timeBase_;
                
                // Try to get matching alpha frame
                if (alphaCodecCtx_) {
                    avcodec_receive_frame(alphaCodecCtx_, alphaFrame);
                }
                
                // copyFromAVFrame handles both YUVA420P (with alpha) and YUV420P (fills alpha with 255)
                yuvFrame->copyFromAVFrame(colorFrame, alphaFrame);
                
                // Check before cache access - unload() may have been called
                if (shouldStop_ || !frameCache_) {
                    break;
                }
                frameCache_->put(makeFrameKey(frameIndex), yuvFrame);
                
                frameIndex++;
                stats_.framesDecoded++;
                stats_.framesCached++;
            }
            break;
        }
        
        if (packet->stream_index == colorStreamIndex_) {
            // Log packet side data for first packet only
            if (frameIndex == 0) {
                VLOGI("Packet %d: size=%d, side_data_elems=%d", frameIndex, packet->size, packet->side_data_elems);
                for (int sd = 0; sd < packet->side_data_elems; sd++) {
                    VLOGI("  Packet side_data[%d]: type=%d (BLOCKADDITIONAL=%d), size=%d", 
                         sd, packet->side_data[sd].type, 
                         AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL,
                         (int)packet->side_data[sd].size);
                    if (packet->side_data[sd].type == AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL) {
                        VLOGI("  -> Found VP9 alpha block additional data!");
                    }
                }
            }
            
            ret = avcodec_send_packet(colorCodecCtx_, packet);
            if (ret >= 0) {
                while (avcodec_receive_frame(colorCodecCtx_, colorFrame) >= 0 && !shouldStop_) {
                    // Use actual decoded frame dimensions
                    int frameW = colorFrame->width;
                    int frameH = colorFrame->height;
                    
                    // Log first frame's pixel format
                    if (frameIndex == 0) {
                        VLOGI("First decoded frame: format=%d (YUVA420P=%d), linesize=[%d,%d,%d,%d], data[3]=%p, hasAlpha=%s",
                             colorFrame->format, AV_PIX_FMT_YUVA420P,
                             colorFrame->linesize[0], colorFrame->linesize[1], 
                             colorFrame->linesize[2], colorFrame->linesize[3],
                             (void*)colorFrame->data[3],
                             (colorFrame->format == AV_PIX_FMT_YUVA420P && colorFrame->data[3]) ? "YES" : "NO");
                    }
                    
                    // Update stored dimensions if needed
                    if (frameIndex == 0) {
                        width_ = frameW;
                        height_ = frameH;
                    }
                    
                    auto yuvFrame = std::make_shared<YUVAFrame>();
                    if (!yuvFrame->allocate(frameW, frameH)) {
                        LOGE("Failed to allocate frame buffer for %dx%d at frame %d", frameW, frameH, frameIndex);
                        continue;
                    }
                    yuvFrame->frameIndex = frameIndex;
                    yuvFrame->pts = colorFrame->pts;
                    yuvFrame->timeBase = timeBase_;
                    
                    // copyFromAVFrame handles both YUVA420P (with alpha) and YUV420P (fills alpha with 255)
                    yuvFrame->copyFromAVFrame(colorFrame, nullptr);
                    
                    // Check before cache access - unload() may have been called
                    if (shouldStop_ || !frameCache_) {
                        break;
                    }
                    
                    // Store temporarily - alpha might come later
                    frameCache_->put(makeFrameKey(frameIndex), yuvFrame);
                    
                    frameIndex++;
                    stats_.framesDecoded++;
                    stats_.framesCached++;
                }
            }
        } else if (packet->stream_index == alphaStreamIndex_ && alphaCodecCtx_) {
            ret = avcodec_send_packet(alphaCodecCtx_, packet);
            if (ret >= 0) {
                while (avcodec_receive_frame(alphaCodecCtx_, alphaFrame) >= 0 && !shouldStop_) {
                    // Log first alpha frame
                    if (alphaFrameIndex == 0) {
                        VLOGI("First alpha frame decoded: %dx%d, linesize[0]=%d, format=%d, data[0]=%p",
                             alphaFrame->width, alphaFrame->height, 
                             alphaFrame->linesize[0], alphaFrame->format,
                             (void*)alphaFrame->data[0]);
                    }
                    
                    // Check before cache access - unload() may have been called
                    if (shouldStop_ || !frameCache_) {
                        break;
                    }
                    
                    // Update corresponding color frame with alpha
                    auto existingFrame = frameCache_->get(makeFrameKey(alphaFrameIndex));
                    if (existingFrame) {
                        if (alphaFrameIndex == 0) {
                            VLOGI("Copying alpha: src linesize=%d, dst stride=%d, range=%s",
                                 alphaFrame->linesize[0], existingFrame->aStride,
                                 av_color_range_name(alphaFrame->color_range));
                        }
                        existingFrame->copyAlphaFromAVFrame(alphaFrame);
                    } else if (alphaFrameIndex < 3) {
                        VLOGI("WARNING: No color frame found for alpha frame %d", alphaFrameIndex);
                    }
                    alphaFrameIndex++;
                }
            }
        }
        
        av_packet_unref(packet);
    }
    
    av_packet_free(&packet);
    av_frame_free(&colorFrame);
    av_frame_free(&alphaFrame);
    
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

int WebMAlphaPlayer::frameIndexForPts(int64_t pts) const {
    return static_cast<int>(std::llround((pts - startPts_) * timeBase_ * frameRate_));
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
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* colorFrame = av_frame_alloc();
    AVFrame* alphaFrame = av_frame_alloc();
    
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
    std::deque<AVPacket*> heldColourPackets;
    std::deque<AVFrame*> alphaAhead;
    const AVRational alphaTimeBase = alphaStreamIndex_ >= 0
        ? formatCtx_->streams[alphaStreamIndex_]->time_base : AVRational{1, 1};
    const AVRational colourTimeBase = formatCtx_->streams[colorStreamIndex_]->time_base;
    auto dropHeld = [&] {
        for (AVPacket* p : heldColourPackets) av_packet_free(&p);
        heldColourPackets.clear();
        for (AVFrame* f : alphaAhead) av_frame_free(&f);
        alphaAhead.clear();
    };
    // <0: the alpha frame is for an earlier colour frame, 0: this one, >0: a later one.
    auto compareToColour = [&](const AVFrame* alpha, int64_t colourPts) {
        int64_t pts = alpha->best_effort_timestamp != AV_NOPTS_VALUE ? alpha->best_effort_timestamp : alpha->pts;
        double gapFrames = (av_rescale_q(pts, alphaTimeBase, colourTimeBase) - colourPts) * timeBase_ * frameRate_;
        return gapFrames < -0.5 ? -1 : gapFrames > 0.5 ? 1 : 0;
    };
    auto keepAlphaFrames = [&] {
        AVFrame* f = av_frame_alloc();
        while (avcodec_receive_frame(alphaCodecCtx_, f) >= 0) {
            alphaAhead.push_back(f);
            f = av_frame_alloc();
        }
        av_frame_free(&f);
        while (alphaAhead.size() > 8) {
            av_frame_free(&alphaAhead.front());
            alphaAhead.pop_front();
        }
    };
    auto attachAlpha = [&](YUVAFrame& frame, int64_t colourPts) {
        AVPacket* p = av_packet_alloc();
        bool attached = false;
        while (!attached && !shouldStop_) {
            while (!alphaAhead.empty()) {
                AVFrame* f = alphaAhead.front();
                int order = compareToColour(f, colourPts);
                if (order > 0) break;
                alphaAhead.pop_front();
                if (order == 0) {
                    frame.copyAlphaFromAVFrame(f);
                    attached = true;
                }
                av_frame_free(&f);
                if (attached) break;
            }
            if (attached || !alphaAhead.empty()) break;
            if (av_read_frame(formatCtx_, p) < 0) break;
            if (p->stream_index == colorStreamIndex_) {
                heldColourPackets.push_back(av_packet_clone(p));
            } else if (p->stream_index == alphaStreamIndex_ && avcodec_send_packet(alphaCodecCtx_, p) >= 0) {
                keepAlphaFrames();
            }
            av_packet_unref(p);
        }
        av_packet_free(&p);
    };
    
    while (!shouldStop_ && loaded_) {
        int requested = seekRequest_.exchange(-1);
        if (requested >= 0) {
            int64_t ts = startPts_ + static_cast<int64_t>(std::llround(requested / frameRate_ / timeBase_));
            av_seek_frame(formatCtx_, colorStreamIndex_, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(colorCodecCtx_);
            if (alphaCodecCtx_) {
                avcodec_flush_buffers(alphaCodecCtx_);
            }
            dropHeld();
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
                av_seek_frame(formatCtx_, colorStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(colorCodecCtx_);
                if (alphaCodecCtx_) {
                    avcodec_flush_buffers(alphaCodecCtx_);
                }
                dropHeld();
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
        int ret = 0;
        if (!heldColourPackets.empty()) {
            AVPacket* held = heldColourPackets.front();
            heldColourPackets.pop_front();
            av_packet_move_ref(packet, held);
            av_packet_free(&held);
        } else {
            ret = av_read_frame(formatCtx_, packet);
        }
        if (ret < 0) {
            if (shouldStop_ || ioAbort_) break;
            // The input itself failed, e.g. a download broke off: looping would replay only the
            // part that arrived, so stop decoding instead.
            if (formatCtx_->pb && formatCtx_->pb->error < 0) {
                char err[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(formatCtx_->pb->error, err, sizeof(err));
                LOGE("📺 Read failed at frame %d, stopping decode: %s", frameIndex, err);
                break;
            }
            // End of file - handle loop or stop
            if (config_.loopPlayback) {
                av_seek_frame(formatCtx_, colorStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(colorCodecCtx_);
                if (alphaCodecCtx_) {
                    avcodec_flush_buffers(alphaCodecCtx_);
                }
                dropHeld();
                frameIndex = 0;
                streamDecodePos_ = 0;
                seekTarget = 0;
                VLOGI("📺 av_read_frame EOF, looping back to frame 0 (cache preserved)");
            }
            continue;
        }
        
        if (packet->stream_index == colorStreamIndex_) {
            ret = avcodec_send_packet(colorCodecCtx_, packet);
            if (ret >= 0) {
                while (avcodec_receive_frame(colorCodecCtx_, colorFrame) >= 0 && !shouldStop_) {
                    if (resyncFromPts) {
                        resyncFromPts = false;
                        int64_t pts = colorFrame->best_effort_timestamp != AV_NOPTS_VALUE
                            ? colorFrame->best_effort_timestamp : colorFrame->pts;
                        if (pts != AV_NOPTS_VALUE) frameIndex = frameIndexForPts(pts);
                    }
                    if (frameIndex < seekTarget) {
                        frameIndex++;
                        continue;
                    }
                    seekTarget = 0;
                    
                    // Check if we already have this frame cached
                    std::string key = makeFrameKey(frameIndex);
                    if (frameCache_ && !frameCache_->contains(key)) {
                        int frameW = colorFrame->width;
                        int frameH = colorFrame->height;
                        
                        auto yuvFrame = std::make_shared<YUVAFrame>();
                        if (yuvFrame->allocate(frameW, frameH)) {
                            yuvFrame->frameIndex = frameIndex;
                            yuvFrame->pts = colorFrame->pts;
                            yuvFrame->timeBase = timeBase_;
                            yuvFrame->copyFromAVFrame(colorFrame, nullptr);
                            if (alphaCodecCtx_) {
                                int64_t pts = colorFrame->best_effort_timestamp != AV_NOPTS_VALUE
                                    ? colorFrame->best_effort_timestamp : colorFrame->pts;
                                attachAlpha(*yuvFrame, pts);
                            }
                            
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
            }
        } else if (packet->stream_index == alphaStreamIndex_ && alphaCodecCtx_) {
            if (avcodec_send_packet(alphaCodecCtx_, packet) >= 0) keepAlphaFrames();
        }
        
        av_packet_unref(packet);
    }
    
    dropHeld();
    av_packet_free(&packet);
    av_frame_free(&colorFrame);
    av_frame_free(&alphaFrame);
    
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
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (eventCallback_) {
        eventCallback_(event, data);
    }
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

void WebMAlphaPlayer::unmapFile() {
    if (mappedData_) {
        if (useMmap_) {
            munmap(mappedData_, mappedSize_);
        } else {
            av_freep(&mappedData_);
        }
        mappedData_ = nullptr;
        mappedSize_ = 0;
        useMmap_ = false;
    }
}

// Static methods
bool WebMAlphaPlayer::isFFmpegInitialized() {
    return ffmpegInitialized;
}

void WebMAlphaPlayer::initializeFFmpeg() {
    std::call_once(ffmpegInitFlag, []() {
        // Modern FFmpeg (4.0+) doesn't require av_register_all()
        // Network is initialized automatically when needed
        ffmpegInitialized = true;
    });
}

std::string WebMAlphaPlayer::getFFmpegVersion() {
    return av_version_info();
}

} // namespace webm
