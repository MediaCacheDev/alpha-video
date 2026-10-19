package dev.mediacache.alphavideo.vpx;

/**
 * The vpx codec module: libvpx's VP8 and VP9 decoders for alpha-video's C++ core. alpha-video finds this
 * class on the classpath and registers it (dev.mediacache.alphavideo.AlphaVideoCodecs); apps don't
 * call it.
 */
public final class VpxCodec {
    static {
        System.loadLibrary("alphavideo_vpx");
    }

    private VpxCodec() {}

    /** The address of the module's native AlphaVideoDecoderPluginList. */
    public static long pluginList() {
        return nativePluginList();
    }

    private static native long nativePluginList();
}
