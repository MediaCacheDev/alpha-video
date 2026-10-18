/**
 * AlphaVideoPlayer.mm
 * The Objective-C++ layer over the C++ WebMAlphaPlayer core: the player and the Metal view.
 */

#import "AlphaVideoInternal.h"
#include "RangeReader.h"
#include "VideoDecoder.h"
#include "WebMAlphaPlayer.h"
#include <memory>

// Lifecycle and per-frame logging inside AlphaVideoMetalView, only with its verbose flag
#define WEBM_VLOG(...) do { if (self.verbose) NSLog(__VA_ARGS__); } while (0)

//==============================================================================
// AlphaVideoFrame Implementation
//==============================================================================

@implementation AlphaVideoFrame {
    std::shared_ptr<webm::YUVAFrame> _cppFrame;
}

- (instancetype)initWithCppFrame:(std::shared_ptr<webm::YUVAFrame>)frame {
    self = [super init];
    if (self && frame) {
        _cppFrame = frame;
    }
    return self;
}

- (NSData *)yPlane {
    if (!_cppFrame) return nil;
    return [NSData dataWithBytesNoCopy:_cppFrame->yPlane 
                                length:_cppFrame->ySize 
                          freeWhenDone:NO];
}

- (NSData *)uPlane {
    if (!_cppFrame) return nil;
    return [NSData dataWithBytesNoCopy:_cppFrame->uPlane 
                                length:_cppFrame->uSize 
                          freeWhenDone:NO];
}

- (NSData *)vPlane {
    if (!_cppFrame) return nil;
    return [NSData dataWithBytesNoCopy:_cppFrame->vPlane 
                                length:_cppFrame->vSize 
                          freeWhenDone:NO];
}

- (NSData *)aPlane {
    if (!_cppFrame) return nil;
    return [NSData dataWithBytesNoCopy:_cppFrame->aPlane 
                                length:_cppFrame->aSize 
                          freeWhenDone:NO];
}

- (const uint8_t *)yPlanePtr { return _cppFrame ? _cppFrame->yPlane : nullptr; }
- (const uint8_t *)uPlanePtr { return _cppFrame ? _cppFrame->uPlane : nullptr; }
- (const uint8_t *)vPlanePtr { return _cppFrame ? _cppFrame->vPlane : nullptr; }
- (const uint8_t *)aPlanePtr { return _cppFrame ? _cppFrame->aPlane : nullptr; }

- (int)yStride { return _cppFrame ? _cppFrame->yStride : 0; }
- (int)uvStride { return _cppFrame ? _cppFrame->uvStride : 0; }
- (int)aStride { return _cppFrame ? _cppFrame->aStride : 0; }

- (int)width { return _cppFrame ? _cppFrame->width : 0; }
- (int)height { return _cppFrame ? _cppFrame->height : 0; }
- (int)frameIndex { return _cppFrame ? _cppFrame->frameIndex : 0; }
- (double)timestamp { return _cppFrame ? _cppFrame->getDurationMs() / 1000.0 : 0; }

@end

//==============================================================================
// AlphaVideoStats Implementation
//==============================================================================

@implementation AlphaVideoStats

- (instancetype)initWithStats:(const webm::DecoderStatsSnapshot &)stats {
    self = [super init];
    if (self) {
        _framesDecoded = stats.framesDecoded;
        _cacheHits = stats.cacheHits;
        _cacheMisses = stats.cacheMisses;
        _avgDecodeTimeMs = stats.getAverageDecodeTimeMs();
        _cacheHitRate = stats.getCacheHitRate();
    }
    return self;
}

@end

//==============================================================================
// AlphaVideoConfig Implementation
//==============================================================================

@implementation AlphaVideoConfig

+ (instancetype)defaultConfig {
    AlphaVideoConfig *config = [[AlphaVideoConfig alloc] init];
    config.decodeThreads = 4;
    config.cacheSize = 250;  // Large enough for most animations (181 frames typical)
    config.preloadAllFrames = YES;
    config.loopPlayback = YES;
    config.playbackSpeed = 1.0;
    config.mode = @"stream";
    config.stream = YES;
    config.streamCacheSize = 30;
    config.streamPrefetchAhead = 15;
    return config;
}

- (webm::PlayerConfig)toCppConfig {
    webm::PlayerConfig cfg;
    cfg.decodeThreads = self.decodeThreads;
    cfg.cacheSize = self.cacheSize;
    cfg.preloadAllFrames = self.preloadAllFrames;
    cfg.loopPlayback = self.loopPlayback;
    cfg.playbackSpeed = self.playbackSpeed;
    
    // Parse mode string to enum
    if ([self.mode isEqualToString:@"preload"]) {
        cfg.mode = webm::PlaybackMode::Preload;
        cfg.stream = false;
    } else if ([self.mode isEqualToString:@"streamAndPreload"]) {
        cfg.mode = webm::PlaybackMode::StreamAndPreload;
        cfg.stream = false;  // Not pure streaming
    } else {
        cfg.mode = webm::PlaybackMode::Stream;
        cfg.stream = true;
    }
    
    cfg.streamCacheSize = self.streamCacheSize > 0 ? self.streamCacheSize : 30;
    cfg.streamPrefetchAhead = self.streamPrefetchAhead > 0 ? self.streamPrefetchAhead : 15;
    return cfg;
}

@end

//==============================================================================
// AlphaVideoPlayer Implementation
//==============================================================================

@interface AlphaVideoPlayer ()
/** The loaded clip's YUV to RGB, as the Metal view's fragment shader takes it. */
- (webm::YuvToRgb)yuvToRgb;
@end

@implementation AlphaVideoPlayer {
    std::unique_ptr<webm::WebMAlphaPlayer> _player;
    dispatch_queue_t _callbackQueue;
}

- (instancetype)init {
    return [self initWithConfig:[AlphaVideoConfig defaultConfig]];
}

- (instancetype)initWithConfig:(AlphaVideoConfig *)config {
    self = [super init];
    if (self) {
        [AlphaVideoCodecs discover];
        _player = std::make_unique<webm::WebMAlphaPlayer>();
        _player->setConfig([config toCppConfig]);
        _callbackQueue = dispatch_get_main_queue();
        
        [self setupCallbacks];
    }
    return self;
}

- (void)setupCallbacks {
    // Use __weak and __block for safe capture in C++ lambdas
    __weak AlphaVideoPlayer *weakSelf = self;
    
    // Frame callback - use CFRunLoopPerformBlock with kCFRunLoopCommonModes
    // to ensure frames are processed during scrolling and other UI interactions
    _player->setFrameCallback([weakSelf](std::shared_ptr<webm::YUVAFrame> frame) {
        AlphaVideoPlayer *strongSelf = weakSelf;
        if (!strongSelf) return;
        
        AlphaVideoFrame *objcFrame = [[AlphaVideoFrame alloc] initWithCppFrame:frame];
        
        // Use CFRunLoopPerformBlock with kCFRunLoopCommonModes for immediate execution
        // This ensures callbacks run even during UI tracking (scrolling)
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            if ([strongSelf.delegate respondsToSelector:@selector(playerDidReceiveFrame:)]) {
                [strongSelf.delegate playerDidReceiveFrame:objcFrame];
            }
        });
        // Wake up the run loop to process the block immediately
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    
    // Event callback - also use CFRunLoopPerformBlock with kCFRunLoopCommonModes.
    // The core calls it from its own methods and threads, which it stops before it's freed.
    webm::WebMAlphaPlayer *core = _player.get();
    _player->setEventCallback([weakSelf, core](const std::string& event, const std::string& data) {
        AlphaVideoPlayer *strongSelf = weakSelf;
        if (!strongSelf) return;
        
        NSString *eventStr = [NSString stringWithUTF8String:event.c_str()];
        NSString *dataStr = [NSString stringWithUTF8String:data.c_str()];
        const std::string code = event == "error" ? core->getLastErrorCode() : std::string();
        NSString *codeStr = code.empty() ? nil : @(code.c_str());
        
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            [strongSelf handleEvent:eventStr data:dataStr code:codeStr];
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
}

- (void)handleEvent:(NSString *)event data:(NSString *)data code:(nullable NSString *)code {
    id<AlphaVideoPlayerDelegate> delegate = self.delegate;
    
    if ([event isEqualToString:@"loaded"]) {
        if ([delegate respondsToSelector:@selector(playerDidLoad:width:height:)]) {
            [delegate playerDidLoad:self.totalFrames width:self.width height:self.height];
        }
    } else if ([event isEqualToString:@"play"]) {
        if ([delegate respondsToSelector:@selector(playerDidStartPlaying)]) {
            [delegate playerDidStartPlaying];
        }
    } else if ([event isEqualToString:@"pause"]) {
        if ([delegate respondsToSelector:@selector(playerDidPause)]) {
            [delegate playerDidPause];
        }
    } else if ([event isEqualToString:@"stop"]) {
        if ([delegate respondsToSelector:@selector(playerDidStop)]) {
            [delegate playerDidStop];
        }
    } else if ([event isEqualToString:@"loop"]) {
        if ([delegate respondsToSelector:@selector(playerDidLoop)]) {
            [delegate playerDidLoop];
        }
    } else if ([event isEqualToString:@"ended"]) {
        if ([delegate respondsToSelector:@selector(playerDidEnd)]) {
            [delegate playerDidEnd];
        }
    } else if ([event isEqualToString:@"error"]) {
        if ([delegate respondsToSelector:@selector(playerDidEncounterError:code:)]) {
            [delegate playerDidEncounterError:data code:code];
        }
    } else if ([event isEqualToString:@"preload_complete"]) {
        // Parse "X frames in Yms"
        NSArray *parts = [data componentsSeparatedByString:@" frames in "];
        if (parts.count == 2) {
            int frames = [parts[0] intValue];
            double ms = [[parts[1] stringByReplacingOccurrencesOfString:@"ms" withString:@""] doubleValue];
            if ([delegate respondsToSelector:@selector(playerPreloadComplete:timeMs:)]) {
                [delegate playerPreloadComplete:frames timeMs:ms];
            }
        }
    }
}

- (void)setVerbose:(BOOL)verbose {
    _player->setVerbose(verbose);
}

- (void)setConfig:(AlphaVideoConfig *)config {
    _player->setConfig([config toCppConfig]);
}

- (AlphaVideoConfig *)getConfig {
    webm::PlayerConfig cfg = _player->getConfig();
    AlphaVideoConfig *config = [[AlphaVideoConfig alloc] init];
    config.decodeThreads = cfg.decodeThreads;
    config.cacheSize = cfg.cacheSize;
    config.preloadAllFrames = cfg.preloadAllFrames;
    config.loopPlayback = cfg.loopPlayback;
    config.playbackSpeed = cfg.playbackSpeed;
    config.stream = cfg.stream;
    config.streamCacheSize = cfg.streamCacheSize;
    config.streamPrefetchAhead = cfg.streamPrefetchAhead;
    
    // Convert mode enum back to string
    switch (cfg.mode) {
        case webm::PlaybackMode::Preload:
            config.mode = @"preload";
            break;
        case webm::PlaybackMode::StreamAndPreload:
            config.mode = @"streamAndPreload";
            break;
        default:
            config.mode = @"stream";
            break;
    }
    return config;
}

- (BOOL)loadFromFile:(NSString *)path {
    return _player->loadFromFile([path UTF8String]);
}

- (BOOL)loadFromData:(NSData *)data {
    return _player->loadFromMemory(
        static_cast<const uint8_t *>(data.bytes),
        data.length
    );
}

- (void)unload {
    _player->unload();
}

- (void)play { _player->play(); }
- (void)pause { _player->pause(); }
- (void)stop { _player->stop(); }

- (void)seekToTime:(double)seconds {
    _player->seekTo(seconds);
}

- (void)seekToFrame:(int)frameIndex {
    _player->seekToFrame(frameIndex);
}

- (void)setPlaybackSpeed:(double)speed {
    _player->setPlaybackSpeed(speed);
}

- (double)playbackSpeed {
    return _player->getPlaybackSpeed();
}

- (AlphaVideoFrame *)getFrame:(int)index {
    auto frame = _player->getFrame(index);
    if (!frame) return nil;
    return [[AlphaVideoFrame alloc] initWithCppFrame:frame];
}

- (AlphaVideoFrame *)getCachedFrame:(int)index {
    auto frame = _player->getCachedFrame(index);
    if (!frame) return nil;
    return [[AlphaVideoFrame alloc] initWithCppFrame:frame];
}

- (AlphaVideoFrame *)getCurrentFrame {
    auto frame = _player->getCurrentFrameData();
    if (!frame) return nil;
    return [[AlphaVideoFrame alloc] initWithCppFrame:frame];
}

- (AlphaVideoStats *)getStats {
    return [[AlphaVideoStats alloc] initWithStats:_player->getStats()];
}

- (void)resetStats {
    _player->resetStats();
}

// Properties
- (BOOL)isLoaded { return _player->isLoaded(); }
- (BOOL)isPlaying { return _player->isPlaying(); }
- (int)currentFrame { return _player->getCurrentFrame(); }
- (double)currentTime { return _player->getCurrentTime(); }
- (double)duration { return _player->getDuration(); }
- (int)totalFrames { return _player->getTotalFrames(); }
- (int)width { return _player->getWidth(); }
- (int)height { return _player->getHeight(); }
- (double)frameRate { return _player->getFrameRate(); }
- (NSString *)decoderName { return [NSString stringWithUTF8String:_player->getDecoderName().c_str()]; }
- (webm::YuvToRgb)yuvToRgb { return _player->getYuvToRgb(); }

+ (NSString *)libraryVersions {
    return [NSString stringWithUTF8String:webm::WebMAlphaPlayer::getLibraryVersions().c_str()];
}

@end

//==============================================================================
// AlphaVideoMetalView Implementation
//==============================================================================

@interface AlphaVideoMetalView () <AlphaVideoPlayerDelegate, MTKViewDelegate>

@property (nonatomic, strong) id<MTLDevice> metalDevice;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLBuffer> vertexBuffer;

// Triple-buffered YUVA textures to prevent tearing
@property (nonatomic, strong) NSArray<id<MTLTexture>> *yTextures;
@property (nonatomic, strong) NSArray<id<MTLTexture>> *uTextures;
@property (nonatomic, strong) NSArray<id<MTLTexture>> *vTextures;
@property (nonatomic, strong) NSArray<id<MTLTexture>> *aTextures;

@property (nonatomic) int textureWidth;
@property (nonatomic) int textureHeight;

// Triple buffering state
@property (nonatomic) int currentTextureIndex;
@property (nonatomic, strong) dispatch_semaphore_t frameSemaphore;

// Frame tracking
@property (nonatomic) int lastRenderedFrame;
@property (nonatomic) BOOL isAnimating;  // Whether we should be animating

// Store latest frame from C++ callback (thread-safe)
@property (atomic, strong) AlphaVideoFrame *latestFrame;

@end

@implementation AlphaVideoMetalView

- (void)setVerbose:(BOOL)verbose {
    _verbose = verbose;
    [self.player setVerbose:verbose];
}

- (instancetype)initWithFrame:(CGRect)frame {
    return [self initWithFrame:frame config:[AlphaVideoConfig defaultConfig]];
}

- (instancetype)initWithFrame:(CGRect)frame config:(AlphaVideoConfig *)config {
    // Create Metal device
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    
    self = [super initWithFrame:frame device:device];
    if (self) {
        _metalDevice = device;
        _player = [[AlphaVideoPlayer alloc] initWithConfig:config];
        _player.delegate = self;
        _autoPlay = YES;
        _loop = YES;
        _playbackSpeed = 1.0;
        _currentTextureIndex = 0;
        _lastRenderedFrame = -1;
        _isAnimating = NO;
        // Allow up to 3 frames in flight
        _frameSemaphore = dispatch_semaphore_create(3);
        
        [self setupMetal];
    }
    return self;
}

- (void)dealloc {
    // Nothing special needed - MTKView handles its own cleanup
}

/**
 * Forwards to the running player rather than only recording the value.
 *
 * `playerDidLoad:` pushes this into the config, which covers a rate set before the video was
 * ready. On its own that is not enough: props change after load too, and a hover that speeds the
 * animation up would otherwise do nothing until the source was reloaded.
 */
- (void)setPlaybackSpeed:(double)playbackSpeed {
    _playbackSpeed = playbackSpeed;
    [_player setPlaybackSpeed:playbackSpeed];
}

- (void)setupMetal {
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.clearColor = MTLClearColorMake(0, 0, 0, 0);
    self.opaque = NO;
    self.layer.opaque = NO;
    self.backgroundColor = [UIColor clearColor];
    self.framebufferOnly = NO;  // Needed for texture operations
    
    // KEY: Use MTKView's built-in continuous rendering loop (like ChatGPT example)
    self.delegate = self;
    self.enableSetNeedsDisplay = NO;  // Don't use setNeedsDisplay, use continuous rendering
    self.preferredFramesPerSecond = 30;  // Until the clip's frame rate is known (playerDidLoad)
    self.paused = YES;  // Start paused, will unpause when playback starts
    
    _commandQueue = [_metalDevice newCommandQueue];
    
    // Create shader library
    NSString *shaderSource = @R"(
        #include <metal_stdlib>
        using namespace metal;
        
        struct VertexOut {
            float4 position [[position]];
            float2 texCoord;
        };
        
        vertex VertexOut vertexShader(
            uint vertexID [[vertex_id]],
            constant float4 *vertices [[buffer(0)]]
        ) {
            VertexOut out;
            out.position = float4(vertices[vertexID].xy, 0.0, 1.0);
            out.texCoord = vertices[vertexID].zw;
            return out;
        }
        
        fragment float4 fragmentShader(
            VertexOut in [[stage_in]],
            texture2d<float> yTexture [[texture(0)]],
            texture2d<float> uTexture [[texture(1)]],
            texture2d<float> vTexture [[texture(2)]],
            texture2d<float> aTexture [[texture(3)]],
            constant float4x4 &yuvToRgb [[buffer(0)]]
        ) {
            constexpr sampler texSampler(mag_filter::linear, min_filter::linear);
            
            float4 yuv = float4(yTexture.sample(texSampler, in.texCoord).r,
                                uTexture.sample(texSampler, in.texCoord).r,
                                vTexture.sample(texSampler, in.texCoord).r, 1.0);
            float a = aTexture.sample(texSampler, in.texCoord).r;
            
            // The clip's range and matrix (webm::yuvToRgbFor)
            float3 rgb = clamp((yuvToRgb * yuv).rgb, 0.0, 1.0);
            
            // Premultiplied alpha for correct compositing
            return float4(rgb * a, a);
        }
    )";
    
    NSError *error;
    id<MTLLibrary> library = [_metalDevice newLibraryWithSource:shaderSource 
                                                       options:nil 
                                                         error:&error];
    if (error) {
        NSLog(@"AlphaVideoMetalView shader compile error: %@", error);
        return;
    }
    
    id<MTLFunction> vertexFunc = [library newFunctionWithName:@"vertexShader"];
    id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"fragmentShader"];
    
    MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDesc.vertexFunction = vertexFunc;
    pipelineDesc.fragmentFunction = fragmentFunc;
    pipelineDesc.colorAttachments[0].pixelFormat = self.colorPixelFormat;
    
    // Enable alpha blending
    pipelineDesc.colorAttachments[0].blendingEnabled = YES;
    pipelineDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pipelineDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipelineDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipelineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    
    _pipelineState = [_metalDevice newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (error) {
        NSLog(@"AlphaVideoMetalView pipeline error: %@", error);
        return;
    }
    
    // Create vertex buffer (fullscreen quad)
    float vertices[] = {
        // position    texCoord
        -1.0, -1.0,    0.0, 1.0,
         1.0, -1.0,    1.0, 1.0,
        -1.0,  1.0,    0.0, 0.0,
         1.0,  1.0,    1.0, 0.0,
    };
    _vertexBuffer = [_metalDevice newBufferWithBytes:vertices 
                                              length:sizeof(vertices)
                                             options:MTLResourceStorageModeShared];
    
    WEBM_VLOG(@"AlphaVideoMetalView Metal setup complete");
}

- (void)createTexturesForWidth:(int)width height:(int)height {
    if (_textureWidth == width && _textureHeight == height) {
        return;  // Already correct size
    }
    
    _textureWidth = width;
    _textureHeight = height;
    
    // Create triple-buffered textures
    NSMutableArray *yTexs = [NSMutableArray arrayWithCapacity:3];
    NSMutableArray *uTexs = [NSMutableArray arrayWithCapacity:3];
    NSMutableArray *vTexs = [NSMutableArray arrayWithCapacity:3];
    NSMutableArray *aTexs = [NSMutableArray arrayWithCapacity:3];
    
    // Y texture descriptor (full resolution)
    MTLTextureDescriptor *yDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    yDesc.usage = MTLTextureUsageShaderRead;
    yDesc.storageMode = MTLStorageModeShared;  // CPU+GPU access
    
    // UV texture descriptor (half resolution, rounded up for odd sizes)
    int uvWidth = (width + 1) / 2;
    int uvHeight = (height + 1) / 2;
    MTLTextureDescriptor *uvDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:uvWidth
                                                                                     height:uvHeight
                                                                                  mipmapped:NO];
    uvDesc.usage = MTLTextureUsageShaderRead;
    uvDesc.storageMode = MTLStorageModeShared;
    
    // Create 3 sets of textures for triple buffering
    for (int i = 0; i < 3; i++) {
        [yTexs addObject:[_metalDevice newTextureWithDescriptor:yDesc]];
        [uTexs addObject:[_metalDevice newTextureWithDescriptor:uvDesc]];
        [vTexs addObject:[_metalDevice newTextureWithDescriptor:uvDesc]];
        [aTexs addObject:[_metalDevice newTextureWithDescriptor:yDesc]];  // Alpha is full resolution
    }
    
    _yTextures = yTexs;
    _uTextures = uTexs;
    _vTextures = vTexs;
    _aTextures = aTexs;
    
    WEBM_VLOG(@"Created triple-buffered textures %dx%d (UV: %dx%d)", width, height, uvWidth, uvHeight);
}

- (BOOL)loadFromPath:(NSString *)path {
    _sourcePath = path;
    return [_player loadFromFile:path];
}

- (void)unloadVideo {
    [_player unload];
    _yTextures = nil;
    _uTextures = nil;
    _vTextures = nil;
    _aTextures = nil;
    _textureWidth = 0;
    _textureHeight = 0;
    _currentTextureIndex = 0;
}

- (void)play { [_player play]; }
- (void)pause { [_player pause]; }
- (void)stop { [_player stop]; }
- (void)seekToFrame:(int)frame { [_player seekToFrame:frame]; }
- (void)seekToTime:(double)seconds { [_player seekToTime:seconds]; }

- (void)renderFrame:(AlphaVideoFrame *)frame {
    if (!frame || !_pipelineState || !_frameSemaphore) {
        NSLog(@"renderFrame early return: frame=%@, pipeline=%@, semaphore=%@",
              frame ? @"YES" : @"NO", _pipelineState ? @"YES" : @"NO", _frameSemaphore ? @"YES" : @"NO");
        return;
    }
    
    // Debug: log render attempts
    static int renderLogCount = 0;
    if (renderLogCount++ < 20 || renderLogCount % 30 == 0) {
        WEBM_VLOG(@"renderFrame called: frame=%d, size=%dx%d", frame.frameIndex, frame.width, frame.height);
    }
    
    // Wait for a texture set to become available (max 3 in flight)
    // Use a short timeout to avoid blocking the main thread too long
    if (dispatch_semaphore_wait(_frameSemaphore, dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC)) != 0) {
        // Timeout - skip this frame to avoid blocking
        if (renderLogCount < 10) NSLog(@"renderFrame timeout waiting for semaphore");
        return;
    }
    
    // Ensure textures exist and are correct size
    [self createTexturesForWidth:frame.width height:frame.height];
    
    if (!_yTextures || _yTextures.count < 3) {
        dispatch_semaphore_signal(_frameSemaphore);
        return;
    }
    
    // Get current texture set and advance index
    int texIdx = _currentTextureIndex;
    _currentTextureIndex = (_currentTextureIndex + 1) % 3;
    
    id<MTLTexture> yTex = _yTextures[texIdx];
    id<MTLTexture> uTex = _uTextures[texIdx];
    id<MTLTexture> vTex = _vTextures[texIdx];
    id<MTLTexture> aTex = _aTextures[texIdx];
    
    // Upload plane data to this texture set
    MTLRegion yRegion = MTLRegionMake2D(0, 0, frame.width, frame.height);
    [yTex replaceRegion:yRegion 
            mipmapLevel:0 
              withBytes:frame.yPlanePtr 
            bytesPerRow:frame.yStride];
    
    [aTex replaceRegion:yRegion 
            mipmapLevel:0 
              withBytes:frame.aPlanePtr 
            bytesPerRow:frame.aStride];
    
    int uvWidth = (frame.width + 1) / 2;
    int uvHeight = (frame.height + 1) / 2;
    MTLRegion uvRegion = MTLRegionMake2D(0, 0, uvWidth, uvHeight);
    
    [uTex replaceRegion:uvRegion 
            mipmapLevel:0 
              withBytes:frame.uPlanePtr 
            bytesPerRow:frame.uvStride];
    
    [vTex replaceRegion:uvRegion 
            mipmapLevel:0 
              withBytes:frame.vPlanePtr 
            bytesPerRow:frame.uvStride];
    
    // Get drawable - use MTKView's currentDrawable when called from drawInMTKView
    // or nextDrawable from layer when called directly
    id<CAMetalDrawable> drawable = self.currentDrawable;
    if (!drawable) {
        // Fallback to getting from layer directly
        CAMetalLayer *metalLayer = (CAMetalLayer *)self.layer;
        drawable = [metalLayer nextDrawable];
    }
    
    if (!drawable) {
        static int noDrawableCount = 0;
        if (noDrawableCount++ < 10) {
            NSLog(@"renderFrame: no drawable available (view size: %.0fx%.0f)", 
                  self.bounds.size.width, self.bounds.size.height);
        }
        dispatch_semaphore_signal(_frameSemaphore);
        return;
    }
    
    static int drawableSuccessCount = 0;
    if (drawableSuccessCount++ < 3) {
        WEBM_VLOG(@"Got drawable: %@ texture size: %lux%lu", drawable, 
              (unsigned long)drawable.texture.width, (unsigned long)drawable.texture.height);
    }
    
    // Use currentRenderPassDescriptor if available, otherwise create manually
    MTLRenderPassDescriptor *passDesc = self.currentRenderPassDescriptor;
    if (!passDesc) {
        passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture = drawable.texture;
    }
    
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    
    // Signal semaphore when GPU completes this frame
    __block dispatch_semaphore_t sem = _frameSemaphore;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull buffer) {
        dispatch_semaphore_signal(sem);
    }];
    
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDesc];
    
    [encoder setRenderPipelineState:_pipelineState];
    [encoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [encoder setFragmentTexture:yTex atIndex:0];
    [encoder setFragmentTexture:uTex atIndex:1];
    [encoder setFragmentTexture:vTex atIndex:2];
    [encoder setFragmentTexture:aTex atIndex:3];
    webm::YuvToRgb yuvToRgb = [_player yuvToRgb];
    [encoder setFragmentBytes:yuvToRgb.m length:sizeof(yuvToRgb.m) atIndex:0];
    
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    
    // Debug: log successful render
    static int commitCount = 0;
    if (commitCount++ < 20 || commitCount % 30 == 0) {
        WEBM_VLOG(@"Frame %d committed to GPU", frame.frameIndex);
    }
}

- (void)renderCurrentFrame {
    AlphaVideoFrame *frame = [_player getCurrentFrame];
    if (frame) {
        self.latestFrame = frame;
        _lastRenderedFrame = -1;
        [self draw];
    }
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    // Handle resize if needed - textures are created dynamically per frame
}

- (void)drawInMTKView:(MTKView *)view {
    // This is called by MTKView at preferredFramesPerSecond (the clip's frame rate)
    // It's the core render loop - equivalent to CADisplayLink callback
    
    // Debug: log that we're being called
    static int callCount = 0;
    if (callCount++ < 10) {
        WEBM_VLOG(@"drawInMTKView called #%d! isAnimating=%d, isLoaded=%d, paused=%d, bounds=%.0fx%.0f, window=%@", 
              callCount, _isAnimating, _player.isLoaded, self.paused, 
              self.bounds.size.width, self.bounds.size.height, self.window);
    }
    
    if (!_player || !_player.isLoaded) {
        return;
    }
    
    // Even if not "animating", we should render if we have a frame
    // This ensures the first frame shows up
    
    // Draw the last frame the C++ clock emitted. Its frame index has already moved on to the
    // next frame, which in the stream modes may not be decoded yet (a slow download, a slow
    // device), so looking that one up in the cache would mostly miss and show nothing new.
    AlphaVideoFrame *frame = self.latestFrame;

    static int debugCounter = 0;
    if (debugCounter++ % 30 == 0) {
        WEBM_VLOG(@"drawInMTKView: latest=%d, lastRendered=%d, isPlaying=%d, isAnimating=%d",
              frame ? frame.frameIndex : -1, _lastRenderedFrame, _player.isPlaying, _isAnimating);
    }

    if (frame && (frame.frameIndex != _lastRenderedFrame || _lastRenderedFrame == -1)) {
        _lastRenderedFrame = frame.frameIndex;
        [self renderFrame:frame];
    }
}

#pragma mark - AlphaVideoPlayerDelegate

- (void)playerDidLoad:(int)totalFrames width:(int)width height:(int)height {
    WEBM_VLOG(@"AlphaVideoMetalView loaded: %d frames, %dx%d", totalFrames, width, height);
    
    // Update config from player settings
    AlphaVideoConfig *config = [_player getConfig];
    config.loopPlayback = _loop;
    config.playbackSpeed = _playbackSpeed;
    [_player setConfig:config];

    // The C++ clock advances frames at the file's rate; redraw at least that often, or a 60 fps
    // clip would show every other frame. MTKView rounds to a rate the display supports.
    double frameRate = _player.frameRate;
    if (frameRate > 0) {
        self.preferredFramesPerSecond = (NSInteger)ceil(frameRate * MAX(_playbackSpeed, 1.0));
    }
    
    // Render first frame
    AlphaVideoFrame *firstFrame = [_player getFrame:0];
    if (firstFrame) {
        WEBM_VLOG(@"First frame details: w=%d h=%d yStride=%d uvStride=%d aStride=%d",
              firstFrame.width, firstFrame.height, 
              firstFrame.yStride, firstFrame.uvStride, firstFrame.aStride);
        
        // Check alpha plane - sample first few bytes
        const uint8_t *aPtr = firstFrame.aPlanePtr;
        if (aPtr) {
            WEBM_VLOG(@"Alpha plane first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                  aPtr[0], aPtr[1], aPtr[2], aPtr[3], aPtr[4], aPtr[5], aPtr[6], aPtr[7]);
        }
        
        // Through draw rather than renderFrame directly: a drawable presented outside the view's
        // draw cycle is handed out again on the next draw, and Metal rejects it.
        self.latestFrame = firstFrame;
        _lastRenderedFrame = -1;
        [self draw];
        WEBM_VLOG(@"First frame rendered via C++ → Metal GPU pipeline");
    }
    
    if (_autoPlay) {
        [_player play];
        // Start MTKView's continuous rendering loop
        self.paused = NO;
        _isAnimating = YES;
        WEBM_VLOG(@"MTKView rendering started for animation (paused=%d, bounds=%.0fx%.0f, delegate=%@)", 
              self.paused, self.bounds.size.width, self.bounds.size.height, self.delegate);
    }
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidLoad:width:height:)]) {
        [_playerDelegate playerDidLoad:totalFrames width:width height:height];
    }
}

- (void)playerDidReceiveFrame:(AlphaVideoFrame *)frame {
    // Store the latest frame for display link to render
    // This is thread-safe due to atomic property
    self.latestFrame = frame;
    
    // While paused the view's draw loop is stopped, but a seek still has to show its frame.
    if (!_isAnimating) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.paused) [self draw];
        });
    }
    
    // Forward to external delegate if needed
    if ([_playerDelegate respondsToSelector:@selector(playerDidReceiveFrame:)]) {
        [_playerDelegate playerDidReceiveFrame:frame];
    }
}

- (void)playerDidStartPlaying {
    // Start MTKView's continuous rendering loop
    self.paused = NO;
    _isAnimating = YES;
    WEBM_VLOG(@"MTKView rendering started (playerDidStartPlaying)");
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidStartPlaying)]) {
        [_playerDelegate playerDidStartPlaying];
    }
}

- (void)playerDidPause {
    // Pause MTKView's rendering loop
    self.paused = YES;
    _isAnimating = NO;
    WEBM_VLOG(@"MTKView rendering paused");
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidPause)]) {
        [_playerDelegate playerDidPause];
    }
}

- (void)playerDidStop {
    self.paused = YES;
    _isAnimating = NO;
    _lastRenderedFrame = -1;
    self.latestFrame = nil;
    if ([_playerDelegate respondsToSelector:@selector(playerDidStop)]) {
        [_playerDelegate playerDidStop];
    }
}

- (void)playerDidLoop {
    // Reset last rendered frame on loop to ensure smooth transition
    _lastRenderedFrame = -1;
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidLoop)]) {
        [_playerDelegate playerDidLoop];
    }
}

- (void)playerDidEnd {
    // Stop MTKView rendering when playback ends
    self.paused = YES;
    _isAnimating = NO;
    _lastRenderedFrame = -1;
    self.latestFrame = nil;
    WEBM_VLOG(@"MTKView rendering stopped (playback ended)");
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidEnd)]) {
        [_playerDelegate playerDidEnd];
    }
}

- (void)playerDidEncounterError:(NSString *)error code:(nullable NSString *)code {
    NSLog(@"AlphaVideoMetalView error: %@", error);
    if ([_playerDelegate respondsToSelector:@selector(playerDidEncounterError:code:)]) {
        [_playerDelegate playerDidEncounterError:error code:code];
    }
}

- (void)playerPreloadComplete:(int)totalFrames timeMs:(double)timeMs {
    WEBM_VLOG(@"Preload complete: %d frames in %.1fms (C++ core)", totalFrames, timeMs);
    if ([_playerDelegate respondsToSelector:@selector(playerPreloadComplete:timeMs:)]) {
        [_playerDelegate playerPreloadComplete:totalFrames timeMs:timeMs];
    }
}

@end

//==============================================================================
// Range readers
//==============================================================================

@implementation AlphaVideoPlayer (SourceReader)

- (BOOL)loadFromReader:(AlphaVideoSourceReader *)reader {
    return _player->loadFromReader(reader.rangeReader);
}

@end

@implementation AlphaVideoMetalView (SourceReader)

- (BOOL)loadFromReader:(AlphaVideoSourceReader *)reader {
    self.sourcePath = nil;
    return [self.player loadFromReader:reader];
}

@end

//==============================================================================
// Local sources
//==============================================================================

NSString *AlphaVideoLocalPathForSource(NSString *uri) {
    if ([uri hasPrefix:@"asset://"]) {
        NSString *relative = [uri substringFromIndex:@"asset://".length];
        relative = [relative stringByRemovingPercentEncoding] ?: relative;
        while ([relative hasPrefix:@"/"]) {
            relative = [relative substringFromIndex:1];
        }
        return relative.length > 0 ? [[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:relative] : nil;
    }
    if ([uri hasPrefix:@"file://"]) {
        NSURL *url = [NSURL URLWithString:uri];
        return url.isFileURL ? url.path : [uri substringFromIndex:@"file://".length];
    }
    if ([uri hasPrefix:@"/"]) {
        return uri;
    }
    return nil;
}
