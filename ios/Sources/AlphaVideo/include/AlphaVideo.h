/**
 * AlphaVideo.h
 * Transparent video for iOS: VP9 WebM with alpha, decoded by the C++ core (FFmpeg + libvpx)
 * and drawn with Metal. AlphaVideoView is the view to use; the rest is the lower-level API.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Frame data structure for Metal rendering
 */
@interface AlphaVideoFrame : NSObject

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
@interface AlphaVideoStats : NSObject

@property (nonatomic, readonly) int64_t framesDecoded;
@property (nonatomic, readonly) int64_t cacheHits;
@property (nonatomic, readonly) int64_t cacheMisses;
@property (nonatomic, readonly) double avgDecodeTimeMs;
@property (nonatomic, readonly) double cacheHitRate;

@end

/**
 * Delegate for player events
 */
@protocol AlphaVideoPlayerDelegate <NSObject>

@optional
- (void)playerDidLoad:(int)totalFrames width:(int)width height:(int)height;
- (void)playerDidReceiveFrame:(AlphaVideoFrame *)frame;
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
@interface AlphaVideoConfig : NSObject

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
@interface AlphaVideoPlayer : NSObject

@property (nonatomic, weak, nullable) id<AlphaVideoPlayerDelegate> delegate;
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
- (instancetype)initWithConfig:(AlphaVideoConfig *)config;

// Configuration
- (void)setConfig:(AlphaVideoConfig *)config;
- (AlphaVideoConfig *)getConfig;
// C++ core info logging; errors are always logged
- (void)setVerbose:(BOOL)verbose;

// Loading
- (BOOL)loadFromFile:(NSString *)path;
// A AlphaVideoSourceResolver cache file that may still be downloading; reads wait for the rest.
// Use with the stream modes (preload's first seek waits for the whole file).
- (BOOL)loadFromDownload:(NSString *)path;
- (BOOL)loadFromData:(NSData *)data;
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
- (nullable AlphaVideoFrame *)getFrame:(int)index;
- (nullable AlphaVideoFrame *)getCurrentFrame;

// Statistics
- (AlphaVideoStats *)getStats;
- (void)resetStats;

// FFmpeg info
+ (NSString *)ffmpegVersion;
+ (BOOL)isFFmpegInitialized;

@end

/**
 * Metal-based view that uses the C++ player
 * High-performance GPU-accelerated YUV→RGB conversion
 */
@interface AlphaVideoMetalView : MTKView

@property (nonatomic, strong, readonly) AlphaVideoPlayer *player;
@property (nonatomic, weak, nullable) id<AlphaVideoPlayerDelegate> playerDelegate;

// Source management
@property (nonatomic, copy, nullable) NSString *sourcePath;

// Playback configuration
@property (nonatomic) BOOL autoplay;
@property (nonatomic) BOOL loop;
@property (nonatomic) double playbackSpeed;

// Log lifecycle and per-frame events; errors and warnings are always logged
@property (nonatomic) BOOL verbose;

// Initialization
- (instancetype)initWithFrame:(CGRect)frame;
- (instancetype)initWithFrame:(CGRect)frame config:(AlphaVideoConfig *)config;

// Loading
- (BOOL)loadFromPath:(NSString *)path;
- (BOOL)loadFromDownloadPath:(NSString *)path;  // See -[AlphaVideoPlayer loadFromDownload:]
- (void)unloadVideo;

// Playback control
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToFrame:(int)frame;
- (void)seekToTime:(double)seconds;

// Rendering
- (void)renderFrame:(AlphaVideoFrame *)frame;
- (void)renderCurrentFrame;

@end

@class AlphaVideoView;

/** AlphaVideoView events, delivered on the main queue. */
@protocol AlphaVideoViewDelegate <NSObject>
@optional
- (void)alphaVideoView:(AlphaVideoView *)view didLoadFrames:(int)totalFrames width:(int)width height:(int)height;
- (void)alphaVideoView:(AlphaVideoView *)view didFailWithError:(NSString *)error;
- (void)alphaVideoViewDidEnd:(AlphaVideoView *)view;
@end

/**
 * The player view: resolves a source, loads it in the chosen mode and draws it with Metal.
 *
 * `source` takes an https:// or http:// URL (downloaded into Caches, see AlphaVideoSourceResolver),
 * asset://path inside the app bundle, a file:// URL or an absolute path. A new source loads on the
 * next main-queue turn, so set the other properties in the same turn and they apply to it.
 */
@interface AlphaVideoView : UIView

@property (nonatomic, weak, nullable) id<AlphaVideoViewDelegate> delegate;

@property (nonatomic, copy, nullable) NSString *source;
// Cache file name for http(s) sources, which may include subfolders; default: a hash of the URL
@property (nonatomic, copy, nullable) NSString *cacheKey;

// "preload" (default) decodes every frame before playing; "stream" decodes a few frames ahead
// while playing; "streamAndPreload" starts like stream and keeps every frame after the first
// pass. The stream modes play http(s) sources while they download.
@property (nonatomic, copy) NSString *mode;
@property (nonatomic) int streamCacheSize;      // Frames kept while streaming. Default: 30
@property (nonatomic) int streamPrefetchAhead;  // Frames decoded ahead while streaming. Default: 15
// Preload and streamAndPreload decode one clip at a time across all views. Default: YES
@property (nonatomic) BOOL useDecodeQueue;

@property (nonatomic) BOOL autoPlay;      // Default: YES
@property (nonatomic) BOOL loop;          // Default: YES
@property (nonatomic) BOOL paused;
@property (nonatomic) double playbackSpeed;  // Default: 1.0, changeable while playing
// Log lifecycle and per-frame events; errors are always logged
@property (nonatomic) BOOL verbose;

@property (nonatomic, strong, readonly) AlphaVideoMetalView *metalView;

- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToFrame:(int)frame;

@end

/**
 * Turns a source URI into a local file the player can open.
 *
 * - https:// and http:// are downloaded into Caches, named by cacheKey (which may include
 *   subfolders) or by a hash of the URL, and reused on later calls. Views asking for the same
 *   file while it downloads share one download.
 * - asset://relative/path is a file inside the app bundle
 * - file:// URLs and absolute paths are used as they are
 */
@interface AlphaVideoSourceResolver : NSObject

/**
 * Both blocks run on the main queue.
 *
 * `ready` runs once with the file to load. For an http(s) source with `whileDownloading`, that is
 * as soon as the download starts, with `downloading` YES: load it with `loadFromDownload:`, which
 * waits for the bytes still to come. Otherwise it runs when the file is complete.
 *
 * `failed` runs if the source can't be resolved, and also after `ready` if a download breaks off.
 */
+ (void)resolveSource:(NSString *)uri
             cacheKey:(nullable NSString *)cacheKey
     whileDownloading:(BOOL)whileDownloading
                ready:(void (^)(NSString *path, BOOL downloading))ready
               failed:(void (^)(NSString *error))failed;

@end

NS_ASSUME_NONNULL_END
