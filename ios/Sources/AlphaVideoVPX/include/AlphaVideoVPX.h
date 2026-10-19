/**
 * AlphaVideoVPX.h
 * The vpx codec module: libvpx, for VP8 and VP9. Linking it is enough; AlphaVideo registers it itself.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface AlphaVideoVPXCodec : NSObject

/** The module's AlphaVideoDecoderPluginList, for AlphaVideoCodecs registerPluginList:error:. */
+ (const void *)pluginList;

@end

NS_ASSUME_NONNULL_END
