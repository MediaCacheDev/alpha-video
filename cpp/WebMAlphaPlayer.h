/**
 * WebMAlphaPlayer.h
 * High-performance VP9+Alpha WebM player using FFmpeg C API
 * Fully self-contained C++ implementation for iOS and Android
 * 
 * This is a complete native implementation - no delegation to Java/Swift layers.
 * FFmpeg is linked directly as a static library.
 */

#ifndef WEBM_ALPHA_PLAYER_H
#define WEBM_ALPHA_PLAYER_H

#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <unordered_map>
#include <list>
#include <chrono>

// Forward declarations for FFmpeg types (avoid including heavy headers here)
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVIOContext;
struct SwsContext;

namespace webm {

/**
 * Represents a decoded YUVA frame with zero-copy plane access
 */
struct YUVAFrame {
    // Plane data pointers (owned by this struct)
    uint8_t* yPlane;
    uint8_t* uPlane;
    uint8_t* vPlane;
    uint8_t* aPlane;
    
    // Plane sizes
    int ySize;
    int uSize;
    int vSize;
    int aSize;
    
    // Frame dimensions
    int width;
    int height;
    
    // Timing
    int64_t pts;          // Presentation timestamp
    double timeBase;      // Time base for pts conversion
    int frameIndex;       // Frame number in sequence
    
    // Strides (bytes per row - may differ from width due to alignment)
    int yStride;
    int uvStride;
    int aStride;
    
    YUVAFrame();
    ~YUVAFrame();
    
    // Move semantics for efficient frame passing
    YUVAFrame(YUVAFrame&& other) noexcept;
    YUVAFrame& operator=(YUVAFrame&& other) noexcept;
    
    // Delete copy to enforce zero-copy semantics
    YUVAFrame(const YUVAFrame&) = delete;
    YUVAFrame& operator=(const YUVAFrame&) = delete;
    
    // Allocate plane memory
    bool allocate(int w, int h);
    
    // Copy from AVFrame (used during decode)
    void copyFromAVFrame(AVFrame* colorFrame, AVFrame* alphaFrame);
    
    // Get duration in milliseconds
    double getDurationMs() const;
};

/**
 * Callback type for frame delivery
 * Called on decoder thread - implementations should be fast (just queue for GPU upload)
 */
using FrameCallback = std::function<void(std::shared_ptr<YUVAFrame>)>;

/**
 * Callback for playback events
 */
using EventCallback = std::function<void(const std::string& event, const std::string& data)>;

/**
 * LRU Cache for decoded frames
 */
class FrameCache {
public:
    explicit FrameCache(size_t maxFrames = 30);
    
    void put(const std::string& key, std::shared_ptr<YUVAFrame> frame);
    std::shared_ptr<YUVAFrame> get(const std::string& key);
    void clear();
    size_t size() const;
    size_t maxSize() const;
    bool contains(const std::string& key) const;
    bool isFull() const;
    void evict(const std::string& key);  // Remove specific frame from cache
    void setMaxSize(size_t maxFrames);   // Dynamically adjust cache size
    // Evict the frame furthest behind playbackPos in circular buffer (modulo totalFrames)
    // Returns true if a frame was evicted
    bool evictFurthestBehind(int playbackPos, int totalFrames, const std::string& keyPrefix);
    
private:
    size_t maxSize_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<YUVAFrame>> cache_;
    std::list<std::string> lruList_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lruMap_;
    
    void evictOldest();
};

/**
 * Decoder statistics snapshot (for returning stats to callers)
 */
struct DecoderStatsSnapshot {
    int64_t framesDecoded{0};
    int64_t framesCached{0};
    int64_t cacheHits{0};
    int64_t cacheMisses{0};
    int64_t decodeTimeUs{0};    // Total decode time in microseconds
    int64_t peakMemoryBytes{0};
    
    double getAverageDecodeTimeMs() const {
        return framesDecoded > 0 ? (decodeTimeUs / 1000.0) / framesDecoded : 0.0;
    }
    double getCacheHitRate() const {
        int64_t total = cacheHits + cacheMisses;
        return total > 0 ? (double)cacheHits / total : 0.0;
    }
};

/**
 * Decoder statistics for performance monitoring (thread-safe internal use)
 */
struct DecoderStats {
    std::atomic<int64_t> framesDecoded{0};
    std::atomic<int64_t> framesCached{0};
    std::atomic<int64_t> cacheHits{0};
    std::atomic<int64_t> cacheMisses{0};
    std::atomic<int64_t> decodeTimeUs{0};    // Total decode time in microseconds
    std::atomic<int64_t> peakMemoryBytes{0};
    
    double getAverageDecodeTimeMs() const;
    double getCacheHitRate() const;
    void reset();
    
    // Create a copyable snapshot
    DecoderStatsSnapshot snapshot() const {
        DecoderStatsSnapshot s;
        s.framesDecoded = framesDecoded.load();
        s.framesCached = framesCached.load();
        s.cacheHits = cacheHits.load();
        s.cacheMisses = cacheMisses.load();
        s.decodeTimeUs = decodeTimeUs.load();
        s.peakMemoryBytes = peakMemoryBytes.load();
        return s;
    }
};

/**
 * Playback mode for the player
 */
enum class PlaybackMode {
    Preload,          // Decode all frames upfront before playback starts
    Stream,           // Decode on-demand with sliding window cache (frames evicted)
    StreamAndPreload  // Start playing immediately, cache all frames for smooth looping
};

/**
 * Configuration for the player
 */
struct PlayerConfig {
    int decodeThreads = 4;          // FFmpeg decoder threads
    bool useHardwareAccel = false;  // Try hardware decode (rarely works for VP9+alpha)
    int cacheSize = 250;            // Max frames in cache (should be >= totalFrames when preloading)
    bool preloadAllFrames = true;   // Decode all frames upfront (ignored if mode != Preload)
    bool loopPlayback = true;       // Loop at end
    double playbackSpeed = 1.0;     // Playback speed multiplier
    
    // Playback mode:
    // - Preload (default): decode all frames upfront, then play
    // - Stream: decode on-demand with sliding window cache (frames evicted on loop)
    // - StreamAndPreload: start playing immediately, cache all frames for smooth looping
    PlaybackMode mode = PlaybackMode::Preload;
    
    // Legacy: kept for backwards compatibility, use mode instead
    bool stream = false;
    
    int streamCacheSize = 30;       // Frames to keep in cache when streaming (default: ~1 second at 30fps)
    int streamPrefetchAhead = 15;   // Frames to decode ahead of playback position
};

/**
 * Main WebM Alpha Player class
 * Thread-safe, supports concurrent access
 */
class WebMAlphaPlayer {
public:
    WebMAlphaPlayer();
    ~WebMAlphaPlayer();
    
    // Configuration
    void setConfig(const PlayerConfig& config);
    PlayerConfig getConfig() const;
    
    // Loading
    bool loadFromFile(const std::string& path);
    bool loadFromMemory(const uint8_t* data, size_t size);
    bool loadFromUrl(const std::string& url);  // Network loading
    void unload();
    
    // Playback control
    void play();
    void pause();
    void stop();
    void seekTo(double timeSeconds);
    void seekToFrame(int frameIndex);
    
    // State queries
    bool isLoaded() const;
    bool isPlaying() const;
    int getCurrentFrame() const;
    double getCurrentTime() const;
    double getDuration() const;
    int getTotalFrames() const;
    int getWidth() const;
    int getHeight() const;
    double getFrameRate() const;
    
    // Callbacks
    void setFrameCallback(FrameCallback callback);
    void setEventCallback(EventCallback callback);
    
    // Direct frame access (for single-frame rendering)
    std::shared_ptr<YUVAFrame> getFrame(int index);
    std::shared_ptr<YUVAFrame> getCachedFrame(int index);  // Cache-only, thread-safe
    std::shared_ptr<YUVAFrame> getCurrentFrameData();
    
    // Statistics
    DecoderStatsSnapshot getStats() const;
    void resetStats();
    
    // Static utilities
    static bool isFFmpegInitialized();
    static void initializeFFmpeg();
    static std::string getFFmpegVersion();
    
private:
    // FFmpeg contexts
    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* colorCodecCtx_ = nullptr;
    AVCodecContext* alphaCodecCtx_ = nullptr;
    
    int colorStreamIndex_ = -1;
    int alphaStreamIndex_ = -1;
    
    // Video info
    int width_ = 0;
    int height_ = 0;
    int totalFrames_ = 0;
    double duration_ = 0.0;
    double frameRate_ = 30.0;
    double timeBase_ = 0.0;
    
    // State
    std::atomic<bool> loaded_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int> currentFrame_{0};
    
    // Threading
    std::unique_ptr<std::thread> decodeThread_;
    std::unique_ptr<std::thread> playbackThread_;
    std::unique_ptr<std::thread> prefetchThread_;  // Background prefetch for streaming mode
    std::timed_mutex decodeMutex_;  // timed_mutex allows try_lock_for() for non-blocking unload
    std::mutex unloadMutex_;  // Protects unload() from concurrent calls
    std::condition_variable decodeCV_;
    std::condition_variable_any prefetchCV_;  // Signal prefetch thread
    std::atomic<bool> shouldStop_{false};
    std::atomic<int> prefetchTarget_{-1};  // Target frame for prefetch thread
    std::atomic<int> streamDecodePos_{0};  // Current sequential decode position for streaming
    std::atomic<bool> firstPassComplete_{false};  // For streamAndPreload: all frames cached after first pass
    
    // Caching
    std::unique_ptr<FrameCache> frameCache_;
    
    // Callbacks
    FrameCallback frameCallback_;
    EventCallback eventCallback_;
    std::mutex callbackMutex_;
    
    // Config
    PlayerConfig config_;
    mutable std::mutex configMutex_;
    
    // Stats
    DecoderStats stats_;
    
    // Private methods
    bool openStreams();
    bool initCodecs();
    void closeCodecs();
    
    std::shared_ptr<YUVAFrame> decodeFrame(int index);
    void decodeAllFrames();
    void playbackLoop();
    void streamingPrefetch(int currentFrame);  // Prefetch frames ahead in streaming mode
    void evictOldFrames(int currentFrame);     // Remove frames behind playback position
    void prefetchLoop();                        // Background prefetch thread loop
    void requestPrefetch(int currentFrame);     // Request prefetch from playback thread (non-blocking)
    void streamingDecodeLoop();                 // Sequential decode loop for streaming mode
    std::shared_ptr<YUVAFrame> decodeNextFrame(); // Decode next frame sequentially (no seeking)
    
    void emitEvent(const std::string& event, const std::string& data = "");
    void emitFrame(std::shared_ptr<YUVAFrame> frame);
    
    std::string makeFrameKey(int index) const;
    
    // Memory-mapped file support
    uint8_t* mappedData_ = nullptr;
    size_t mappedSize_ = 0;
    bool useMmap_ = false;
    
    // Custom I/O support for memory buffers
    struct AVIOContext* avioCtx_ = nullptr;
    void* bufferCtx_ = nullptr;    // Opaque context for memory buffer
    uint8_t* avioBuffer_ = nullptr; // I/O buffer (must be freed separately)
    
    // Swscale context for YUVA420P conversion
    SwsContext* swsCtx_ = nullptr;
    AVFrame* swsFrame_ = nullptr;  // Intermediate frame for sws_scale output
    
    void unmapFile();
    void cleanupIO();
};

} // namespace webm

#endif // WEBM_ALPHA_PLAYER_H
