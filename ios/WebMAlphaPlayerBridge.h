/**
 * WebMAlphaPlayerBridge.h
 * Objective-C++ bridge for WebMAlphaPlayer C++ core
 * Integrates with Metal for GPU rendering
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Frame data structure for Metal rendering
 */
@interface WebMFrame : NSObject

@property (nonatomic, readonly) NSData *yPlane;
@property (nonatomic, readonly) NSData *uPlane;
@property (nonatomic, readonly) NSData *vPlane;
@property (nonatomic, readonly) NSData *aPlane;

@property (nonatomic, readonly) int width;
@property (nonatomic, readonly) int height;
@property (nonatomic, readonly) int frameIndex;
@property (nonatomic, readonly) double timestamp;

// Raw pointers for direct Metal texture upload (faster than NSData copy)
@property (nonatomic, readonly) const uint8_t *yPlanePtr;
@property (nonatomic, readonly) const uint8_t *uPlanePtr;
@property (nonatomic, readonly) const uint8_t *vPlanePtr;
@property (nonatomic, readonly) const uint8_t *aPlanePtr;

@property (nonatomic, readonly) int yStride;
@property (nonatomic, readonly) int uvStride;
@property (nonatomic, readonly) int aStride;

@end

/**
 * Player statistics
 */
@interface WebMPlayerStats : NSObject

@property (nonatomic, readonly) int64_t framesDecoded;
@property (nonatomic, readonly) int64_t cacheHits;
@property (nonatomic, readonly) int64_t cacheMisses;
@property (nonatomic, readonly) double avgDecodeTimeMs;
@property (nonatomic, readonly) double cacheHitRate;

@end

/**
 * Delegate for player events
 */
@protocol WebMAlphaPlayerDelegate <NSObject>

@optional
- (void)playerDidLoad:(int)totalFrames width:(int)width height:(int)height;
- (void)playerDidReceiveFrame:(WebMFrame *)frame;
- (void)playerDidStartPlaying;
- (void)playerDidPause;
- (void)playerDidStop;
- (void)playerDidLoop;
- (void)playerDidEnd;
- (void)playerDidEncounterError:(NSString *)error;
- (void)playerPreloadProgress:(int)loaded total:(int)total;
- (void)playerPreloadComplete:(int)totalFrames timeMs:(double)timeMs;

@end

/**
 * Configuration for the player
 */
@interface WebMPlayerConfig : NSObject

@property (nonatomic) int decodeThreads;      // Default: 4
@property (nonatomic) int cacheSize;          // Default: 250
@property (nonatomic) BOOL preloadAllFrames;  // Default: YES
@property (nonatomic) BOOL loopPlayback;      // Default: YES
@property (nonatomic) double playbackSpeed;   // Default: 1.0

// Playback mode: "preload", "stream", or "streamAndPreload"
// - preload (default): decode all frames upfront, then play
// - stream: decode on-demand with sliding window cache
// - streamAndPreload: start playing immediately, cache all frames for smooth looping
@property (nonatomic, copy) NSString *mode;   // Default: "preload"

// Legacy stream property (deprecated, use mode instead)
@property (nonatomic) BOOL stream;            // Default: NO
@property (nonatomic) int streamCacheSize;    // Default: 30 (frames to keep in cache when streaming)
@property (nonatomic) int streamPrefetchAhead; // Default: 15 (frames to decode ahead)

+ (instancetype)defaultConfig;

@end

/**
 * Main Objective-C++ wrapper for WebMAlphaPlayer C++ core
 */
@interface WebMAlphaPlayerWrapper : NSObject

@property (nonatomic, weak, nullable) id<WebMAlphaPlayerDelegate> delegate;
@property (nonatomic, readonly) BOOL isLoaded;
@property (nonatomic, readonly) BOOL isPlaying;
@property (nonatomic, readonly) int currentFrame;
@property (nonatomic, readonly) double currentTime;
@property (nonatomic, readonly) double duration;
@property (nonatomic, readonly) int totalFrames;
@property (nonatomic, readonly) int width;
@property (nonatomic, readonly) int height;
@property (nonatomic, readonly) double frameRate;

// Initialization
- (instancetype)init;
- (instancetype)initWithConfig:(WebMPlayerConfig *)config;

// Configuration
- (void)setConfig:(WebMPlayerConfig *)config;
- (WebMPlayerConfig *)getConfig;

// Loading
- (BOOL)loadFromFile:(NSString *)path;
- (BOOL)loadFromData:(NSData *)data;
- (BOOL)loadFromURL:(NSURL *)url;
- (void)unload;

// Playback
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToTime:(double)seconds;
- (void)seekToFrame:(int)frameIndex;

/**
 * Changes playback rate live, without moving the playhead.
 *
 * Distinct from setting `playbackSpeed` in the config, which only takes effect at load: this
 * reaches the running playback loop, so a rate swapped on hover applies to the next frame.
 */
- (void)setPlaybackSpeed:(double)speed;
- (double)playbackSpeed;

// Direct frame access (for manual rendering)
- (nullable WebMFrame *)getFrame:(int)index;
- (nullable WebMFrame *)getCurrentFrame;

// Statistics
- (WebMPlayerStats *)getStats;
- (void)resetStats;

// FFmpeg info
+ (NSString *)ffmpegVersion;
+ (BOOL)isFFmpegInitialized;

@end

/**
 * Metal-based view that uses the C++ player
 * High-performance GPU-accelerated YUV→RGB conversion
 */
@interface WebMAlphaMetalPlayerView : MTKView

@property (nonatomic, strong, readonly) WebMAlphaPlayerWrapper *player;
@property (nonatomic, weak, nullable) id<WebMAlphaPlayerDelegate> playerDelegate;

// Source management
@property (nonatomic, copy, nullable) NSString *sourcePath;
@property (nonatomic, copy, nullable) NSURL *sourceURL;

// Playback configuration
@property (nonatomic) BOOL autoplay;
@property (nonatomic) BOOL loop;
@property (nonatomic) double playbackSpeed;

// Initialization
- (instancetype)initWithFrame:(CGRect)frame;
- (instancetype)initWithFrame:(CGRect)frame config:(WebMPlayerConfig *)config;

// Loading
- (BOOL)loadFromPath:(NSString *)path;
- (BOOL)loadFromURL:(NSURL *)url;
- (void)unloadVideo;

// Playback control
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToFrame:(int)frame;
- (void)seekToTime:(double)seconds;

// Rendering
- (void)renderFrame:(WebMFrame *)frame;
- (void)renderCurrentFrame;

@end

NS_ASSUME_NONNULL_END
