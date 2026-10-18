/**
 * AlphaVideoVPXCodec.mm
 * The vpx codec module (libvpx: VP9) on iOS. AlphaVideoCodecs finds this class by name and
 * registers its plugin list.
 */

#import "AlphaVideoVPX.h"
#include "VpxDecoderPlugin.h"

@implementation AlphaVideoVPXCodec

+ (const void *)pluginList {
    return reinterpret_cast<const void *>(&alpha_video_vpx_plugins);
}

@end
