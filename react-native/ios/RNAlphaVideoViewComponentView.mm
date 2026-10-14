//
//  RNAlphaVideoViewComponentView.mm
//  Fabric component view for AlphaVideoView (new architecture)
//

#ifdef RCT_NEW_ARCH_ENABLED

#import <React/RCTConversions.h>
#import <React/RCTViewComponentView.h>

#import <react/renderer/components/RNAlphaVideoSpec/ComponentDescriptors.h>
#import <react/renderer/components/RNAlphaVideoSpec/EventEmitters.h>
#import <react/renderer/components/RNAlphaVideoSpec/Props.h>
#import <react/renderer/components/RNAlphaVideoSpec/RCTComponentViewHelpers.h>

#import "AlphaVideo.h"

using namespace facebook::react;

static const std::shared_ptr<const RNAlphaVideoViewProps> &RNAlphaVideoViewDefaultProps()
{
  static const auto defaultProps = std::make_shared<const RNAlphaVideoViewProps>();
  return defaultProps;
}

@interface RNAlphaVideoViewComponentView : RCTViewComponentView <RCTRNAlphaVideoViewViewProtocol, AlphaVideoViewDelegate>
@end

@implementation RNAlphaVideoViewComponentView {
  AlphaVideoView *_player;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider
{
  return concreteComponentDescriptorProvider<RNAlphaVideoViewComponentDescriptor>();
}

// A player holds its decoded frames, so each mount gets a new one.
+ (BOOL)shouldBeRecycled
{
  return NO;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  if (self = [super initWithFrame:frame]) {
    _props = RNAlphaVideoViewDefaultProps();
    [self createPlayer];
  }
  return self;
}

- (void)createPlayer
{
  _player = [[AlphaVideoView alloc] initWithFrame:self.bounds];
  _player.delegate = self;
  self.contentView = _player;
}

// Older React Native versions ignore shouldBeRecycled and recycle every component view.
- (void)prepareForRecycle
{
  [super prepareForRecycle];
  [_player stop];
  [self createPlayer];
  _props = RNAlphaVideoViewDefaultProps();
}

// Only the props that changed: the player's setters act (pause, play, reload), and its defaults
// match the spec's.
- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps
{
  const auto &previous = *std::static_pointer_cast<const RNAlphaVideoViewProps>(_props);
  const auto &next = *std::static_pointer_cast<const RNAlphaVideoViewProps>(props);

  if (previous.mode != next.mode) _player.mode = RCTNSStringFromString(toString(next.mode));
  if (previous.streamCacheSize != next.streamCacheSize) _player.streamCacheSize = next.streamCacheSize;
  if (previous.streamPrefetchAhead != next.streamPrefetchAhead) _player.streamPrefetchAhead = next.streamPrefetchAhead;
  if (previous.useDecodeQueue != next.useDecodeQueue) _player.useDecodeQueue = next.useDecodeQueue;
  if (previous.verbose != next.verbose) _player.verbose = next.verbose;
  if (previous.autoPlay != next.autoPlay) _player.autoPlay = next.autoPlay;
  if (previous.loop != next.loop) _player.loop = next.loop;
  if (previous.paused != next.paused) _player.paused = next.paused;
  if (previous.playbackSpeed != next.playbackSpeed) _player.playbackSpeed = next.playbackSpeed;
  if (previous.cacheKey != next.cacheKey) _player.cacheKey = RCTNSStringFromStringNilIfEmpty(next.cacheKey);
  if (previous.source.uri != next.source.uri) _player.source = RCTNSStringFromStringNilIfEmpty(next.source.uri);

  [super updateProps:props oldProps:oldProps];
}

#pragma mark - AlphaVideoViewDelegate

- (std::shared_ptr<const RNAlphaVideoViewEventEmitter>)playerEventEmitter
{
  return std::static_pointer_cast<const RNAlphaVideoViewEventEmitter>(_eventEmitter);
}

- (void)alphaVideoView:(AlphaVideoView *)view didLoadFrames:(int)totalFrames width:(int)width height:(int)height
{
  if (auto emitter = [self playerEventEmitter]) {
    // iOS always decodes on the C++ core, so the decoder prop has nothing to choose
    emitter->onLoad({.totalFrames = totalFrames, .width = width, .height = height, .decoder = "ffmpeg (libvpx-vp9)"});
  }
}

- (void)alphaVideoView:(AlphaVideoView *)view didFailWithError:(NSString *)error
{
  if (auto emitter = [self playerEventEmitter]) {
    emitter->onError({.error = RCTStringFromNSString(error)});
  }
}

- (void)alphaVideoViewDidEnd:(AlphaVideoView *)view
{
  if (auto emitter = [self playerEventEmitter]) {
    emitter->onEnd({});
  }
}

#pragma mark - Commands

- (void)handleCommand:(const NSString *)commandName args:(const NSArray *)args
{
  RCTRNAlphaVideoViewHandleCommand(self, commandName, args);
}

- (void)play
{
  [_player play];
}

- (void)pause
{
  [_player pause];
}

- (void)stop
{
  [_player stop];
}

- (void)seekToFrame:(NSInteger)frame
{
  [_player seekToFrame:(int)frame];
}

- (void)seekToTime:(double)seconds
{
  [_player seekToTime:seconds];
}

@end

// Older React Native versions look the class up by this name instead of codegenConfig.ios.componentProvider.
extern "C" Class<RCTComponentViewProtocol> RNAlphaVideoViewCls(void)
{
  return RNAlphaVideoViewComponentView.class;
}

#endif
