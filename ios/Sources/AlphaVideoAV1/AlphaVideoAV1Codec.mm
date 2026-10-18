/**
 * AlphaVideoAV1Codec.mm
 * The av1 codec module (dav1d: AV1, 8- to 12-bit) on iOS. AlphaVideoCodecs finds this class by
 * name and registers its plugin list.
 */

#import "AlphaVideoAV1.h"
#include "Dav1dDecoderPlugin.h"

@implementation AlphaVideoAV1Codec

+ (const void *)pluginList {
    return reinterpret_cast<const void *>(&alpha_video_av1_plugins);
}

@end
