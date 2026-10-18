#include <jni.h>

#include "VpxDecoderPlugin.h"

extern "C" JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_vpx_VpxCodec_nativePluginList(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&alpha_video_vpx_plugins);
}
