package com.danielkpl2.webmalphaplayer;

import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Dual-stream VP9 alpha player using Surface-based hardware decoding.
 * 
 * This player keeps all decoded frames on the GPU for maximum performance:
 * - MediaCodec outputs directly to SurfaceTexture (GPU memory)
 * - SurfaceTextures provide OES textures for OpenGL rendering
 * - No CPU involvement after demuxing - all compositing done in shader
 * 
 * Performance: 60fps @ 1080p (vs 7fps with ByteBuffer approach)
 */
public class DualStreamSurfacePlayer implements SurfaceTextureVp9Decoder.FrameAvailableCallback {
    private static final String TAG = "DualStreamSurface";
    
    // Native demuxer handle (reuse existing FFmpeg JNI)
    private long nativeDemuxerPtr = 0;
    
    // Surface-based decoders
    private SurfaceTextureVp9Decoder colorDecoder;
    private SurfaceTextureVp9Decoder alphaDecoder;
    
    // Video info
    private int width;
    private int height;
    private int totalFrames;
    private double frameRate;
    private double duration;
    
    // State
    private final AtomicBoolean isLoaded = new AtomicBoolean(false);
    private final AtomicBoolean isPlaying = new AtomicBoolean(false);
    private final AtomicBoolean shouldStop = new AtomicBoolean(false);
    private final AtomicInteger currentFrameIndex = new AtomicInteger(0);
    
    // Frame synchronization - protected by frameLock for atomic read-modify-write
    // SurfaceTexture only holds ONE frame - must consume before next arrives
    private volatile boolean hasNewColorFrame = false;
    private volatile boolean hasNewAlphaFrame = false;
    private volatile boolean frameConsumed = true; // Can decode next?
    private final Object frameLock = new Object(); // Guards all frame flags
    
    // Playback timing
    private long playbackStartTimeNs = 0;
    private final AtomicLong lastRenderTimeNs = new AtomicLong(0);
    
    // Flow control - only decode one frame pair at a time
    private final Object decodeLock = new Object();
    
    // Debug: track frame sequence
    private volatile int lastColorFrameSet = -1;
    private volatile int lastAlphaFrameSet = -1;
    private volatile int lastFrameConsumed = -1;
    
    // Threading
    private HandlerThread demuxThread;
    private Handler demuxHandler;
    private HandlerThread playbackThread;
    private Handler playbackHandler;
    
    // Configuration
    private boolean loop = true;
    private FrameSyncCallback frameSyncCallback;
    private EventCallback eventCallback;
    
    // Performance tracking
    private long lastPerfLogTime = 0;
    private int framesRendered = 0;
    private int framesDropped = 0;
    private long totalRenderTimeNs = 0;
    
    /**
     * Callback when both color and alpha frames are synced and ready for rendering.
     * Called on playback thread - you should sync to GL thread for updateTexImage().
     */
    public interface FrameSyncCallback {
        /**
         * Called when both color and alpha OES textures have a new frame ready.
         * You must call updateTexImage() on BOTH decoders from GL thread, then render.
         */
        void onFrameReady(int frameIndex, SurfaceTextureVp9Decoder colorDecoder, 
                         SurfaceTextureVp9Decoder alphaDecoder);
    }
    
    /**
     * Lifecycle event callback.
     */
    public interface EventCallback {
        void onLoaded(int totalFrames, int width, int height, double fps, double duration);
        void onError(String error);
        void onLooped();
        void onEnded();
    }
    
    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------
    
    public void setLoop(boolean loop) {
        this.loop = loop;
    }
    
    public void setFrameSyncCallback(FrameSyncCallback callback) {
        this.frameSyncCallback = callback;
    }
    
    public void setEventCallback(EventCallback callback) {
        this.eventCallback = callback;
    }
    
    /**
     * Load a dual-stream WebM file.
     * MUST be called from GL thread so OES textures are created in correct context.
     */
    public boolean loadFromFile(String path) {
        if (isLoaded.get()) {
            unload();
        }
        
        Log.i(TAG, "Loading: " + path);
        
        try {
            // Create native demuxer using existing JNI
            nativeDemuxerPtr = nativeCreateDemuxer(path);
            if (nativeDemuxerPtr == 0) {
                Log.e(TAG, "Failed to create demuxer");
                return false;
            }
            
            // Get video info
            int[] videoInfo = nativeGetVideoInfo(nativeDemuxerPtr);
            if (videoInfo == null || videoInfo.length < 5) {
                Log.e(TAG, "Failed to get video info");
                nativeDestroyDemuxer(nativeDemuxerPtr);
                nativeDemuxerPtr = 0;
                return false;
            }
            
            width = videoInfo[0];
            height = videoInfo[1];
            totalFrames = videoInfo[2];
            int fpsMul1000 = videoInfo[3];
            int numStreams = videoInfo[4];
            
            frameRate = fpsMul1000 / 1000.0;
            duration = totalFrames / frameRate;
            
            Log.i(TAG, "Video: " + width + "x" + height + ", " + totalFrames + " frames, " + 
                  String.format("%.2f", frameRate) + " fps, " + numStreams + " streams");
            
            if (numStreams < 2) {
                Log.e(TAG, "Not a dual-stream file (only " + numStreams + " stream)");
                nativeDestroyDemuxer(nativeDemuxerPtr);
                nativeDemuxerPtr = 0;
                return false;
            }
            
            // Create Surface-based decoders (OES textures created here)
            colorDecoder = new SurfaceTextureVp9Decoder("color");
            alphaDecoder = new SurfaceTextureVp9Decoder("alpha");
            
            if (!colorDecoder.configure(width, height)) {
                Log.e(TAG, "Failed to configure color decoder");
                cleanup();
                return false;
            }
            
            if (!alphaDecoder.configure(width, height)) {
                Log.e(TAG, "Failed to configure alpha decoder");
                cleanup();
                return false;
            }
            
            colorDecoder.setFrameCallback(this);
            alphaDecoder.setFrameCallback(this);
            
            if (!colorDecoder.start()) {
                Log.e(TAG, "Failed to start color decoder");
                cleanup();
                return false;
            }
            
            if (!alphaDecoder.start()) {
                Log.e(TAG, "Failed to start alpha decoder");
                cleanup();
                return false;
            }
            
            // Start demux thread
            demuxThread = new HandlerThread("DualDemux");
            demuxThread.start();
            demuxHandler = new Handler(demuxThread.getLooper());
            
            // Start playback thread
            playbackThread = new HandlerThread("DualPlayback");
            playbackThread.start();
            playbackHandler = new Handler(playbackThread.getLooper());
            
            isLoaded.set(true);
            
            Log.i(TAG, "Loaded successfully: " + totalFrames + " frames @ " + 
                  String.format("%.2f", frameRate) + " fps");
            
            if (eventCallback != null) {
                eventCallback.onLoaded(totalFrames, width, height, frameRate, duration);
            }
            
            return true;
            
        } catch (Exception e) {
            Log.e(TAG, "Load error: " + e.getMessage());
            cleanup();
            return false;
        }
    }
    
    /**
     * Start playback.
     */
    public void play() {
        if (!isLoaded.get() || isPlaying.get()) return;
        
        shouldStop.set(false);
        isPlaying.set(true);
        
        // Reset timing
        playbackStartTimeNs = System.nanoTime();
        lastRenderTimeNs.set(playbackStartTimeNs);
        
        // Reset perf counters
        framesRendered = 0;
        framesDropped = 0;
        lastPerfLogTime = playbackStartTimeNs;
        
        Log.i(TAG, "▶️ Playing - starting demux and render loops");
        
        // Start demuxing
        demuxHandler.post(this::demuxLoop);
        
        // Start render timer
        playbackHandler.post(this::renderTimerLoop);
    }
    
    /**
     * Pause playback.
     */
    public void pause() {
        isPlaying.set(false);
        Log.i(TAG, "Paused");
    }
    
    /**
     * Stop playback.
     */
    public void stop() {
        shouldStop.set(true);
        isPlaying.set(false);
        currentFrameIndex.set(0);
        Log.i(TAG, "Stopped");
    }
    
    /**
     * Unload the video.
     */
    public void unload() {
        Log.i(TAG, "Unloading");
        
        shouldStop.set(true);
        isPlaying.set(false);
        isLoaded.set(false);
        
        cleanup();
        
        Log.i(TAG, "Unloaded");
    }
    
    /**
     * Get the color decoder (for accessing OES texture).
     */
    public SurfaceTextureVp9Decoder getColorDecoder() {
        return colorDecoder;
    }
    
    /**
     * Get the alpha decoder (for accessing OES texture).
     */
    public SurfaceTextureVp9Decoder getAlphaDecoder() {
        return alphaDecoder;
    }
    
    // Getters
    public boolean isLoaded() { return isLoaded.get(); }
    public boolean isPlaying() { return isPlaying.get(); }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getTotalFrames() { return totalFrames; }
    public double getFrameRate() { return frameRate; }
    public double getDuration() { return duration; }
    public int getCurrentFrame() { return currentFrameIndex.get(); }
    
    // -------------------------------------------------------------------------
    // Frame availability callback (from SurfaceTextureVp9Decoder)
    // -------------------------------------------------------------------------
    
    @Override
    public void onFrameAvailable(SurfaceTextureVp9Decoder decoder, int frameIndex, long timestampNs) {
        if (shouldStop.get()) return;
        
        boolean bothReady;
        synchronized (frameLock) {
            if (decoder == colorDecoder) {
                hasNewColorFrame = true;
                lastColorFrameSet = frameIndex;
                Log.d(TAG, "🎨 Color frame " + frameIndex + " available, ts=" + timestampNs + 
                      " (alpha=" + hasNewAlphaFrame + ", consumed=" + frameConsumed + ")");
            } else if (decoder == alphaDecoder) {
                hasNewAlphaFrame = true;
                lastAlphaFrameSet = frameIndex;
                Log.d(TAG, "⬜ Alpha frame " + frameIndex + " available, ts=" + timestampNs + 
                      " (color=" + hasNewColorFrame + ", consumed=" + frameConsumed + ")");
            }
            bothReady = hasNewColorFrame && hasNewAlphaFrame;
        }
        
        // Check if both frames are ready - then we can render
        if (bothReady) {
            Log.d(TAG, "✅ Both frames ready - notifying render");
            synchronized (decodeLock) {
                decodeLock.notifyAll();
            }
        }
    }
    
    @Override
    public void onDecoderError(SurfaceTextureVp9Decoder decoder, String error) {
        Log.e(TAG, "[" + decoder.getLabel() + "] Decoder error: " + error);
        if (eventCallback != null) {
            eventCallback.onError(error);
        }
    }
    
    /**
     * Render the current frame pair if both are ready.
     * Returns true if rendered, false if not ready.
     */
    private boolean tryRenderFrame() {
        if (!isPlaying.get() || frameSyncCallback == null) return false;
        
        int frameIdx;
        boolean canRender;
        
        // Atomically check and consume flags
        synchronized (frameLock) {
            canRender = hasNewColorFrame && hasNewAlphaFrame;
            if (!canRender) {
                return false;
            }
            
            frameIdx = currentFrameIndex.get();
            
            // Render them! (callback invocation is safe outside lock but we do it inside for simplicity)
            Log.d(TAG, "🎬 RENDER frame " + frameIdx + " (colorSet=" + lastColorFrameSet + 
                  ", alphaSet=" + lastAlphaFrameSet + ")");
        }
        
        // Invoke callback (this may take time, so keep it outside the lock if possible)
        frameSyncCallback.onFrameReady(frameIdx, colorDecoder, alphaDecoder);
        framesRendered++;
        
        // Now mark as consumed under lock
        synchronized (frameLock) {
            hasNewColorFrame = false;
            hasNewAlphaFrame = false;
            frameConsumed = true;
            lastFrameConsumed = frameIdx;
        }
        
        // Advance frame counter
        int nextFrame = frameIdx + 1;
        if (nextFrame >= totalFrames) {
            nextFrame = 0;
            Log.i(TAG, "🔄 Frame counter wrapped to 0");
        }
        currentFrameIndex.set(nextFrame);
        lastRenderTimeNs.set(System.nanoTime());
        
        // Wake up demux thread to decode next frame
        synchronized (decodeLock) {
            decodeLock.notifyAll();
        }
        
        return true;
    }
    
    // -------------------------------------------------------------------------
    // Demux loop - decodes one frame pair at a time
    // -------------------------------------------------------------------------
    
    private void demuxLoop() {
        Log.i(TAG, "Demux loop started");
        
        int colorStreamIndex = 0;
        int alphaStreamIndex = 1;
        int frameIndex = 0;
        
        while (!shouldStop.get() && isLoaded.get()) {
            // Wait for previous frame to be consumed before decoding next
            boolean canDecode;
            synchronized (frameLock) {
                canDecode = frameConsumed;
            }
            
            if (!canDecode) {
                synchronized (decodeLock) {
                    try {
                        decodeLock.wait(50); // Wait up to 50ms
                    } catch (InterruptedException e) {
                        break;
                    }
                }
                continue;
            }
            
            // Read color packet
            Object[] colorPacket = nativeReadPacket(nativeDemuxerPtr);
            if (colorPacket == null) {
                // End of file
                Log.i(TAG, "🔚 End of file at frame " + frameIndex);
                
                if (loop) {
                    // Wait for last frame to render with timeout
                    boolean frameWasConsumed = false;
                    int waitCount = 0;
                    final int maxWaitCount = 100; // Max 1 second wait
                    
                    while (!shouldStop.get() && !frameWasConsumed && waitCount < maxWaitCount) {
                        synchronized (frameLock) {
                            frameWasConsumed = frameConsumed;
                        }
                        if (!frameWasConsumed) {
                            try {
                                Thread.sleep(10);
                                waitCount++;
                            } catch (InterruptedException e) {
                                break;
                            }
                        }
                    }
                    
                    if (waitCount >= maxWaitCount) {
                        Log.w(TAG, "🔄 Loop timeout waiting for last frame - forcing reset");
                    }
                    
                    Log.i(TAG, "🔄 Looping - flushing decoders (frameWasConsumed=" + frameWasConsumed + 
                          ", waited=" + (waitCount * 10) + "ms)");
                    
                    // Reset state BEFORE flushing to prevent race with onFrameAvailable
                    synchronized (frameLock) {
                        hasNewColorFrame = false;
                        hasNewAlphaFrame = false;
                        frameConsumed = true;
                        lastColorFrameSet = -1;
                        lastAlphaFrameSet = -1;
                        lastFrameConsumed = -1;
                    }
                    
                    // Flush decoders (this now properly handles state)
                    colorDecoder.flush();
                    alphaDecoder.flush();
                    
                    // Small delay to ensure flush completes
                    try { Thread.sleep(20); } catch (InterruptedException ignored) {}
                    
                    // Seek demuxer to start
                    nativeSeekToStart(nativeDemuxerPtr);
                    
                    frameIndex = 0;
                    currentFrameIndex.set(0);
                    
                    // Reset perf counters
                    framesRendered = 0;
                    framesDropped = 0;
                    lastPerfLogTime = System.nanoTime();
                    
                    Log.i(TAG, "🔄 Looped - restarting decode from frame 0");
                    if (eventCallback != null) {
                        eventCallback.onLooped();
                    }
                    continue;
                } else {
                    if (eventCallback != null) {
                        eventCallback.onEnded();
                    }
                    break;
                }
            }
            
            // Parse color packet
            int colorStream = (Integer) colorPacket[0];
            ByteBuffer colorData = (ByteBuffer) colorPacket[1];
            long colorPts = (Long) colorPacket[3];
            
            // Read alpha packet (should be next)
            Object[] alphaPacket = nativeReadPacket(nativeDemuxerPtr);
            if (alphaPacket == null) {
                Log.w(TAG, "Missing alpha packet for frame " + frameIndex);
                continue;
            }
            
            int alphaStream = (Integer) alphaPacket[0];
            ByteBuffer alphaData = (ByteBuffer) alphaPacket[1];
            long alphaPts = (Long) alphaPacket[3];
            
            // Verify we got the right streams
            if (colorStream != colorStreamIndex || alphaStream != alphaStreamIndex) {
                Log.w(TAG, "Stream mismatch: got " + colorStream + "/" + alphaStream + 
                      ", expected " + colorStreamIndex + "/" + alphaStreamIndex);
                // Swap if needed
                if (colorStream == alphaStreamIndex && alphaStream == colorStreamIndex) {
                    ByteBuffer temp = colorData;
                    colorData = alphaData;
                    alphaData = temp;
                }
            }
            
            // Mark that we're decoding a new frame pair - wait for render to consume
            synchronized (frameLock) {
                frameConsumed = false;
            }
            
            // Queue both packets
            boolean colorQueued = false;
            boolean alphaQueued = false;
            
            while (!colorQueued && !shouldStop.get()) {
                colorQueued = colorDecoder.queueInputPacket(colorData, colorPts, frameIndex);
                if (!colorQueued) {
                    try { Thread.sleep(1); } catch (InterruptedException e) { break; }
                }
            }
            
            while (!alphaQueued && !shouldStop.get()) {
                alphaQueued = alphaDecoder.queueInputPacket(alphaData, alphaPts, frameIndex);
                if (!alphaQueued) {
                    try { Thread.sleep(1); } catch (InterruptedException e) { break; }
                }
            }
            
            Log.d(TAG, "📦 Queued frame pair " + frameIndex + " (pts=" + colorPts + ")");
            frameIndex++;
        }
        
        Log.i(TAG, "Demux loop ended");
    }
    
    // -------------------------------------------------------------------------
    // Render timer loop - controls frame rate
    // -------------------------------------------------------------------------
    
    private void renderTimerLoop() {
        Log.i(TAG, "Render timer started");
        
        long frameDurationNs = (long)(1_000_000_000.0 / frameRate);
        long nextFrameTimeNs = System.nanoTime();
        
        while (!shouldStop.get() && isLoaded.get()) {
            if (!isPlaying.get()) {
                try {
                    Thread.sleep(16);
                } catch (InterruptedException e) {
                    break;
                }
                nextFrameTimeNs = System.nanoTime();
                continue;
            }
            
            long now = System.nanoTime();
            
            if (now >= nextFrameTimeNs) {
                // Time to render the next frame
                boolean rendered = tryRenderFrame();
                
                if (rendered) {
                    nextFrameTimeNs += frameDurationNs;
                } else {
                    // Frame not ready - we're starving, but keep trying
                    framesDropped++;
                    // Small sleep to avoid spinning
                    try { Thread.sleep(1); } catch (InterruptedException e) { break; }
                }
                
                // Don't fall too far behind
                if (nextFrameTimeNs < now - frameDurationNs * 3) {
                    nextFrameTimeNs = now;
                }
            } else {
                // Wait for next frame time or for frames to be ready
                long sleepMs = (nextFrameTimeNs - now) / 1_000_000;
                if (sleepMs > 0) {
                    synchronized (decodeLock) {
                        try {
                            decodeLock.wait(Math.min(sleepMs, 16));
                        } catch (InterruptedException e) {
                            break;
                        }
                    }
                }
            }
            
            // Log performance
            if (now - lastPerfLogTime >= 2_000_000_000L) {
                double elapsed = (now - lastPerfLogTime) / 1_000_000_000.0;
                double fps = framesRendered / elapsed;
                
                boolean colorReady, alphaReady, consumed;
                int colorSet, alphaSet, lastConsumed;
                synchronized (frameLock) {
                    colorReady = hasNewColorFrame;
                    alphaReady = hasNewAlphaFrame;
                    consumed = frameConsumed;
                    colorSet = lastColorFrameSet;
                    alphaSet = lastAlphaFrameSet;
                    lastConsumed = this.lastFrameConsumed;
                }
                
                Log.i(TAG, String.format("⏱ PERF: fps=%.1f (target=%.1f), rendered=%d, dropped=%d",
                    fps, frameRate, framesRendered, framesDropped));
                Log.i(TAG, String.format("⏱ STATE: frame=%d, colorReady=%b(set=%d), alphaReady=%b(set=%d), consumed=%b(last=%d)",
                    currentFrameIndex.get(), colorReady, colorSet, alphaReady, alphaSet, consumed, lastConsumed));
                
                framesRendered = 0;
                framesDropped = 0;
                lastPerfLogTime = now;
            }
        }
        
        Log.i(TAG, "Render timer ended");
    }
    
    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------
    
    private void cleanup() {
        // Stop threads
        if (demuxThread != null) {
            demuxThread.quitSafely();
            demuxThread = null;
            demuxHandler = null;
        }
        
        if (playbackThread != null) {
            playbackThread.quitSafely();
            playbackThread = null;
            playbackHandler = null;
        }
        
        // Release decoders
        if (colorDecoder != null) {
            colorDecoder.release();
            colorDecoder = null;
        }
        
        if (alphaDecoder != null) {
            alphaDecoder.release();
            alphaDecoder = null;
        }
        
        // Release demuxer
        if (nativeDemuxerPtr != 0) {
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
        }
        
        // Reset state
        currentFrameIndex.set(0);
        synchronized (frameLock) {
            hasNewColorFrame = false;
            hasNewAlphaFrame = false;
            frameConsumed = true;
            lastColorFrameSet = -1;
            lastAlphaFrameSet = -1;
            lastFrameConsumed = -1;
        }
    }
    
    // -------------------------------------------------------------------------
    // Native methods (reuse existing JNI from DualStreamMediaCodecPlayer)
    // -------------------------------------------------------------------------
    
    private static native long nativeCreateDemuxer(String path);
    private static native void nativeDestroyDemuxer(long ptr);
    private static native int[] nativeGetVideoInfo(long ptr);
    private static native Object[] nativeReadPacket(long ptr);
    private static native void nativeSeekToStart(long ptr);
    
    static {
        System.loadLibrary("webmalphaplayer");
    }
}
