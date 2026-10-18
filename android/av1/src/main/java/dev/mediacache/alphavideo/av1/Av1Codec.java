package dev.mediacache.alphavideo.av1;

/**
 * The av1 codec module: dav1d (8- to 12-bit AV1) for alpha-video's C++ core. alpha-video finds
 * this class on the classpath and registers it (dev.mediacache.alphavideo.AlphaVideoCodecs); apps
 * don't call it.
 */
public final class Av1Codec {
    static {
        System.loadLibrary("alphavideo_av1");
    }

    private Av1Codec() {}

    /** The address of the module's native AlphaVideoDecoderPluginList. */
    public static long pluginList() {
        return nativePluginList();
    }

    private static native long nativePluginList();
}
