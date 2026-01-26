/**
 * WebMAlphaPlayerBridge.mm
 * Objective-C++ implementation bridging C++ WebMAlphaPlayer to iOS/Metal
 */

#import "WebMAlphaPlayerBridge.h"
#include "WebMAlphaPlayer.h"
#include <memory>

//==============================================================================
// WebMFrame Implementation
//==============================================================================

@implementation WebMFrame {
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
// WebMPlayerStats Implementation
//==============================================================================

@implementation WebMPlayerStats

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
// WebMPlayerConfig Implementation
//==============================================================================

@implementation WebMPlayerConfig

+ (instancetype)defaultConfig {
    WebMPlayerConfig *config = [[WebMPlayerConfig alloc] init];
    config.decodeThreads = 4;
    config.cacheSize = 250;  // Large enough for most animations (181 frames typical)
    config.preloadAllFrames = YES;
    config.loopPlayback = YES;
    config.playbackSpeed = 1.0;
    config.mode = @"preload";
    config.stream = NO;
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
    if ([self.mode isEqualToString:@"stream"]) {
        cfg.mode = webm::PlaybackMode::Stream;
        cfg.stream = true;
    } else if ([self.mode isEqualToString:@"streamAndPreload"]) {
        cfg.mode = webm::PlaybackMode::StreamAndPreload;
        cfg.stream = false;  // Not pure streaming
    } else {
        cfg.mode = webm::PlaybackMode::Preload;
        cfg.stream = false;
    }
    
    cfg.streamCacheSize = self.streamCacheSize > 0 ? self.streamCacheSize : 30;
    cfg.streamPrefetchAhead = self.streamPrefetchAhead > 0 ? self.streamPrefetchAhead : 15;
    return cfg;
}

@end

//==============================================================================
// WebMAlphaPlayerWrapper Implementation
//==============================================================================

@implementation WebMAlphaPlayerWrapper {
    std::unique_ptr<webm::WebMAlphaPlayer> _player;
    dispatch_queue_t _callbackQueue;
}

- (instancetype)init {
    return [self initWithConfig:[WebMPlayerConfig defaultConfig]];
}

- (instancetype)initWithConfig:(WebMPlayerConfig *)config {
    self = [super init];
    if (self) {
        _player = std::make_unique<webm::WebMAlphaPlayer>();
        _player->setConfig([config toCppConfig]);
        _callbackQueue = dispatch_get_main_queue();
        
        [self setupCallbacks];
    }
    return self;
}

- (void)setupCallbacks {
    // Use __weak and __block for safe capture in C++ lambdas
    __weak WebMAlphaPlayerWrapper *weakSelf = self;
    
    // Frame callback - use CFRunLoopPerformBlock with kCFRunLoopCommonModes
    // to ensure frames are processed during scrolling and other UI interactions
    _player->setFrameCallback([weakSelf](std::shared_ptr<webm::YUVAFrame> frame) {
        WebMAlphaPlayerWrapper *strongSelf = weakSelf;
        if (!strongSelf) return;
        
        WebMFrame *objcFrame = [[WebMFrame alloc] initWithCppFrame:frame];
        
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
    
    // Event callback - also use CFRunLoopPerformBlock with kCFRunLoopCommonModes
    _player->setEventCallback([weakSelf](const std::string& event, const std::string& data) {
        WebMAlphaPlayerWrapper *strongSelf = weakSelf;
        if (!strongSelf) return;
        
        NSString *eventStr = [NSString stringWithUTF8String:event.c_str()];
        NSString *dataStr = [NSString stringWithUTF8String:data.c_str()];
        
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            [strongSelf handleEvent:eventStr data:dataStr];
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
}

- (void)handleEvent:(NSString *)event data:(NSString *)data {
    id<WebMAlphaPlayerDelegate> delegate = self.delegate;
    
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
        if ([delegate respondsToSelector:@selector(playerDidEncounterError:)]) {
            [delegate playerDidEncounterError:data];
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

- (void)setConfig:(WebMPlayerConfig *)config {
    _player->setConfig([config toCppConfig]);
}

- (WebMPlayerConfig *)getConfig {
    webm::PlayerConfig cfg = _player->getConfig();
    WebMPlayerConfig *config = [[WebMPlayerConfig alloc] init];
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
        case webm::PlaybackMode::Stream:
            config.mode = @"stream";
            break;
        case webm::PlaybackMode::StreamAndPreload:
            config.mode = @"streamAndPreload";
            break;
        default:
            config.mode = @"preload";
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

- (BOOL)loadFromURL:(NSURL *)url {
    if (url.isFileURL) {
        return [self loadFromFile:url.path];
    }
    return _player->loadFromUrl([url.absoluteString UTF8String]);
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

- (WebMFrame *)getFrame:(int)index {
    auto frame = _player->getFrame(index);
    if (!frame) return nil;
    return [[WebMFrame alloc] initWithCppFrame:frame];
}

- (WebMFrame *)getCachedFrame:(int)index {
    auto frame = _player->getCachedFrame(index);
    if (!frame) return nil;
    return [[WebMFrame alloc] initWithCppFrame:frame];
}

- (WebMFrame *)getCurrentFrame {
    auto frame = _player->getCurrentFrameData();
    if (!frame) return nil;
    return [[WebMFrame alloc] initWithCppFrame:frame];
}

- (WebMPlayerStats *)getStats {
    return [[WebMPlayerStats alloc] initWithStats:_player->getStats()];
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

+ (NSString *)ffmpegVersion {
    return [NSString stringWithUTF8String:webm::WebMAlphaPlayer::getFFmpegVersion().c_str()];
}

+ (BOOL)isFFmpegInitialized {
    return webm::WebMAlphaPlayer::isFFmpegInitialized();
}

@end

//==============================================================================
// WebMAlphaMetalPlayerView Implementation
//==============================================================================

@interface WebMAlphaMetalPlayerView () <WebMAlphaPlayerDelegate, MTKViewDelegate>

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
@property (atomic, strong) WebMFrame *latestFrame;

@end

@implementation WebMAlphaMetalPlayerView

- (instancetype)initWithFrame:(CGRect)frame {
    return [self initWithFrame:frame config:[WebMPlayerConfig defaultConfig]];
}

- (instancetype)initWithFrame:(CGRect)frame config:(WebMPlayerConfig *)config {
    // Create Metal device
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    
    self = [super initWithFrame:frame device:device];
    if (self) {
        _metalDevice = device;
        _player = [[WebMAlphaPlayerWrapper alloc] initWithConfig:config];
        _player.delegate = self;
        _autoplay = YES;
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
    self.preferredFramesPerSecond = 30;  // Match video frame rate (30fps)
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
            texture2d<float> aTexture [[texture(3)]]
        ) {
            constexpr sampler texSampler(mag_filter::linear, min_filter::linear);
            
            float y = yTexture.sample(texSampler, in.texCoord).r;
            float u = uTexture.sample(texSampler, in.texCoord).r - 0.5;
            float v = vTexture.sample(texSampler, in.texCoord).r - 0.5;
            float a = aTexture.sample(texSampler, in.texCoord).r;
            
            // BT.601 YUV to RGB conversion
            float r = y + 1.402 * v;
            float g = y - 0.344136 * u - 0.714136 * v;
            float b = y + 1.772 * u;
            
            // Clamp to valid range
            r = clamp(r, 0.0, 1.0);
            g = clamp(g, 0.0, 1.0);
            b = clamp(b, 0.0, 1.0);
            
            // Premultiplied alpha for correct compositing
            return float4(r * a, g * a, b * a, a);
        }
    )";
    
    NSError *error;
    id<MTLLibrary> library = [_metalDevice newLibraryWithSource:shaderSource 
                                                       options:nil 
                                                         error:&error];
    if (error) {
        NSLog(@"❌ WebMAlphaMetalPlayerView shader compile error: %@", error);
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
        NSLog(@"❌ WebMAlphaMetalPlayerView pipeline error: %@", error);
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
    
    NSLog(@"✅ WebMAlphaMetalPlayerView Metal setup complete");
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
    
    // UV texture descriptor (half resolution)
    int uvWidth = width / 2;
    int uvHeight = height / 2;
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
    
    NSLog(@"📐 Created triple-buffered textures %dx%d (UV: %dx%d)", width, height, uvWidth, uvHeight);
}

- (BOOL)loadFromPath:(NSString *)path {
    _sourcePath = path;
    return [_player loadFromFile:path];
}

- (BOOL)loadFromURL:(NSURL *)url {
    _sourceURL = url;
    return [_player loadFromURL:url];
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

- (void)renderFrame:(WebMFrame *)frame {
    if (!frame || !_pipelineState || !_frameSemaphore) {
        NSLog(@"❌ renderFrame early return: frame=%@, pipeline=%@, semaphore=%@",
              frame ? @"YES" : @"NO", _pipelineState ? @"YES" : @"NO", _frameSemaphore ? @"YES" : @"NO");
        return;
    }
    
    // Debug: log render attempts
    static int renderLogCount = 0;
    if (renderLogCount++ < 20 || renderLogCount % 30 == 0) {
        NSLog(@"🎨 renderFrame called: frame=%d, size=%dx%d", frame.frameIndex, frame.width, frame.height);
    }
    
    // Wait for a texture set to become available (max 3 in flight)
    // Use a short timeout to avoid blocking the main thread too long
    if (dispatch_semaphore_wait(_frameSemaphore, dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC)) != 0) {
        // Timeout - skip this frame to avoid blocking
        if (renderLogCount < 10) NSLog(@"⏱️ renderFrame timeout waiting for semaphore");
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
    
    int uvWidth = frame.width / 2;
    int uvHeight = frame.height / 2;
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
            NSLog(@"⚠️ renderFrame: no drawable available (view size: %.0fx%.0f)", 
                  self.bounds.size.width, self.bounds.size.height);
        }
        dispatch_semaphore_signal(_frameSemaphore);
        return;
    }
    
    static int drawableSuccessCount = 0;
    if (drawableSuccessCount++ < 3) {
        NSLog(@"✅ Got drawable: %@ texture size: %lux%lu", drawable, 
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
    
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    
    // Debug: log successful render
    static int commitCount = 0;
    if (commitCount++ < 20 || commitCount % 30 == 0) {
        NSLog(@"✅ Frame %d committed to GPU", frame.frameIndex);
    }
}

- (void)renderCurrentFrame {
    WebMFrame *frame = [_player getCurrentFrame];
    if (frame) {
        self.latestFrame = frame;
        [self renderFrame:frame];
    }
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    // Handle resize if needed - textures are created dynamically per frame
}

- (void)drawInMTKView:(MTKView *)view {
    // This is called by MTKView at preferredFramesPerSecond (30fps)
    // It's the core render loop - equivalent to CADisplayLink callback
    
    // Debug: log that we're being called
    static int callCount = 0;
    if (callCount++ < 10) {
        NSLog(@"🔄 drawInMTKView called #%d! isAnimating=%d, isLoaded=%d, paused=%d, bounds=%.0fx%.0f, window=%@", 
              callCount, _isAnimating, _player.isLoaded, self.paused, 
              self.bounds.size.width, self.bounds.size.height, self.window);
    }
    
    if (!_player || !_player.isLoaded) {
        return;
    }
    
    // Even if not "animating", we should render if we have a frame
    // This ensures the first frame shows up
    
    // Poll the current frame index from C++ player
    int currentFrame = _player.currentFrame;
    
    // Debug log periodically
    static int debugCounter = 0;
    if (debugCounter++ % 30 == 0) {
        NSLog(@"🎞️ drawInMTKView: frame=%d, lastRendered=%d, isPlaying=%d, isAnimating=%d", 
              currentFrame, _lastRenderedFrame, _player.isPlaying, _isAnimating);
    }
    
    // Render if frame changed OR if we haven't rendered anything yet
    if ((currentFrame != _lastRenderedFrame && currentFrame >= 0) || _lastRenderedFrame == -1) {
        int frameToGet = currentFrame >= 0 ? currentFrame : 0;
        
        // Get frame from cache (thread-safe, no decoding)
        WebMFrame *frame = [_player getCachedFrame:frameToGet];
        
        // Debug: log cache retrieval
        static int cacheLogCount = 0;
        if (cacheLogCount++ < 20 || cacheLogCount % 60 == 0) {
            NSLog(@"📦 getCachedFrame(%d): %@ (lastRendered=%d)", 
                  frameToGet, frame ? @"GOT FRAME" : @"NIL!", _lastRenderedFrame);
        }
        
        if (frame) {
            _lastRenderedFrame = currentFrame >= 0 ? currentFrame : 0;
            self.latestFrame = frame;
            [self renderFrame:frame];
        } else {
            // Frame not in cache - this shouldn't happen if preload worked
            NSLog(@"⚠️ Frame %d not in cache!", frameToGet);
        }
    }
}

#pragma mark - WebMAlphaPlayerDelegate

- (void)playerDidLoad:(int)totalFrames width:(int)width height:(int)height {
    NSLog(@"🎬 WebMAlphaMetalPlayerView loaded: %d frames, %dx%d", totalFrames, width, height);
    
    // Update config from player settings
    WebMPlayerConfig *config = [_player getConfig];
    config.loopPlayback = _loop;
    config.playbackSpeed = _playbackSpeed;
    [_player setConfig:config];
    
    // Render first frame
    WebMFrame *firstFrame = [_player getFrame:0];
    if (firstFrame) {
        NSLog(@"🔍 First frame details: w=%d h=%d yStride=%d uvStride=%d aStride=%d",
              firstFrame.width, firstFrame.height, 
              firstFrame.yStride, firstFrame.uvStride, firstFrame.aStride);
        
        // Check alpha plane - sample first few bytes
        const uint8_t *aPtr = firstFrame.aPlanePtr;
        if (aPtr) {
            NSLog(@"🔍 Alpha plane first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                  aPtr[0], aPtr[1], aPtr[2], aPtr[3], aPtr[4], aPtr[5], aPtr[6], aPtr[7]);
        }
        
        self.latestFrame = firstFrame;
        [self renderFrame:firstFrame];
        _lastRenderedFrame = 0;
        NSLog(@"🚀 First frame rendered via C++ → Metal GPU pipeline");
    }
    
    if (_autoplay) {
        [_player play];
        // Start MTKView's continuous rendering loop
        self.paused = NO;
        _isAnimating = YES;
        NSLog(@"▶️ MTKView rendering started for animation (paused=%d, bounds=%.0fx%.0f, delegate=%@)", 
              self.paused, self.bounds.size.width, self.bounds.size.height, self.delegate);
    }
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidLoad:width:height:)]) {
        [_playerDelegate playerDidLoad:totalFrames width:width height:height];
    }
}

- (void)playerDidReceiveFrame:(WebMFrame *)frame {
    // Store the latest frame for display link to render
    // This is thread-safe due to atomic property
    self.latestFrame = frame;
    
    // Forward to external delegate if needed
    if ([_playerDelegate respondsToSelector:@selector(playerDidReceiveFrame:)]) {
        [_playerDelegate playerDidReceiveFrame:frame];
    }
}

- (void)playerDidStartPlaying {
    // Start MTKView's continuous rendering loop
    self.paused = NO;
    _isAnimating = YES;
    NSLog(@"▶️ MTKView rendering started (playerDidStartPlaying)");
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidStartPlaying)]) {
        [_playerDelegate playerDidStartPlaying];
    }
}

- (void)playerDidPause {
    // Pause MTKView's rendering loop
    self.paused = YES;
    _isAnimating = NO;
    NSLog(@"⏸️ MTKView rendering paused");
    
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
    NSLog(@"⏹️ MTKView rendering stopped (playback ended)");
    
    if ([_playerDelegate respondsToSelector:@selector(playerDidEnd)]) {
        [_playerDelegate playerDidEnd];
    }
}

- (void)playerDidEncounterError:(NSString *)error {
    NSLog(@"❌ WebMAlphaMetalPlayerView error: %@", error);
    if ([_playerDelegate respondsToSelector:@selector(playerDidEncounterError:)]) {
        [_playerDelegate playerDidEncounterError:error];
    }
}

- (void)playerPreloadComplete:(int)totalFrames timeMs:(double)timeMs {
    NSLog(@"✅ Preload complete: %d frames in %.1fms (C++ core)", totalFrames, timeMs);
    if ([_playerDelegate respondsToSelector:@selector(playerPreloadComplete:timeMs:)]) {
        [_playerDelegate playerPreloadComplete:totalFrames timeMs:timeMs];
    }
}

@end
