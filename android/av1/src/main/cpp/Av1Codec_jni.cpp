#include <jni.h>

#include "Dav1dDecoderPlugin.h"

extern "C" JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_av1_Av1Codec_nativePluginList(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&alpha_video_av1_plugins);
}
