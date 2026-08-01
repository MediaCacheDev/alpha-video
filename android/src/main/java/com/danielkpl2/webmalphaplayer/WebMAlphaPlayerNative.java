package com.danielkpl2.webmalphaplayer;

import java.nio.ByteBuffer;

/**
 * JNI bridge to C++ WebMAlphaPlayer core
 * Provides high-performance VP9+Alpha WebM playback
 */
public class WebMAlphaPlayerNative {
    
    static {
        System.loadLibrary("webmalphaplayer");
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
    public static native boolean nativeLoadFromBytes(long playerId, byte[] data);
    public static native boolean nativeLoadFromUrl(long playerId, String url);
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
    
    // Direct frame access
    public static native WebMFrame nativeGetFrame(long playerId, int frameIndex);
    
    // Statistics
    public static native long[] nativeGetStats(long playerId);
    public static native void nativeResetStats(long playerId);
    
    // FFmpeg info
    public static native String nativeGetFFmpegVersion();
}
