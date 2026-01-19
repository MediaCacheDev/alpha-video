//
//  RNWebMAlphaViewManager.mm
//  React Native ViewManager for WebM Alpha Player (Objective-C++ implementation)
//

#import <React/RCTViewManager.h>
#import <React/RCTBridge.h>
#import <React/RCTUIManager.h>
#import "WebMAlphaPlayerBridge.h"

#pragma mark - Global Decode Queue

// Global decode queue - serializes non-stream decoding across all player instances
// This prevents CPU saturation and crashes when many players are created simultaneously
static dispatch_queue_t _globalDecodeQueue = nil;
static NSInteger _globalDecodeQueueRefCount = 0;
static dispatch_semaphore_t _globalDecodeQueueLock = nil;

static dispatch_queue_t getGlobalDecodeQueue(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        _globalDecodeQueueLock = dispatch_semaphore_create(1);
    });
    
    dispatch_semaphore_wait(_globalDecodeQueueLock, DISPATCH_TIME_FOREVER);
    if (!_globalDecodeQueue) {
        _globalDecodeQueue = dispatch_queue_create("com.webmalpha.globalDecode", DISPATCH_QUEUE_SERIAL);
        NSLog(@"🔧 Created global decode queue");
    }
    _globalDecodeQueueRefCount++;
    dispatch_semaphore_signal(_globalDecodeQueueLock);
    
    return _globalDecodeQueue;
}

static void releaseGlobalDecodeQueue(void) {
    dispatch_semaphore_wait(_globalDecodeQueueLock, DISPATCH_TIME_FOREVER);
    _globalDecodeQueueRefCount--;
    if (_globalDecodeQueueRefCount <= 0 && _globalDecodeQueue) {
        NSLog(@"🗑️ Releasing global decode queue (no more references)");
        _globalDecodeQueue = nil;
        _globalDecodeQueueRefCount = 0;
    }
    dispatch_semaphore_signal(_globalDecodeQueueLock);
}

#pragma mark - WebMAlphaWrapper

/**
 * Wrapper view that exposes React Native props and manages the Metal player view
 */
@interface WebMAlphaWrapper : UIView <WebMAlphaPlayerDelegate>

@property (nonatomic, strong) WebMAlphaMetalPlayerView *metalView;
@property (nonatomic, copy) NSString *currentSourceUri;

// Props - match WebMAlphaView.tsx interface
@property (nonatomic, strong) NSDictionary *source;
@property (nonatomic, assign) BOOL autoPlay;
@property (nonatomic, assign) BOOL loop;
@property (nonatomic, assign) BOOL paused;
@property (nonatomic, assign) double playbackSpeed;
@property (nonatomic, copy) NSString *cacheKey;

// Playback mode: "preload", "stream", or "streamAndPreload"
@property (nonatomic, copy) NSString *mode;
@property (nonatomic, assign) int streamCacheSize;
@property (nonatomic, assign) int streamPrefetchAhead;

// Legacy stream prop (deprecated, use mode instead)
@property (nonatomic, assign) BOOL stream;

// Global decode queue for preload/streamAndPreload modes
@property (nonatomic, assign) BOOL useDecodeQueue;

// Event callbacks
@property (nonatomic, copy) RCTDirectEventBlock onLoad;
@property (nonatomic, copy) RCTDirectEventBlock onError;
@property (nonatomic, copy) RCTDirectEventBlock onEnd;

@end

@implementation WebMAlphaWrapper

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self setupWithConfig:[WebMPlayerConfig defaultConfig]];
    }
    return self;
}

- (void)setupWithConfig:(WebMPlayerConfig *)config {
    self.backgroundColor = [UIColor clearColor];
    
    // Default values
    _autoPlay = YES;
    _loop = YES;
    _paused = NO;
    _playbackSpeed = 1.0;
    _mode = @"preload";
    _stream = NO;
    _streamCacheSize = 30;
    _streamPrefetchAhead = 15;
    _useDecodeQueue = YES;  // Default to using global queue for preload/streamAndPreload modes
    
    _metalView = [[WebMAlphaMetalPlayerView alloc] initWithFrame:self.bounds config:config];
    _metalView.playerDelegate = self;
    _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _metalView.translatesAutoresizingMaskIntoConstraints = YES;
    
    [self addSubview:_metalView];
    
    NSLog(@"✅ RNWebMAlphaWrapper initialized with C++ backend");
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.metalView.frame = self.bounds;
}

#pragma mark - Property Setters

- (void)setSource:(NSDictionary *)source {
    _source = source;
    NSString *uri = source[@"uri"];
    if (![uri isEqualToString:self.currentSourceUri]) {
        self.currentSourceUri = uri;
        [self loadSource];
    }
}

- (void)setAutoPlay:(BOOL)autoPlay {
    _autoPlay = autoPlay;
    self.metalView.autoplay = autoPlay;
}

- (void)setLoop:(BOOL)loop {
    _loop = loop;
    self.metalView.loop = loop;
}

- (void)setPaused:(BOOL)paused {
    _paused = paused;
    if (paused) {
        [self.metalView pause];
    } else if (self.autoPlay) {
        [self.metalView play];
    }
}

- (void)setPlaybackSpeed:(double)playbackSpeed {
    _playbackSpeed = playbackSpeed;
    self.metalView.playbackSpeed = playbackSpeed;
}

- (void)setMode:(NSString *)mode {
    if ([mode isEqualToString:@"stream"] || [mode isEqualToString:@"streamAndPreload"]) {
        _mode = mode;
    } else {
        _mode = @"preload";
    }
    // Update legacy stream flag for compatibility
    _stream = [_mode isEqualToString:@"stream"];
    [self updatePlayerConfig];
}

// Deprecated: use setMode instead
- (void)setStream:(BOOL)stream {
    _stream = stream;
    _mode = stream ? @"stream" : @"preload";
    [self updatePlayerConfig];
}

- (void)setStreamCacheSize:(int)streamCacheSize {
    _streamCacheSize = streamCacheSize > 0 ? streamCacheSize : 30;
    [self updatePlayerConfig];
}

- (void)setStreamPrefetchAhead:(int)streamPrefetchAhead {
    _streamPrefetchAhead = streamPrefetchAhead > 0 ? streamPrefetchAhead : 15;
    [self updatePlayerConfig];
}

- (void)updatePlayerConfig {
    WebMPlayerConfig *config = [self.metalView.player getConfig];
    config.mode = _mode;
    config.stream = [_mode isEqualToString:@"stream"];
    config.streamCacheSize = _streamCacheSize;
    config.streamPrefetchAhead = _streamPrefetchAhead;
    [self.metalView.player setConfig:config];
    
    NSLog(@"📺 iOS mode: %@, cacheSize=%d, prefetchAhead=%d", _mode, _streamCacheSize, _streamPrefetchAhead);
}

#pragma mark - Source Loading

- (void)loadSource {
    NSString *sourceUri = self.currentSourceUri;
    if (!sourceUri || sourceUri.length == 0) return;
    
    // Apply streaming config before loading
    [self updatePlayerConfig];
    
    NSLog(@"🎬 RNWebMAlphaWrapper loading: %@ (mode=%@, useDecodeQueue=%d)", sourceUri, _mode, _useDecodeQueue);
    
    if ([sourceUri hasPrefix:@"http://"] || [sourceUri hasPrefix:@"https://"]) {
        [self downloadAndLoadURL:sourceUri];
    } else if ([sourceUri hasPrefix:@"file://"]) {
        NSString *path = [sourceUri stringByReplacingOccurrencesOfString:@"file://" withString:@""];
        [self loadFromPathWithQueue:path];
    } else if ([sourceUri hasPrefix:@"asset://"] || [sourceUri hasPrefix:@"asset:/"]) {
        [self loadFromAsset:sourceUri];
    } else {
        [self loadFromPathWithQueue:sourceUri];
    }
}

/**
 * Load from path, optionally using global decode queue for preload/streamAndPreload modes
 */
- (void)loadFromPathWithQueue:(NSString *)path {
    // Stream mode: load directly (streaming is already lightweight)
    // Preload/StreamAndPreload + useDecodeQueue: use global queue to serialize decoding
    // Preload/StreamAndPreload + !useDecodeQueue: load directly
    BOOL shouldUseQueue = ![_mode isEqualToString:@"stream"] && _useDecodeQueue;
    
    if (shouldUseQueue) {
        NSLog(@"📋 Using global decode queue for: %@", path);
        __weak WebMAlphaWrapper *weakSelf = self;
        dispatch_queue_t queue = getGlobalDecodeQueue();
        dispatch_async(queue, ^{
            WebMAlphaWrapper *strongSelf = weakSelf;
            if (!strongSelf) {
                releaseGlobalDecodeQueue();
                return;
            }
            
            // Load on the queue (this blocks until decodeAllFrames completes)
            [strongSelf.metalView loadFromPath:path];
            
            // Release queue reference when done
            releaseGlobalDecodeQueue();
        });
    } else {
        [self.metalView loadFromPath:path];
    }
}

- (void)downloadAndLoadURL:(NSString *)urlString {
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        NSLog(@"❌ Invalid URL: %@", urlString);
        if (self.onError) {
            self.onError(@{@"error": [NSString stringWithFormat:@"Invalid URL: %@", urlString]});
        }
        return;
    }
    
    // Determine cache file path
    NSURL *cacheDir = [[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *cachedFile;
    
    if (self.cacheKey && self.cacheKey.length > 0) {
        // Use custom cache key - create subdirectories if needed
        cachedFile = [cacheDir URLByAppendingPathComponent:self.cacheKey];
        
        // Ensure parent directories exist
        NSURL *parentDir = [cachedFile URLByDeletingLastPathComponent];
        NSError *dirError = nil;
        [[NSFileManager defaultManager] createDirectoryAtURL:parentDir 
                                 withIntermediateDirectories:YES 
                                                  attributes:nil 
                                                       error:&dirError];
        if (dirError) {
            NSLog(@"⚠️ Failed to create cache directory: %@", dirError);
        }
        NSLog(@"🔑 Using custom cache key: %@", self.cacheKey);
    } else {
        // Fall back to URL hash
        NSString *hashKey = [NSString stringWithFormat:@"%lu", (unsigned long)[urlString hash]];
        cachedFile = [cacheDir URLByAppendingPathComponent:[NSString stringWithFormat:@"webm_%@.webm", hashKey]];
    }
    
    // Check cache first
    if ([[NSFileManager defaultManager] fileExistsAtPath:cachedFile.path]) {
        NSLog(@"📦 Using cached file: %@", cachedFile.path);
        [self.metalView loadFromPath:cachedFile.path];
        return;
    }
    
    // Download
    __weak WebMAlphaWrapper *weakSelf = self;
    NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        WebMAlphaWrapper *strongSelf = weakSelf;
        if (!strongSelf) return;
        
        if (error || !data) {
            NSLog(@"❌ Download failed: %@", error.localizedDescription ?: @"unknown");
            RCTDirectEventBlock errorBlock = strongSelf.onError;
            if (errorBlock) {
                NSString *errorMessage = error.localizedDescription ?: @"Download failed";
                dispatch_async(dispatch_get_main_queue(), ^{
                    errorBlock(@{@"error": errorMessage});
                });
            }
            return;
        }
        
        // Save to cache
        NSError *writeError = nil;
        [data writeToURL:cachedFile options:NSDataWritingAtomic error:&writeError];
        if (writeError) {
            NSLog(@"❌ Failed to save cached file: %@", writeError);
        } else {
            NSLog(@"💾 Saved to cache: %@", cachedFile.path);
        }
        
        NSString *filePath = cachedFile.path;
        dispatch_async(dispatch_get_main_queue(), ^{
            [strongSelf loadFromPathWithQueue:filePath];
        });
    }];
    [task resume];
}

- (void)loadFromAsset:(NSString *)assetPath {
    NSString *path = assetPath;
    path = [path stringByReplacingOccurrencesOfString:@"asset://" withString:@""];
    path = [path stringByReplacingOccurrencesOfString:@"asset:/" withString:@""];
    
    if ([path hasPrefix:@"/"]) {
        path = [path substringFromIndex:1];
    }
    
    NSString *bundlePath = [[NSBundle mainBundle] pathForResource:path ofType:nil];
    if (bundlePath) {
        [self loadFromPathWithQueue:bundlePath];
    } else {
        NSString *name = [path stringByDeletingPathExtension];
        NSString *ext = [path pathExtension];
        bundlePath = [[NSBundle mainBundle] pathForResource:name ofType:ext.length > 0 ? ext : @"webm"];
        if (bundlePath) {
            [self loadFromPathWithQueue:bundlePath];
        } else {
            NSLog(@"❌ Asset not found: %@", path);
            if (self.onError) {
                self.onError(@{@"error": [NSString stringWithFormat:@"Asset not found: %@", path]});
            }
        }
    }
}

#pragma mark - Playback Control

- (void)play {
    [self.metalView play];
}

- (void)pause {
    [self.metalView pause];
}

- (void)stop {
    [self.metalView stop];
}

- (void)seekToFrame:(NSInteger)frame {
    [self.metalView seekToFrame:(int)frame];
}

#pragma mark - WebMAlphaPlayerDelegate

- (void)playerDidLoad:(int)totalFrames width:(int)width height:(int)height {
    NSLog(@"🎬 C++ Player loaded: %d frames, %dx%d", totalFrames, width, height);
    if (self.onLoad) {
        self.onLoad(@{
            @"totalFrames": @(totalFrames),
            @"width": @(width),
            @"height": @(height)
        });
    }
}

- (void)playerDidReceiveFrame:(WebMFrame *)frame {
    // Frame rendering is handled by the Metal view
}

- (void)playerDidEncounterError:(NSString *)error {
    NSLog(@"❌ C++ Player error: %@", error);
    if (self.onError) {
        self.onError(@{@"error": error ?: @"Unknown error"});
    }
}

- (void)playerDidEnd {
    NSLog(@"🏁 C++ Player ended");
    if (self.onEnd) {
        self.onEnd(@{});
    }
}

- (void)playerPreloadComplete:(int)totalFrames timeMs:(double)timeMs {
    NSLog(@"✅ C++ Preload complete: %d frames in %.1fms", totalFrames, timeMs);
}

@end

#pragma mark - RNWebMAlphaViewManager

@interface RNWebMAlphaViewManager : RCTViewManager
@end

@implementation RNWebMAlphaViewManager

RCT_EXPORT_MODULE(RNWebMAlphaView)

+ (BOOL)requiresMainQueueSetup {
    return YES;
}

- (UIView *)view {
    return [[WebMAlphaWrapper alloc] initWithFrame:CGRectZero];
}

// Props
RCT_EXPORT_VIEW_PROPERTY(source, NSDictionary)
RCT_EXPORT_VIEW_PROPERTY(autoPlay, BOOL)
RCT_EXPORT_VIEW_PROPERTY(loop, BOOL)
RCT_EXPORT_VIEW_PROPERTY(paused, BOOL)
RCT_EXPORT_VIEW_PROPERTY(playbackSpeed, double)
RCT_EXPORT_VIEW_PROPERTY(cacheKey, NSString)

// Playback mode: "preload", "stream", or "streamAndPreload"
RCT_EXPORT_VIEW_PROPERTY(mode, NSString)

// Legacy stream prop (deprecated, use mode instead)
RCT_EXPORT_VIEW_PROPERTY(stream, BOOL)

// Streaming configuration
RCT_EXPORT_VIEW_PROPERTY(streamCacheSize, int)
RCT_EXPORT_VIEW_PROPERTY(streamPrefetchAhead, int)

// Global decode queue for preload/streamAndPreload modes
RCT_EXPORT_VIEW_PROPERTY(useDecodeQueue, BOOL)

// Events
RCT_EXPORT_VIEW_PROPERTY(onLoad, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onError, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onEnd, RCTDirectEventBlock)

// Methods exposed to JS
RCT_EXPORT_METHOD(play:(nonnull NSNumber *)reactTag) {
    [self.bridge.uiManager addUIBlock:^(RCTUIManager *uiManager, NSDictionary<NSNumber *, UIView *> *viewRegistry) {
        WebMAlphaWrapper *view = (WebMAlphaWrapper *)viewRegistry[reactTag];
        if ([view isKindOfClass:[WebMAlphaWrapper class]]) {
            [view play];
        }
    }];
}

RCT_EXPORT_METHOD(pause:(nonnull NSNumber *)reactTag) {
    [self.bridge.uiManager addUIBlock:^(RCTUIManager *uiManager, NSDictionary<NSNumber *, UIView *> *viewRegistry) {
        WebMAlphaWrapper *view = (WebMAlphaWrapper *)viewRegistry[reactTag];
        if ([view isKindOfClass:[WebMAlphaWrapper class]]) {
            [view pause];
        }
    }];
}

RCT_EXPORT_METHOD(stop:(nonnull NSNumber *)reactTag) {
    [self.bridge.uiManager addUIBlock:^(RCTUIManager *uiManager, NSDictionary<NSNumber *, UIView *> *viewRegistry) {
        WebMAlphaWrapper *view = (WebMAlphaWrapper *)viewRegistry[reactTag];
        if ([view isKindOfClass:[WebMAlphaWrapper class]]) {
            [view stop];
        }
    }];
}

RCT_EXPORT_METHOD(seekToFrame:(nonnull NSNumber *)reactTag frame:(nonnull NSNumber *)frame) {
    [self.bridge.uiManager addUIBlock:^(RCTUIManager *uiManager, NSDictionary<NSNumber *, UIView *> *viewRegistry) {
        WebMAlphaWrapper *view = (WebMAlphaWrapper *)viewRegistry[reactTag];
        if ([view isKindOfClass:[WebMAlphaWrapper class]]) {
            [view seekToFrame:[frame integerValue]];
        }
    }];
}

@end
