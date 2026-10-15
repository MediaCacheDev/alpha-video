package dev.mediacache.alphavideo;

import java.nio.ByteBuffer;

/**
 * JNI bridge to C++ WebMAlphaPlayer core
 * Provides high-performance VP9+Alpha WebM playback
 */
public class WebMAlphaPlayerNative {
    
    static {
        System.loadLibrary("alphavideo");
    }
    
    /**
     * Listener for player events
     */
    public interface PlayerListener {
        void onLoad(int totalFrames, int width, int height);
        void onFrame(int frameIndex, ByteBuffer yPlane, ByteBuffer uPlane, ByteBuffer vPlane, ByteBuffer aPlane, int width, int height);
        void onPlay();
        void onPause();
        void onStop();
        void onLoop();
        void onEnd();
        void onError(String error);
        void onPreloadComplete(int totalFrames, double timeMs);
    }
    
    // Player lifecycle
    public static native long nativeCreate(
        int decodeThreads,
        int cacheSize,
        boolean preloadAllFrames,
        boolean loopPlayback,
        double playbackSpeed,
        String mode,              // Playback mode: "preload", "stream", or "streamAndPreload"
        int streamCacheSize,      // Cache size when streaming (default: 30)
        int streamPrefetchAhead   // Frames to prefetch ahead (default: 15)
    );
    
    public static native void nativeDestroy(long playerId);
    
    // Callbacks
    public static native void nativeSetListener(long playerId, PlayerListener listener);
    
    // Loading
    public static native boolean nativeLoadFromFile(long playerId, String path);
    /** Bytes [offset, offset + length) of fd; length -1 means to end of file. fd is duplicated. */
    public static native boolean nativeLoadFromFd(long playerId, int fd, long offset, long length);
    /** A {@link WebMDownloader} cache file, read as it downloads. Use with the stream modes. */
    public static native boolean nativeLoadFromDownload(long playerId, String path);
    public static native boolean nativeLoadFromBytes(long playerId, byte[] data);
    public static native void nativeUnload(long playerId);
    
    // Playback control
    public static native void nativePlay(long playerId);
    public static native void nativePause(long playerId);
    public static native void nativeStop(long playerId);
    public static native void nativeSeekToFrame(long playerId, int frameIndex);

    /**
     * Changes playback rate without moving the playhead. Safe mid-playback.
     */
    public static native void nativeSetPlaybackSpeed(long playerId, double speed);
    public static native void nativeSetVerbose(long playerId, boolean verbose);
    public static native void nativeSeekToTime(long playerId, double seconds);
    
    // State queries
    public static native boolean nativeIsLoaded(long playerId);
    public static native boolean nativeIsPlaying(long playerId);
    public static native int nativeGetCurrentFrame(long playerId);
    public static native int nativeGetTotalFrames(long playerId);
    public static native int nativeGetWidth(long playerId);
    public static native int nativeGetHeight(long playerId);
    public static native double nativeGetDuration(long playerId);
    public static native double nativeGetFrameRate(long playerId);
    /** The loaded clip's YUV to RGB for the shader: a column-major mat4, or null if no player. */
    public static native float[] nativeGetYuvToRgb(long playerId);
    /** The decoder for the loaded clip, "libvpx" or "dav1d", or null if no player. */
    public static native String nativeGetDecoderName(long playerId);
    /** The same matrix for a stream's colour range and matrix (YuvToRgb.h), by the core's rules. */
    public static native float[] nativeYuvToRgbFor(int colorRange, int colorSpace, int width, int height);
    
    // Direct frame access
    public static native WebMFrame nativeGetFrame(long playerId, int frameIndex);
    
    // Statistics
    public static native long[] nativeGetStats(long playerId);
    public static native void nativeResetStats(long playerId);
    
    /** The decoder libraries, e.g. "libvpx v1.14.0, dav1d 1.5.1". */
    public static native String nativeGetLibraryVersions();
}
