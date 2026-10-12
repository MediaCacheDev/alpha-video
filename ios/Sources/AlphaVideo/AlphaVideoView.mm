/**
 * AlphaVideoView.mm
 * The player view: source resolving, playback mode, the global decode queue and the Metal view.
 */

#import "AlphaVideo.h"

#define WEBM_VLOG(...) do { if (self.verbose) NSLog(__VA_ARGS__); } while (0)

#pragma mark - Global Decode Queue

// Serializes preload decoding across all views, so many views spawned at once don't saturate
// the CPU.
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
        _globalDecodeQueue = dispatch_queue_create("dev.mediacache.alphavideo.decode", DISPATCH_QUEUE_SERIAL);
    }
    _globalDecodeQueueRefCount++;
    dispatch_semaphore_signal(_globalDecodeQueueLock);
    
    return _globalDecodeQueue;
}

static void releaseGlobalDecodeQueue(void) {
    dispatch_semaphore_wait(_globalDecodeQueueLock, DISPATCH_TIME_FOREVER);
    _globalDecodeQueueRefCount--;
    if (_globalDecodeQueueRefCount <= 0 && _globalDecodeQueue) {
        _globalDecodeQueue = nil;
        _globalDecodeQueueRefCount = 0;
    }
    dispatch_semaphore_signal(_globalDecodeQueueLock);
}

#pragma mark - AlphaVideoView

@interface AlphaVideoView () <AlphaVideoPlayerDelegate>
@end

@implementation AlphaVideoView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self setup];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder {
    self = [super initWithCoder:coder];
    if (self) {
        [self setup];
    }
    return self;
}

- (void)setup {
    self.backgroundColor = [UIColor clearColor];
    
    _autoPlay = YES;
    _loop = YES;
    _paused = NO;
    _playbackSpeed = 1.0;
    _mode = @"preload";
    _streamCacheSize = 30;
    _streamPrefetchAhead = 15;
    _useDecodeQueue = YES;
    
    _metalView = [[AlphaVideoMetalView alloc] initWithFrame:self.bounds config:[AlphaVideoConfig defaultConfig]];
    _metalView.playerDelegate = self;
    _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _metalView.translatesAutoresizingMaskIntoConstraints = YES;
    
    [self addSubview:_metalView];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.metalView.frame = self.bounds;
}

#pragma mark - Properties

- (void)setSource:(NSString *)source {
    if (source == _source || [source isEqualToString:_source]) return;
    _source = [source copy];
    // Load on the next turn, once the other properties set in this turn have been applied
    __weak AlphaVideoView *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf loadSource];
    });
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

- (void)setVerbose:(BOOL)verbose {
    _verbose = verbose;
    self.metalView.verbose = verbose;
}

- (void)setMode:(NSString *)mode {
    if ([mode isEqualToString:@"stream"] || [mode isEqualToString:@"streamAndPreload"]) {
        _mode = [mode copy];
    } else {
        _mode = @"preload";
    }
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
    AlphaVideoConfig *config = [self.metalView.player getConfig];
    config.mode = _mode;
    config.stream = [_mode isEqualToString:@"stream"];
    config.streamCacheSize = _streamCacheSize;
    config.streamPrefetchAhead = _streamPrefetchAhead;
    [self.metalView.player setConfig:config];
    
    WEBM_VLOG(@"AlphaVideoView mode: %@, cacheSize=%d, prefetchAhead=%d", _mode, _streamCacheSize, _streamPrefetchAhead);
}

#pragma mark - Source Loading

- (void)loadSource {
    NSString *sourceUri = self.source;
    if (sourceUri.length == 0) return;
    
    [self updatePlayerConfig];
    
    WEBM_VLOG(@"AlphaVideoView loading: %@ (mode=%@, useDecodeQueue=%d)", sourceUri, _mode, _useDecodeQueue);
    
    // The stream modes start on the first bytes. Preload decodes the whole clip before showing
    // anything, and would hold the global decode queue for the whole download.
    BOOL whileDownloading = ![_mode isEqualToString:@"preload"];
    __weak AlphaVideoView *weakSelf = self;
    [AlphaVideoSourceResolver resolveSource:sourceUri
                                   cacheKey:self.cacheKey
                           whileDownloading:whileDownloading
                                      ready:^(NSString *path, BOOL downloading) {
        AlphaVideoView *strongSelf = weakSelf;
        if (!strongSelf || ![sourceUri isEqualToString:strongSelf.source]) return;
        [strongSelf loadFromPathWithQueue:path downloading:downloading];
    }
                                     failed:^(NSString *error) {
        AlphaVideoView *strongSelf = weakSelf;
        if (!strongSelf || ![sourceUri isEqualToString:strongSelf.source]) return;
        [strongSelf playerDidEncounterError:error];
    }];
}

/**
 * Preload and streamAndPreload go through the global decode queue when useDecodeQueue is on;
 * stream mode is light enough to load directly.
 * `downloading`: the file may still be arriving (AlphaVideoSourceResolver), so load it as a download.
 */
- (void)loadFromPathWithQueue:(NSString *)path downloading:(BOOL)downloading {
    BOOL shouldUseQueue = ![_mode isEqualToString:@"stream"] && _useDecodeQueue;
    
    if (shouldUseQueue) {
        WEBM_VLOG(@"Using global decode queue for: %@", path);
        __weak AlphaVideoView *weakSelf = self;
        dispatch_queue_t queue = getGlobalDecodeQueue();
        dispatch_async(queue, ^{
            AlphaVideoView *strongSelf = weakSelf;
            if (!strongSelf) {
                releaseGlobalDecodeQueue();
                return;
            }
            
            // Blocks until decodeAllFrames completes
            [strongSelf loadPath:path downloading:downloading];
            
            releaseGlobalDecodeQueue();
        });
    } else {
        [self loadPath:path downloading:downloading];
    }
}

- (void)loadPath:(NSString *)path downloading:(BOOL)downloading {
    if (downloading) {
        [self.metalView loadFromDownloadPath:path];
    } else {
        [self.metalView loadFromPath:path];
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

- (void)seekToFrame:(int)frame {
    [self.metalView seekToFrame:frame];
}

#pragma mark - AlphaVideoPlayerDelegate

- (void)playerDidLoad:(int)totalFrames width:(int)width height:(int)height {
    WEBM_VLOG(@"AlphaVideoView loaded: %d frames, %dx%d", totalFrames, width, height);
    id<AlphaVideoViewDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(alphaVideoView:didLoadFrames:width:height:)]) {
        [delegate alphaVideoView:self didLoadFrames:totalFrames width:width height:height];
    }
}

- (void)playerDidReceiveFrame:(AlphaVideoFrame *)frame {
    // The Metal view draws the frame
}

- (void)playerDidEncounterError:(NSString *)error {
    NSLog(@"AlphaVideoView error: %@", error);
    id<AlphaVideoViewDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(alphaVideoView:didFailWithError:)]) {
        [delegate alphaVideoView:self didFailWithError:error ?: @"Unknown error"];
    }
}

- (void)playerDidEnd {
    WEBM_VLOG(@"AlphaVideoView ended");
    id<AlphaVideoViewDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(alphaVideoViewDidEnd:)]) {
        [delegate alphaVideoViewDidEnd:self];
    }
}

- (void)playerPreloadComplete:(int)totalFrames timeMs:(double)timeMs {
    WEBM_VLOG(@"AlphaVideoView preload complete: %d frames in %.1fms", totalFrames, timeMs);
}

@end
