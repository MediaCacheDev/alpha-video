package com.danielkpl2.webmalphaplayer;

import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Direct OES dual-stream VP9 alpha player.
 * 
 * Architecture (per instructions3.json):
 * - FFmpeg demux → MediaCodec (Surface) → SurfaceTexture → OES texture
 * - Single GL draw call composites both OES textures in fragment shader
 * - NO intermediate FBO ring buffer — SurfaceTexture IS the buffer
 * 
 * Timing model:
 * - MediaCodec + SurfaceTexture control decode cadence and buffering
 * - Playback clock (frame-index based) controls presentation rate
 * - Both SurfaceTextures are drained at frame rate to prevent backpressure
 * 
 * Looping protocol:
 * 1. Stop feeding packets
 * 2. Drain SurfaceTextures via updateTexImage()
 * 3. Flush both MediaCodecs
 * 4. Seek demuxer to frame 0
 * 5. Reset frame index, restart feeding
 */
public class BufferedDualStreamPlayer implements SurfaceTextureVideoDecoder.FrameAvailableCallback {
    private static final String TAG = "DualStreamOES";
    
    // Native demuxer handle
    private long nativeDemuxerPtr = 0;
    
    // Surface-based decoders (OES texture output)
    private SurfaceTextureVideoDecoder colorDecoder;
    private SurfaceTextureVideoDecoder alphaDecoder;
    
    // Video info
    private int width;
    private int height;
    private int totalFrames;
    private double frameRate;

    /**
     * Wall-clock rate multiplier. Volatile: written from the UI thread, read by the playback loop.
     *
     * Scales only how long each frame is shown. The frame index and the PTS handed to the
     * decoders are in the media's own timebase and must not move, or a rate change would seek
     * as well as accelerate.
     */
    private volatile double playbackSpeed = 1.0;
    private double duration;
    
    // State
    private final AtomicBoolean isLoaded = new AtomicBoolean(false);
    private final AtomicBoolean isPlaying = new AtomicBoolean(false);
    private final AtomicBoolean shouldStop = new AtomicBoolean(false);
    private final AtomicInteger playbackFrameIndex = new AtomicInteger(0);
    
    // Frame availability — set by onFrameAvailable, used only for initial sync
    // After initial frames arrive, the playback loop renders unconditionally at frame rate
    private final AtomicBoolean colorDirty = new AtomicBoolean(false);
    private final AtomicBoolean alphaDirty = new AtomicBoolean(false);
    
    // Transform matrices (updated on GL thread by consumeFrames())
    private final float[] colorTransformMatrix = new float[16];
    private final float[] alphaTransformMatrix = new float[16];
    
    // Decode tracking
    private volatile int decodeFrameIndex = 0;
    
    // Threading
    private HandlerThread demuxThread;
    private Handler demuxHandler;
    private HandlerThread playbackThread;
    private Handler playbackHandler;
    
    // GL operations executor (runs tasks on GL thread)
    private GLExecutor glExecutor;
    
    // Configuration
    private boolean loop = true;
    private volatile boolean verbose = false;
    private RenderCallback renderCallback;
    private EventCallback eventCallback;
    
    // Performance tracking
    private long lastPerfLogTime = 0;
    private int framesRendered = 0;
    
    /**
     * Callback to signal the view that a frame is ready for compositing.
     * The view should call consumeFrames() on the GL thread, then composite
     * using the OES textures and transform matrices.
     */
    public interface RenderCallback {
        void onRenderRequested(int frameIndex);
    }
    
    /**
     * Interface for executing tasks on the GL thread.
     */
    public interface GLExecutor {
        void executeOnGLThread(Runnable task);
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
    
    /** Lifecycle and performance chatter, only with verbose. Warnings and errors always log. */
    private void logV(String message) {
        if (verbose) Log.i(TAG, message);
    }
    
    public void setVerbose(boolean verbose) {
        this.verbose = verbose;
        // Propagate to decoders
        SurfaceTextureVideoDecoder cd = colorDecoder;
        SurfaceTextureVideoDecoder ad = alphaDecoder;
        if (cd != null) cd.setVerbose(verbose);
        if (ad != null) ad.setVerbose(verbose);
    }
    
    public void setRenderCallback(RenderCallback callback) {
        this.renderCallback = callback;
    }
    
    public void setEventCallback(EventCallback callback) {
        this.eventCallback = callback;
    }
    
    public void setGLExecutor(GLExecutor executor) {
        this.glExecutor = executor;
    }
    
    /**
     * Load a dual-stream WebM from bytes [offset, offset + length) of fd (length -1: to end of file).
     * The demuxer duplicates fd, so the caller may close it as soon as this returns.
     * MUST be called from GL thread so OES textures are created in correct EGL context.
     */
    public boolean loadFromFd(int fd, long offset, long length) {
        if (isLoaded.get()) {
            unload();
        }
        return load(nativeCreateDemuxer(fd, offset, length));
    }
    
    /**
     * Load a {@link WebMDownloader} cache file, reading it as it downloads.
     * MUST be called from GL thread so OES textures are created in correct EGL context.
     */
    public boolean loadFromDownload(String path) {
        if (isLoaded.get()) {
            unload();
        }
        return load(nativeCreateDemuxerFromDownload(path));
    }
    
    private boolean load(long demuxerPtr) {
        try {
            nativeDemuxerPtr = demuxerPtr;
            if (nativeDemuxerPtr == 0) {
                Log.e(TAG, "Failed to create demuxer");
                return false;
            }
            
            // Get video info: [width, height, totalFrames, frameRate*1000, streamCount, codecId]
            int[] videoInfo = nativeGetVideoInfo(nativeDemuxerPtr);
            if (videoInfo == null || videoInfo.length < 6) {
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
            int codecId = videoInfo[5];
            
            // Map FFmpeg codec_id to Android MIME type
            // AV_CODEC_ID_VP9 = 167, AV_CODEC_ID_AV1 = 226
            String mimeType;
            if (codecId == 226) {
                mimeType = SurfaceTextureVideoDecoder.MIME_AV1;
            } else {
                mimeType = SurfaceTextureVideoDecoder.MIME_VP9;
            }
            
            frameRate = fpsMul1000 / 1000.0;
            duration = totalFrames / frameRate;
            
            logV("Video: " + width + "x" + height + ", " + totalFrames + " frames, " + 
                  String.format("%.2f", frameRate) + " fps, " + numStreams + " streams, codec=" + mimeType);
            
            if (numStreams < 2) {
                Log.e(TAG, "Not a dual-stream file");
                nativeDestroyDemuxer(nativeDemuxerPtr);
                nativeDemuxerPtr = 0;
                return false;
            }
            
            // Create decoders with detected MIME type — OES textures are created here (must be on GL thread)
            colorDecoder = new SurfaceTextureVideoDecoder("color", mimeType);
            alphaDecoder = new SurfaceTextureVideoDecoder("alpha", mimeType);
            colorDecoder.setVerbose(verbose);
            alphaDecoder.setVerbose(verbose);
            
            if (!colorDecoder.configure(width, height) || !alphaDecoder.configure(width, height)) {
                Log.e(TAG, "Failed to configure decoders");
                cleanup();
                return false;
            }
            
            // onFrameAvailable just sets dirty flags — no FBO copy needed
            colorDecoder.setFrameCallback(this);
            alphaDecoder.setFrameCallback(this);
            
            if (!colorDecoder.start() || !alphaDecoder.start()) {
                Log.e(TAG, "Failed to start decoders");
                cleanup();
                return false;
            }
            
            // Start threads
            demuxThread = new HandlerThread("DemuxThread");
            demuxThread.start();
            demuxHandler = new Handler(demuxThread.getLooper());
            
            playbackThread = new HandlerThread("PlaybackThread");
            playbackThread.start();
            playbackHandler = new Handler(playbackThread.getLooper());
            
            isLoaded.set(true);
            
            logV("Loaded successfully (direct OES pipeline)");
            
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
        
        // Reset counters
        framesRendered = 0;
        lastPerfLogTime = System.nanoTime();
        playbackFrameIndex.set(0);
        
        logV("Starting playback");
        
        // Start demux loop (feeds packets to decoders)
        demuxHandler.post(this::demuxLoop);
        
        // Start playback loop (triggers rendering at frame rate)
        playbackHandler.post(this::playbackLoop);
    }
    
    /**
     * Pause playback.
     */
    public void pause() {
        isPlaying.set(false);
        logV("Paused");
    }
    
    /**
     * Stop playback.
     */
    public void stop() {
        shouldStop.set(true);
        isPlaying.set(false);
        playbackFrameIndex.set(0);
        colorDirty.set(false);
        alphaDirty.set(false);
        logV("Stopped");
    }
    
    /**
     * Unload the video.
     */
    public void unload() {
        logV("Unloading");
        shouldStop.set(true);
        isPlaying.set(false);
        isLoaded.set(false);
        cleanup();
    }
    
    /**
     * Changes how fast frames are presented, leaving the playhead where it is.
     *
     * Takes effect on the next frame, so this is safe to call while playing — which is the
     * intended use: a button that speeds its animation up on hover must not also jump.
     */
    public void setPlaybackSpeed(double speed) {
        playbackSpeed = speed > 0.01 ? speed : 0.01;
    }

    // Getters
    public boolean isLoaded() { return isLoaded.get(); }
    public double getPlaybackSpeed() { return playbackSpeed; }
    public boolean isPlaying() { return isPlaying.get(); }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getTotalFrames() { return totalFrames; }
    public double getFrameRate() { return frameRate; }
    public double getDuration() { return duration; }
    public int getCurrentFrame() { return playbackFrameIndex.get(); }
    
    public SurfaceTextureVideoDecoder getColorDecoder() { return colorDecoder; }
    public SurfaceTextureVideoDecoder getAlphaDecoder() { return alphaDecoder; }
    
    /**
     * Consume pending frames from both SurfaceTextures.
     * MUST be called on the GL thread.
     * 
     * Calls updateTexImage() on both decoders to consume any frame that the
     * PTS-gated pollOutput() has released. After consuming, clears the release
     * flags so the decoder knows it can release the next frame on the next cycle.
     * 
     * @return true if decoders are valid and textures were updated
     */
    public boolean consumeFrames() {
        SurfaceTextureVideoDecoder cd = colorDecoder;
        SurfaceTextureVideoDecoder ad = alphaDecoder;
        if (cd == null || ad == null) return false;
        
        // Only consume when BOTH decoders have released a frame.
        // This guarantees color and alpha are from the same temporal position.
        // If one decoder is slightly slower, we wait until next render cycle.
        boolean colorReady = cd.wasFrameReleased();
        boolean alphaReady = ad.wasFrameReleased();
        
        if (!colorReady || !alphaReady) {
            // One or both decoders haven't released yet for this cycle.
            // Still call updateTexImage() to avoid blocking the SurfaceTexture
            // pipeline, but don't clear flags — let the slow decoder catch up.
            // Return false to indicate no new synchronized frame pair.
            return false;
        }
        
        // Both decoders have released exactly one frame — consume them
        cd.updateTexImage();
        ad.updateTexImage();
        
        // Clear release flags so the decoder output loop can release
        // the next frame on the next playback cycle
        cd.clearFrameReleased();
        ad.clearFrameReleased();
        
        // Get transform matrices for the vertex shader
        cd.getTransformMatrix(colorTransformMatrix);
        ad.getTransformMatrix(alphaTransformMatrix);
        
        return true;
    }
    
    /**
     * Get color OES texture ID for use in compositor shader.
     */
    public int getColorOesTextureId() {
        SurfaceTextureVideoDecoder cd = colorDecoder;
        return cd != null ? cd.getOesTextureId() : -1;
    }
    
    /**
     * Get alpha OES texture ID for use in compositor shader.
     */
    public int getAlphaOesTextureId() {
        SurfaceTextureVideoDecoder ad = alphaDecoder;
        return ad != null ? ad.getOesTextureId() : -1;
    }
    
    /**
     * Get color transform matrix (valid after consumeFrames()).
     */
    public float[] getColorTransformMatrix() {
        return colorTransformMatrix;
    }
    
    /**
     * Get alpha transform matrix (valid after consumeFrames()).
     */
    public float[] getAlphaTransformMatrix() {
        return alphaTransformMatrix;
    }
    
    // -------------------------------------------------------------------------
    // Frame availability callback (from SurfaceTextureVideoDecoder)
    // -------------------------------------------------------------------------
    
    /**
     * Called when a decoder has a new frame available on its SurfaceTexture.
     * 
     * Per the architecture spec, this ONLY sets a dirty flag and captures metadata.
     * The actual updateTexImage() happens on the GL render thread via consumeFrames().
     */
    @Override
    public void onFrameAvailable(SurfaceTextureVideoDecoder decoder, int frameIndex, long timestampNs) {
        if (shouldStop.get()) return;
        
        if (decoder == colorDecoder) {
            colorDirty.set(true);
        } else if (decoder == alphaDecoder) {
            alphaDirty.set(true);
        }
    }
    
    @Override
    public void onDecoderError(SurfaceTextureVideoDecoder decoder, String error) {
        Log.e(TAG, "[" + decoder.getLabel() + "] Error: " + error);
        if (eventCallback != null) {
            eventCallback.onError(error);
        }
    }
    
    // -------------------------------------------------------------------------
    // Demux loop — feeds compressed packets to MediaCodec decoders
    // -------------------------------------------------------------------------
    
    private int loopCount = 0;
    
    private void demuxLoop() {
        logV("Demux loop started");
        
        decodeFrameIndex = 0;
        loopCount = 0;
        
        while (!shouldStop.get() && isLoaded.get()) {
            if (nativeDemuxerPtr == 0 || colorDecoder == null || alphaDecoder == null) {
                Log.w(TAG, "Demux loop: invalid state, exiting");
                break;
            }
            
            // Read two packets (one color, one alpha)
            Object[] packet1 = nativeReadPacket(nativeDemuxerPtr);
            if (packet1 == null) {
                // End of file
                logV("End of file at frame " + decodeFrameIndex);
                
                if (loop) {
                    loopCount++;
                    logV("Loop #" + loopCount + " - handling loop transition");
                    
                    // Detect stuck loop
                    if (decodeFrameIndex == 0) {
                        Log.e(TAG, "Stuck loop detected: EOF at frame 0");
                        if (loopCount > 5) {
                            Log.e(TAG, "Too many stuck loops, stopping");
                            if (eventCallback != null) {
                                eventCallback.onError("Demuxer seek failed - cannot loop");
                            }
                            break;
                        }
                        try { Thread.sleep(100); } catch (InterruptedException e) { break; }
                    }
                    
                    handleLoop();
                    continue;
                } else {
                    if (eventCallback != null) {
                        eventCallback.onEnded();
                    }
                    break;
                }
            }
            
            Object[] packet2 = nativeReadPacket(nativeDemuxerPtr);
            if (packet2 == null) {
                Log.w(TAG, "Missing second packet for frame " + decodeFrameIndex);
                continue;
            }
            
            // Sort into color (stream 0) and alpha (stream 1)
            int stream1 = (Integer) packet1[0];
            int stream2 = (Integer) packet2[0];
            
            Object[] colorPacket;
            Object[] alphaPacket;
            
            if (stream1 == 0 && stream2 == 1) {
                colorPacket = packet1;
                alphaPacket = packet2;
            } else if (stream1 == 1 && stream2 == 0) {
                colorPacket = packet2;
                alphaPacket = packet1;
            } else {
                Log.e(TAG, "Stream mismatch: got " + stream1 + "," + stream2 + 
                      " for frame " + decodeFrameIndex);
                continue;
            }
            
            ByteBuffer colorData = (ByteBuffer) colorPacket[1];
            long colorPts = (Long) colorPacket[3];
            ByteBuffer alphaData = (ByteBuffer) alphaPacket[1];
            long alphaPts = (Long) alphaPacket[3];
            
            if (shouldStop.get()) break;
            
            SurfaceTextureVideoDecoder cd = colorDecoder;
            SurfaceTextureVideoDecoder ad = alphaDecoder;
            if (cd == null || ad == null) break;
            
            // Queue to color decoder — retry until accepted or stopped
            // MediaCodec backpressure naturally throttles when SurfaceTexture output is full
            while (!cd.queueInputPacket(colorData, colorPts, decodeFrameIndex) && !shouldStop.get()) {
                try { Thread.sleep(1); } catch (InterruptedException e) { break; }
            }
            
            // Queue to alpha decoder
            while (!ad.queueInputPacket(alphaData, alphaPts, decodeFrameIndex) && !shouldStop.get()) {
                try { Thread.sleep(1); } catch (InterruptedException e) { break; }
            }
            
            decodeFrameIndex++;
        }
        
        logV("Demux loop ended");
    }
    
    /**
     * Handle loop transition per spec:
     * 1. Drain SurfaceTextures via updateTexImage()
     * 2. Flush both MediaCodecs
     * 3. Seek demuxer to frame 0
     * 4. Reset state and restart
     */
    private void handleLoop() {
        logV("Loop transition starting...");
        
        if (shouldStop.get()) return;
        
        // Step 1: Drain SurfaceTextures to prevent stale frames
        // Post to GL thread and wait for completion
        if (glExecutor != null) {
            final Object drainLock = new Object();
            final AtomicBoolean drained = new AtomicBoolean(false);
            
            glExecutor.executeOnGLThread(() -> {
                SurfaceTextureVideoDecoder cd = colorDecoder;
                SurfaceTextureVideoDecoder ad = alphaDecoder;
                if (cd != null && cd.hasNewFrame()) {
                    cd.updateTexImage();
                }
                if (ad != null && ad.hasNewFrame()) {
                    ad.updateTexImage();
                }
                synchronized (drainLock) {
                    drained.set(true);
                    drainLock.notifyAll();
                }
            });
            
            // Wait for drain to complete (with timeout)
            synchronized (drainLock) {
                long deadline = System.currentTimeMillis() + 500;
                while (!drained.get() && System.currentTimeMillis() < deadline) {
                    try { drainLock.wait(50); } catch (InterruptedException e) { break; }
                }
            }
        }
        
        // Step 2: Flush both decoders (resets VP9 reference frames and timestamps)
        logV("Flushing decoders...");
        if (colorDecoder != null) colorDecoder.flush();
        if (alphaDecoder != null) alphaDecoder.flush();
        
        // Step 3: Seek demuxer to start
        logV("Seeking demuxer to start...");
        if (nativeDemuxerPtr != 0) {
            nativeSeekToStart(nativeDemuxerPtr);
        }
        
        // Step 4: Reset state
        colorDirty.set(false);
        alphaDirty.set(false);
        decodeFrameIndex = 0;
        playbackFrameIndex.set(0);
        
        // Reset perf counters
        framesRendered = 0;
        lastPerfLogTime = System.nanoTime();
        
        if (eventCallback != null) {
            eventCallback.onLooped();
        }
        
        logV("Loop transition complete - resuming from frame 0");
    }
    
    // -------------------------------------------------------------------------
    // Playback loop — triggers rendering at frame rate
    // -------------------------------------------------------------------------
    
    private void playbackLoop() {
        logV("Playback loop started");
        
        // Media timebase — one frame's worth of presentation time, whatever the playback rate.
        long frameDurationUs = (long)(1_000_000.0 / frameRate);
        long nextFrameTimeNs = System.nanoTime();
        
        // Set initial target PTS to 0 so the very first frame (pts=0) can be released
        SurfaceTextureVideoDecoder cd = colorDecoder;
        SurfaceTextureVideoDecoder ad = alphaDecoder;
        if (cd != null) cd.setTargetPresentationTimeUs(0);
        if (ad != null) ad.setTargetPresentationTimeUs(0);
        
        // Wait for first decoded frame from each decoder before starting the clock.
        // We use dirty flags only for this initial sync.
        logV("Waiting for initial frames...");
        int waitCount = 0;
        while (!shouldStop.get() && waitCount < 200) {
            if (colorDirty.get() && alphaDirty.get()) {
                break;
            }
            waitCount++;
            try { Thread.sleep(10); } catch (InterruptedException e) { break; }
        }
        
        if (shouldStop.get()) {
            logV("Playback loop aborted during initial wait");
            return;
        }
        
        logV("Initial frames ready after " + (waitCount * 10) + "ms");
        nextFrameTimeNs = System.nanoTime();
        
        while (!shouldStop.get() && isLoaded.get()) {
            if (!isPlaying.get()) {
                try { Thread.sleep(16); } catch (InterruptedException e) { break; }
                nextFrameTimeNs = System.nanoTime();
                continue;
            }
            
            // Read per iteration so a mid-playback rate change takes effect on the next frame
            // rather than at the next load.
            double speed = playbackSpeed;
            if (speed <= 0.01) speed = 0.01;
            long frameDurationNs = (long)(1_000_000_000.0 / (frameRate * speed));
            
            long now = System.nanoTime();
            
            if (now >= nextFrameTimeNs) {
                int frameIdx = playbackFrameIndex.get();
                
                // Advance the target PTS on BOTH decoders so they release exactly
                // this frame's data to their SurfaceTextures. Both decoders get the
                // same target, ensuring color and alpha stay synchronized.
                long targetPtsUs = frameIdx * frameDurationUs;
                cd = colorDecoder;
                ad = alphaDecoder;
                if (cd != null) cd.setTargetPresentationTimeUs(targetPtsUs);
                if (ad != null) ad.setTargetPresentationTimeUs(targetPtsUs);
                
                // Request render — the view will call consumeFrames() which calls
                // updateTexImage() to grab the frame we just allowed to be released
                if (renderCallback != null) {
                    renderCallback.onRenderRequested(frameIdx);
                }
                
                playbackFrameIndex.incrementAndGet();
                framesRendered++;
                nextFrameTimeNs += frameDurationNs;
                
                // Don't fall too far behind
                if (nextFrameTimeNs < now - frameDurationNs * 3) {
                    nextFrameTimeNs = now;
                }
            } else {
                // Wait for next frame time
                long sleepMs = (nextFrameTimeNs - now) / 1_000_000;
                if (sleepMs > 0) {
                    try { Thread.sleep(Math.min(sleepMs, 16)); } catch (InterruptedException e) { break; }
                }
            }
            
            // Performance logging every 2 seconds (verbose only)
            if (verbose && now - lastPerfLogTime >= 2_000_000_000L) {
                double elapsed = (now - lastPerfLogTime) / 1_000_000_000.0;
                double fps = framesRendered / elapsed;
                
                logV(String.format("PERF: fps=%.1f (target=%.1f), rendered=%d, playbackFrame=%d",
                    fps, frameRate, framesRendered, playbackFrameIndex.get()));
                
                framesRendered = 0;
                lastPerfLogTime = now;
            }
        }
        
        logV("Playback loop ended");
    }
    
    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------
    
    private void cleanup() {
        logV("Starting cleanup...");
        
        shouldStop.set(true);
        
        // Stop demux thread
        if (demuxThread != null) {
            demuxThread.quitSafely();
            try { demuxThread.join(500); } catch (InterruptedException e) {}
            demuxThread = null;
            demuxHandler = null;
        }
        
        // Stop playback thread
        if (playbackThread != null) {
            playbackThread.quitSafely();
            try { playbackThread.join(500); } catch (InterruptedException e) {}
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
        
        // Destroy native demuxer
        if (nativeDemuxerPtr != 0) {
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
        }
        
        playbackFrameIndex.set(0);
        colorDirty.set(false);
        alphaDirty.set(false);
        
        logV("Cleanup complete");
    }
    
    // -------------------------------------------------------------------------
    // Native methods
    // -------------------------------------------------------------------------
    
    private static native long nativeCreateDemuxer(int fd, long offset, long length);
    private static native long nativeCreateDemuxerFromDownload(String path);
    private static native void nativeDestroyDemuxer(long ptr);
    private static native int[] nativeGetVideoInfo(long ptr);
    private static native Object[] nativeReadPacket(long ptr);
    private static native void nativeSeekToStart(long ptr);
    
    /**
     * Check if bytes [offset, offset + length) of fd hold dual video streams (VP9 or AV1, color + alpha).
     */
    public static native boolean nativeIsDualStream(int fd, long offset, long length);
    
    /** The same for a {@link WebMDownloader} cache file; waits for its first bytes if still downloading. */
    public static native boolean nativeIsDualStreamDownload(String path);
    
    static {
        System.loadLibrary("webmalphaplayer");
    }
}
