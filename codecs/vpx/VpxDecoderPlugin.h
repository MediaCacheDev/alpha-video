#ifndef ALPHA_VIDEO_VPX_DECODER_PLUGIN_H
#define ALPHA_VIDEO_VPX_DECODER_PLUGIN_H

#include "AlphaVideoDecoderPlugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The vpx codec module's plugins: libvpx's VP9 decoder (8-bit). An AlphaVideoDecoderPluginList. */
const AlphaVideoDecoderPlugin* const* alpha_video_vpx_plugins(int* count);

#ifdef __cplusplus
}
#endif

#endif
