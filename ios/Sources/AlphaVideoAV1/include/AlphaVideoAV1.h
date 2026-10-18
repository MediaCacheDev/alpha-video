/**
 * AlphaVideoAV1.h
 * The av1 codec module: dav1d, for AV1 (8- to 12-bit). Linking it is enough; AlphaVideo registers
 * it itself.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface AlphaVideoAV1Codec : NSObject

/** The module's AlphaVideoDecoderPluginList, for AlphaVideoCodecs registerPluginList:error:. */
+ (const void *)pluginList;

@end

NS_ASSUME_NONNULL_END
