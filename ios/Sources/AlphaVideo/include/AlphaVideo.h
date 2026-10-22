/**
 * AlphaVideo.h
 * Transparent video for iOS: VP8, VP9 or AV1 WebM with alpha, decoded by the C++ core's codec modules
 * (libvpx, dav1d) and drawn with Metal. AlphaVideoView is the view to use; the rest is the
 * lower-level API.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * The error code when no linked codec module decodes the file: the message names the module to
 * add (AlphaVideoVPX for VP8 and VP9, AlphaVideoAV1 for AV1).
 */
extern NSString *const AlphaVideoErrorNoDecoder;

/**
 * The error code for a data source without the range-source add-on, which reads them (the
 * RangeSource SwiftPM product, the alpha-video pod's Source subspec, or for React Native
 * react-native-range-source).
 */
extern NSString *const AlphaVideoErrorNoSourceModule;

/**
 * The software decoders the player plays with. The codec modules register themselves when the
 * app links them, nothing to call: SwiftPM products AlphaVideoVPX (libvpx, VP8, VP9) and AlphaVideoAV1
 * (dav1d, AV1), or pods alpha-video/VPX and alpha-video/AV1.
 */
@interface AlphaVideoCodecs : NSObject

/**
 * Registers another software decoder: `pluginList` is a native AlphaVideoDecoderPluginList
 * (cpp/AlphaVideoDecoderPlugin.h). Registered decoders come after the modules', which a stream
 * tries first. NO, with `error`, for a decoder built for another plugin ABI.
 */
+ (BOOL)registerPluginList:(const void *)pluginList error:(NSError **)error;

/** The registered software decoders, e.g. @[@"libvpx v1.15.2", @"dav1d 1.5.1"]; empty with none. */
@property (class, nonatomic, readonly) NSArray<NSString *> *softwareDecoders;

@end

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
// `code`: AlphaVideoErrorNoDecoder, AlphaVideoErrorNoSourceModule, or nil
- (void)playerDidEncounterError:(NSString *)error code:(nullable NSString *)code;
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
// - preload: decode all frames upfront, then play
// - stream (default): decode on-demand with sliding window cache
// - streamAndPreload: start playing immediately, cache all frames for smooth looping
@property (nonatomic, copy) NSString *mode;   // Default: "stream"

// Legacy stream property (deprecated, use mode instead)
@property (nonatomic) BOOL stream;            // Default: YES
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
// The decoder for the loaded clip: "libvpx", or "dav1d" (AV1)
@property (nonatomic, readonly) NSString *decoderName;

// Initialization
- (instancetype)init;
- (instancetype)initWithConfig:(AlphaVideoConfig *)config;

// Configuration
- (void)setConfig:(AlphaVideoConfig *)config;
- (AlphaVideoConfig *)getConfig;
// C++ core info logging; errors are always logged
- (void)setVerbose:(BOOL)verbose;

// Loading. AlphaVideoView also plays URLs and data sources, read by range as they play.
- (BOOL)loadFromFile:(NSString *)path;
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

// The decoder libraries, e.g. "libvpx v1.14.0, dav1d 1.5.1"
+ (NSString *)libraryVersions;

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
@property (nonatomic) BOOL autoPlay;
@property (nonatomic) BOOL loop;
@property (nonatomic) double playbackSpeed;

// Log lifecycle and per-frame events; errors and warnings are always logged
@property (nonatomic) BOOL verbose;

// Initialization
- (instancetype)initWithFrame:(CGRect)frame;
- (instancetype)initWithFrame:(CGRect)frame config:(AlphaVideoConfig *)config;

// Loading
- (BOOL)loadFromPath:(NSString *)path;
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
// `code`: AlphaVideoErrorNoDecoder, or nil
- (void)alphaVideoView:(AlphaVideoView *)view didFailWithError:(NSString *)error code:(nullable NSString *)code;
- (void)alphaVideoViewDidEnd:(AlphaVideoView *)view;
@end

/**
 * The player view: resolves a source, loads it in the chosen mode and draws it with Metal.
 *
 * `source` takes an https:// or http:// URL (read by range requests as it plays, or without the
 * range-source add-on downloaded whole),
 * asset://path inside the app bundle, a file:// URL or an absolute path, or with `dataSource` any
 * URL that data source reads. A new source loads on the next main-queue turn, so set the other
 * properties in the same turn and they apply to it.
 */
@interface AlphaVideoView : UIView

@property (nonatomic, weak, nullable) id<AlphaVideoViewDelegate> delegate;

@property (nonatomic, copy, nullable) NSString *source;
// Sent with every request for an http(s) source, and given to a data source
@property (nonatomic, copy, nullable) NSDictionary<NSString *, NSString *> *headers;
// Reads the source through the data source registered under this name with range-source's
// RangeDataSources (the range-source add-on)
@property (nonatomic, copy, nullable) NSString *dataSource;
// What the store keeps the source under, instead of its URL (for presigned URLs that change)
@property (nonatomic, copy, nullable) NSString *cacheKey;
// Where a URL's or data source's bytes are kept between plays: "disk" (Caches/alpha-video),
// "memory" (for the app's lifetime) or "none". "auto" (default) is disk for URLs and none for a
// data source. The store is partial: it keeps what has been played, and a clip held whole plays
// again offline.
@property (nonatomic, copy) NSString *storage;
// The store's cap over every clip in it, least recently played out first. Default: 512 MB
@property (nonatomic) int64_t maxStorageBytes;

// "stream" (default) decodes a few frames ahead while playing; "preload" decodes every frame
// before playing; "streamAndPreload" starts like stream and keeps every frame after the first
// pass. The stream modes play URLs as they arrive; preload fetches the whole file first.
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
// The engine and decoder playing the loaded clip, as Android's onLoad names it: "software (dav1d)"
@property (nonatomic, readonly) NSString *loadedDecoder;

- (void)play;
- (void)pause;
- (void)stop;
// Both clamp to the clip and keep playing or stay paused; a paused view shows the frame.
- (void)seekToFrame:(int)frame;
- (void)seekToTime:(double)seconds;

@end

NS_ASSUME_NONNULL_END
