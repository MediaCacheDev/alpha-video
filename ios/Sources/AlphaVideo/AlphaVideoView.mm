/**
 * AlphaVideoView.mm
 * The player view: source resolving, playback mode, the global decode queue and the Metal view.
 */

#import "AlphaVideoInternal.h"

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

static const int64_t AlphaVideoDefaultMaxStorageBytes = 512LL * 1024 * 1024;

static void AlphaVideoWarnWithoutAddOn(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSLog(@"[AlphaVideo] range-source isn't installed (the RangeSource SwiftPM product, the alpha-video pod's "
              @"Source subspec, or for React Native npm i react-native-range-source): URLs are downloaded whole for "
              @"each play, without range requests, storage, maxStorageBytes or cacheKey");
    });
}

@interface AlphaVideoView () <AlphaVideoPlayerDelegate>
@end

@implementation AlphaVideoView {
    // Bumped on every source, so a slow open can't load over a newer source. Main queue only.
    NSUInteger _sourceGeneration;
    // The reader of the current source, for URLs and data sources. Main queue only.
    AlphaVideoSourceReader *_reader;
    // Loads that read over the network, in order and off the main queue
    dispatch_queue_t _loadQueue;
}

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
    _mode = @"stream";
    _streamPrefetchAhead = 8;
    _useDecodeQueue = YES;
    _storage = @"auto";
    _maxStorageBytes = AlphaVideoDefaultMaxStorageBytes;
    _loadQueue = dispatch_queue_create("dev.mediacache.alphavideo.load", DISPATCH_QUEUE_SERIAL);
    
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

- (NSString *)loadedDecoder {
    return [NSString stringWithFormat:@"software (%@)", self.metalView.player.decoderName];
}

- (void)setSource:(NSString *)source {
    if (source == _source || [source isEqualToString:_source]) return;
    _source = [source copy];
    [self scheduleLoad];
}

// What a source is read with: a change reloads it
- (void)setHeaders:(NSDictionary<NSString *, NSString *> *)headers {
    if (headers == _headers || [headers isEqualToDictionary:_headers]) return;
    _headers = [headers copy];
    if (_source) [self scheduleLoad];
}

- (void)setDataSource:(NSString *)dataSource {
    if (dataSource == _dataSource || [dataSource isEqualToString:_dataSource]) return;
    _dataSource = [dataSource copy];
    if (_source) [self scheduleLoad];
}

/** Loads on the next turn, once the other properties set in this turn have been applied. */
- (void)scheduleLoad {
    NSUInteger generation = ++_sourceGeneration;
    __weak AlphaVideoView *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        AlphaVideoView *strongSelf = weakSelf;
        if (strongSelf && generation == strongSelf->_sourceGeneration) [strongSelf loadSource];
    });
}

- (void)setStorage:(NSString *)storage {
    if ([storage isEqualToString:@"disk"] || [storage isEqualToString:@"memory"] || [storage isEqualToString:@"none"]) {
        _storage = [storage copy];
    } else {
        _storage = @"auto";
    }
}

- (void)setMaxStorageBytes:(int64_t)maxStorageBytes {
    _maxStorageBytes = maxStorageBytes > 0 ? maxStorageBytes : AlphaVideoDefaultMaxStorageBytes;
}

- (void)setAutoPlay:(BOOL)autoPlay {
    _autoPlay = autoPlay;
    self.metalView.autoPlay = autoPlay && !_paused;
}

- (void)setLoop:(BOOL)loop {
    _loop = loop;
    self.metalView.loop = loop;
}

- (void)setPaused:(BOOL)paused {
    _paused = paused;
    // A view paused before it loads must not start on load
    self.metalView.autoPlay = _autoPlay && !paused;
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
    if ([mode isEqualToString:@"preload"] || [mode isEqualToString:@"streamAndPreload"]) {
        _mode = [mode copy];
    } else {
        _mode = @"stream";
    }
    [self updatePlayerConfig];
}

- (void)setStreamPrefetchAhead:(int)streamPrefetchAhead {
    _streamPrefetchAhead = streamPrefetchAhead > 0 ? streamPrefetchAhead : 8;
    [self updatePlayerConfig];
}

- (void)updatePlayerConfig {
    AlphaVideoConfig *config = [self.metalView.player getConfig];
    config.mode = _mode;
    config.stream = [_mode isEqualToString:@"stream"];
    config.streamPrefetchAhead = _streamPrefetchAhead;
    [self.metalView.player setConfig:config];
    
    WEBM_VLOG(@"AlphaVideoView mode: %@, prefetchAhead=%d", _mode, _streamPrefetchAhead);
}

#pragma mark - Source Loading

- (void)loadSource {
    NSString *sourceUri = self.source;
    if (sourceUri.length == 0) return;
    
    [self updatePlayerConfig];
    _reader = nil;
    
    WEBM_VLOG(@"AlphaVideoView loading: %@ (mode=%@, useDecodeQueue=%d)", sourceUri, _mode, _useDecodeQueue);
    
    BOOL appSource = self.dataSource.length > 0;
    BOOL remote = [sourceUri hasPrefix:@"https://"] || [sourceUri hasPrefix:@"http://"];
    if (!appSource && !remote) {
        NSString *path = AlphaVideoLocalPathForSource(sourceUri);
        if (!path) {
            [self playerDidEncounterError:[NSString stringWithFormat:
                @"Unsupported source '%@'. Use https://, file://, an absolute path or asset://", sourceUri] code:nil];
        } else if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
            [self playerDidEncounterError:[NSString stringWithFormat:@"File not found: %@", sourceUri] code:nil];
        } else {
            [self loadWithQueue:^(AlphaVideoView *view) { [view.metalView loadFromPath:path]; } offMain:NO];
        }
        return;
    }
    
    if (!AlphaVideoRangeSourcePlugin()) {
        if (appSource) {
            [self playerDidEncounterError:[NSString stringWithFormat:
                @"The data source \"%@\" needs the range-source add-on (the RangeSource SwiftPM product, the "
                @"alpha-video pod's Source subspec, or for React Native react-native-range-source), which the "
                @"app doesn't include", self.dataSource]
                                     code:AlphaVideoErrorNoSourceModule];
            return;
        }
        AlphaVideoWarnWithoutAddOn();
    }

    NSString *dataSource = appSource ? [self.dataSource copy] : nil;
    NSURL *url = [NSURL URLWithString:sourceUri];
    if (!url) {
        [self playerDidEncounterError:[NSString stringWithFormat:@"Invalid URL: %@", sourceUri] code:nil];
        return;
    }
    NSString *storage = [_storage isEqualToString:@"auto"] ? (appSource ? @"none" : @"disk") : _storage;
    NSString *key = self.cacheKey.length > 0 ? self.cacheKey : sourceUri;
    NSDictionary<NSString *, NSString *> *headers = self.headers ?: @{};
    int64_t maxStorageBytes = _maxStorageBytes;
    // The stream modes start on the first bytes. Preload decodes the whole clip before showing
    // anything, and would hold the global decode queue for the whole transfer.
    BOOL fetchFirst = [_mode isEqualToString:@"preload"];
    NSUInteger generation = _sourceGeneration;
    __weak AlphaVideoView *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *error = nil;
        AlphaVideoSourceReader *reader = [AlphaVideoSourceReader openURL:url
                                                              dataSource:dataSource
                                                                 headers:headers
                                                                 storage:storage
                                                                     key:key
                                                         maxStorageBytes:maxStorageBytes
                                                                   error:&error];
        if (reader && fetchFirst && ![reader fetchAll:&error]) reader = nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            AlphaVideoView *strongSelf = weakSelf;
            if (!strongSelf || generation != strongSelf->_sourceGeneration) return;
            if (!reader) {
                [strongSelf playerDidEncounterError:[NSString stringWithFormat:@"Loading %@ failed: %@", sourceUri, error] code:nil];
                return;
            }
            strongSelf->_reader = reader;
            [strongSelf loadWithQueue:^(AlphaVideoView *view) { [view.metalView loadFromReader:reader]; } offMain:YES];
        });
    });
}

/**
 * Preload and streamAndPreload go through the global decode queue when useDecodeQueue is on;
 * stream mode is light enough to load directly, on the main queue for a local file and on this
 * view's load queue (`offMain`) for one that reads its header over the network.
 */
- (void)loadWithQueue:(void (^)(AlphaVideoView *view))load offMain:(BOOL)offMain {
    BOOL shouldUseQueue = ![_mode isEqualToString:@"stream"] && _useDecodeQueue;
    __weak AlphaVideoView *weakSelf = self;
    
    if (shouldUseQueue) {
        WEBM_VLOG(@"Using global decode queue for: %@", self.source);
        dispatch_queue_t queue = getGlobalDecodeQueue();
        dispatch_async(queue, ^{
            AlphaVideoView *strongSelf = weakSelf;
            // Blocks until decodeAllFrames completes
            if (strongSelf) load(strongSelf);
            releaseGlobalDecodeQueue();
        });
    } else if (offMain) {
        dispatch_async(_loadQueue, ^{
            AlphaVideoView *strongSelf = weakSelf;
            if (strongSelf) load(strongSelf);
        });
    } else {
        load(self);
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

- (void)seekToTime:(double)seconds {
    [self.metalView seekToTime:seconds];
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

- (void)playerDidEncounterError:(NSString *)error code:(nullable NSString *)code {
    NSLog(@"AlphaVideoView error: %@", error);
    id<AlphaVideoViewDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(alphaVideoView:didFailWithError:code:)]) {
        [delegate alphaVideoView:self didFailWithError:error ?: @"Unknown error" code:code];
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
