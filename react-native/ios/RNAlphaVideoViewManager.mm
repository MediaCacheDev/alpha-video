//
//  RNAlphaVideoViewManager.mm
//  React Native view manager for AlphaVideoView
//

#import <React/RCTViewManager.h>
#import <React/RCTBridge.h>
#import <React/RCTUIManager.h>
#import "AlphaVideo.h"

#pragma mark - RNAlphaVideoView

/** AlphaVideoView with the React Native props and events. */
@interface RNAlphaVideoView : AlphaVideoView <AlphaVideoViewDelegate>

// `source` prop: { uri }
@property (nonatomic, copy) NSDictionary *reactSource;
// Deprecated: use mode
@property (nonatomic, assign) BOOL stream;

@property (nonatomic, copy) RCTDirectEventBlock onLoad;
@property (nonatomic, copy) RCTDirectEventBlock onError;
@property (nonatomic, copy) RCTDirectEventBlock onEnd;

@end

@implementation RNAlphaVideoView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.delegate = self;
    }
    return self;
}

- (void)setReactSource:(NSDictionary *)reactSource {
    _reactSource = [reactSource copy];
    self.source = reactSource[@"uri"];
}

- (void)setStream:(BOOL)stream {
    _stream = stream;
    self.mode = stream ? @"stream" : @"preload";
}

- (void)alphaVideoView:(AlphaVideoView *)view didLoadFrames:(int)totalFrames width:(int)width height:(int)height {
    if (self.onLoad) {
        self.onLoad(@{
            @"totalFrames": @(totalFrames),
            @"width": @(width),
            @"height": @(height)
        });
    }
}

- (void)alphaVideoView:(AlphaVideoView *)view didFailWithError:(NSString *)error {
    if (self.onError) {
        self.onError(@{@"error": error});
    }
}

- (void)alphaVideoViewDidEnd:(AlphaVideoView *)view {
    if (self.onEnd) {
        self.onEnd(@{});
    }
}

@end

#pragma mark - RNAlphaVideoViewManager

@interface RNAlphaVideoViewManager : RCTViewManager
@end

@implementation RNAlphaVideoViewManager

RCT_EXPORT_MODULE(RNAlphaVideoView)

+ (BOOL)requiresMainQueueSetup {
    return YES;
}

- (UIView *)view {
    return [[RNAlphaVideoView alloc] initWithFrame:CGRectZero];
}

RCT_REMAP_VIEW_PROPERTY(source, reactSource, NSDictionary)
RCT_EXPORT_VIEW_PROPERTY(autoPlay, BOOL)
RCT_EXPORT_VIEW_PROPERTY(loop, BOOL)
RCT_EXPORT_VIEW_PROPERTY(paused, BOOL)
RCT_EXPORT_VIEW_PROPERTY(playbackSpeed, double)
RCT_EXPORT_VIEW_PROPERTY(cacheKey, NSString)
RCT_EXPORT_VIEW_PROPERTY(mode, NSString)
RCT_EXPORT_VIEW_PROPERTY(stream, BOOL)
RCT_EXPORT_VIEW_PROPERTY(streamCacheSize, int)
RCT_EXPORT_VIEW_PROPERTY(streamPrefetchAhead, int)
RCT_EXPORT_VIEW_PROPERTY(useDecodeQueue, BOOL)
RCT_EXPORT_VIEW_PROPERTY(verbose, BOOL)

RCT_EXPORT_VIEW_PROPERTY(onLoad, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onError, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onEnd, RCTDirectEventBlock)

- (void)withView:(NSNumber *)reactTag do:(void (^)(RNAlphaVideoView *view))block {
    [self.bridge.uiManager addUIBlock:^(RCTUIManager *uiManager, NSDictionary<NSNumber *, UIView *> *viewRegistry) {
        RNAlphaVideoView *view = (RNAlphaVideoView *)viewRegistry[reactTag];
        if ([view isKindOfClass:[RNAlphaVideoView class]]) {
            block(view);
        }
    }];
}

RCT_EXPORT_METHOD(play:(nonnull NSNumber *)reactTag) {
    [self withView:reactTag do:^(RNAlphaVideoView *view) { [view play]; }];
}

RCT_EXPORT_METHOD(pause:(nonnull NSNumber *)reactTag) {
    [self withView:reactTag do:^(RNAlphaVideoView *view) { [view pause]; }];
}

RCT_EXPORT_METHOD(stop:(nonnull NSNumber *)reactTag) {
    [self withView:reactTag do:^(RNAlphaVideoView *view) { [view stop]; }];
}

RCT_EXPORT_METHOD(seekToFrame:(nonnull NSNumber *)reactTag frame:(nonnull NSNumber *)frame) {
    [self withView:reactTag do:^(RNAlphaVideoView *view) { [view seekToFrame:[frame intValue]]; }];
}

@end
