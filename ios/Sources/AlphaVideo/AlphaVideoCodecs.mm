/**
 * AlphaVideoCodecs.mm
 * Finds the linked codec modules by class name and registers their decoders with the C++ core.
 */

#import "AlphaVideoInternal.h"
#include "VideoDecoder.h"
#include "WebMAlphaPlayer.h"

NSString *const AlphaVideoErrorNoDecoder = @(webm::WebMAlphaPlayer::kErrorNoDecoder);

typedef const void *(*AlphaVideoPluginListGetter)(id, SEL);

@implementation AlphaVideoCodecs

+ (void)discover {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // Each module's class has +pluginList, its AlphaVideoDecoderPluginList (AlphaVideoVPX.h,
        // AlphaVideoAV1.h). Static pods link with -ObjC and SwiftPM links source targets as
        // object files, so a linked module's class is in the app though nothing references it.
        for (NSString *name in @[ @"AlphaVideoVPXCodec", @"AlphaVideoAV1Codec" ]) {
            Class module = NSClassFromString(name);
            SEL selector = NSSelectorFromString(@"pluginList");
            if (!module || ![module respondsToSelector:selector]) continue;
            auto getter = (AlphaVideoPluginListGetter)[module methodForSelector:selector];
            NSError *error;
            if (![self registerList:getter(module, selector) error:&error]) {
                NSLog(@"AlphaVideo: %@: %@", name, error.localizedDescription);
            }
        }
    });
}

+ (BOOL)registerList:(const void *)pluginList error:(NSError **)error {
    std::string message = "No plugin list";
    if (pluginList &&
        webm::registerDecoderPlugins(reinterpret_cast<AlphaVideoDecoderPluginList>(const_cast<void *>(pluginList)), &message)) {
        return YES;
    }
    if (error) {
        *error = [NSError errorWithDomain:@"AlphaVideo"
                                     code:1
                                 userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
    }
    return NO;
}

+ (BOOL)registerPluginList:(const void *)pluginList error:(NSError **)error {
    [self discover];
    return [self registerList:pluginList error:error];
}

+ (NSArray<NSString *> *)softwareDecoders {
    [self discover];
    NSMutableArray<NSString *> *decoders = [NSMutableArray array];
    for (const std::string &decoder : webm::registeredDecoders()) {
        [decoders addObject:@(decoder.c_str())];
    }
    return decoders;
}

@end
