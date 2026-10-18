#ifndef ALPHA_VIDEO_DAV1D_DECODER_PLUGIN_H
#define ALPHA_VIDEO_DAV1D_DECODER_PLUGIN_H

#include "AlphaVideoDecoderPlugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The av1 codec module's plugins: dav1d (8-, 10- and 12-bit). An AlphaVideoDecoderPluginList. */
const AlphaVideoDecoderPlugin* const* alpha_video_av1_plugins(int* count);

#ifdef __cplusplus
}
#endif

#endif
