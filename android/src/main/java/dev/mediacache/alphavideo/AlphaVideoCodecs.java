package dev.mediacache.alphavideo;

import android.util.Log;

/**
 * The software decoders the C++ core plays with, for files and devices MediaCodec can't take
 * (see {@link AlphaVideoView#setDecoder}). The codec modules, {@code dev.mediacache:alpha-video-vpx}
 * (libvpx, VP9) and {@code alpha-video-av1} (dav1d), register themselves when they're on the
 * classpath: nothing to call. Without them the core plays through MediaCodec alone, and a file it
 * would decode in software reports {@link AlphaVideoView#ERROR_NO_DECODER}.
 */
public final class AlphaVideoCodecs {
    private static final String TAG = "AlphaVideoCodecs";

    // Each module's class has a static pluginList(), its AlphaVideoDecoderPluginList's address
    private static final String[] MODULES = {
        "dev.mediacache.alphavideo.vpx.VpxCodec",
        "dev.mediacache.alphavideo.av1.Av1Codec",
    };

    private static boolean discovered;

    static {
        System.loadLibrary("alphavideo");
    }

    private AlphaVideoCodecs() {}

    /**
     * Registers another software decoder: {@code pluginList} is the address of a native
     * {@code AlphaVideoDecoderPluginList} (cpp/AlphaVideoDecoderPlugin.h). Registered decoders come
     * after the modules', which a stream tries first.
     *
     * @throws IllegalArgumentException for a decoder built for another plugin ABI
     */
    public static synchronized void register(long pluginList) {
        discover();
        String error = nativeRegister(pluginList);
        if (error != null) throw new IllegalArgumentException(error);
    }

    /** The registered software decoders, e.g. {"libvpx v1.14.0", "dav1d 1.5.1"}; empty with none. */
    public static String[] softwareDecoders() {
        discover();
        return nativeSoftwareDecoders();
    }

    /** Registers the codec modules on the classpath, once. */
    static synchronized void discover() {
        if (discovered) return;
        discovered = true;
        for (String name : MODULES) {
            Class<?> module;
            try {
                module = Class.forName(name);
            } catch (ClassNotFoundException e) {
                continue;
            } catch (LinkageError e) {
                Log.e(TAG, name + " is there but its native library didn't load", e);
                continue;
            }
            try {
                String error = nativeRegister((Long) module.getMethod("pluginList").invoke(null));
                if (error != null) Log.e(TAG, name + ": " + error);
            } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
                Log.e(TAG, name + " didn't register", e);
            }
        }
    }

    private static native String nativeRegister(long pluginList);
    private static native String[] nativeSoftwareDecoders();
}
