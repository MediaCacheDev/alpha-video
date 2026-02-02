package com.danielkpl2.webmalphaplayer;

import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Buffered dual-stream VP9 alpha player with FBO ring buffer.
 * 
 * Key architecture:
 * - MediaCodec outputs to SurfaceTexture (GPU-accelerated)
 * - Immediately copies OES textures to ring buffer (solves single-frame limit)
 * - Render reads from ring buffer at frame rate (decoupled from decode)
 * - Supports 15+ frame buffering for smooth playback
 * 
 * This fixes the SurfaceTexture limitation where only ONE frame can be pending.
 */
public class BufferedDualStreamPlayer implements SurfaceTextureVp9Decoder.FrameAvailableCallback {
    private static final String TAG = "BufferedDualStream";
    
    // Default buffer size (frames ahead of playback)
    private static final int DEFAULT_BUFFER_SIZE = 15;
    
    // Native demuxer handle
    private long nativeDemuxerPtr = 0;
    
    // Surface-based decoders
    private SurfaceTextureVp9Decoder colorDecoder;
    private SurfaceTextureVp9Decoder alphaDecoder;
    
    // Frame ring buffer (color+alpha pairs)
    private FrameRingBuffer ringBuffer;
    
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
    
    // Loop transition state - used to detect and skip stale frames after loop reset
    private final AtomicBoolean inLoopTransition = new AtomicBoolean(false);
    
    // Frame synchronization - track by actual frame index
    private final java.util.Set<Integer> pendingColorFrames = new java.util.HashSet<>();
    private final java.util.Set<Integer> pendingAlphaFrames = new java.util.HashSet<>();
    private final Object frameLock = new Object();
    
    // Decode tracking
    private volatile int decodeFrameIndex = 0;
    private volatile int lastCopiedFrameIndex = -1;
    
    // Transform matrices from SurfaceTextures
    private final float[] colorMatrix = new float[16];
    private final float[] alphaMatrix = new float[16];
    
    // Threading
    private HandlerThread demuxThread;
    private Handler demuxHandler;
    private HandlerThread playbackThread;
    private Handler playbackHandler;
    
    // GL operations executor (runs tasks on GL thread)
    private GLExecutor glExecutor;
    
    // Configuration
    private boolean loop = true;
    private int bufferSize = DEFAULT_BUFFER_SIZE;
    private RenderCallback renderCallback;
    private EventCallback eventCallback;
    
    // Performance tracking
    private long lastPerfLogTime = 0;
    private int framesRendered = 0;
    private int framesDropped = 0;
    private int framesBuffered = 0;
    
    /**
     * Callback for rendering frames from the buffer.
     */
    public interface RenderCallback {
        /**
         * Called when a frame pair is ready to render.
         * The textures are regular 2D textures (not OES).
         */
        void onFrameReady(int frameIndex, int colorTextureId, int alphaTextureId);
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
        void onBufferState(int bufferedFrames, int bufferCapacity);
    }
    
    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------
    
    public void setLoop(boolean loop) {
        this.loop = loop;
    }
    
    public void setBufferSize(int size) {
        this.bufferSize = Math.max(5, Math.min(30, size));
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
     * Load a dual-stream WebM file.
     * MUST be called from GL thread so OES textures are created in correct context.
     */
    public boolean loadFromFile(String path) {
        if (isLoaded.get()) {
            unload();
        }
        
        Log.i(TAG, "Loading: " + path + " (buffer=" + bufferSize + " frames)");
        
        try {
            // Create native demuxer
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
                Log.e(TAG, "Not a dual-stream file");
                nativeDestroyDemuxer(nativeDemuxerPtr);
                nativeDemuxerPtr = 0;
                return false;
            }
            
            // Create decoders
            colorDecoder = new SurfaceTextureVp9Decoder("color");
            alphaDecoder = new SurfaceTextureVp9Decoder("alpha");
            
            if (!colorDecoder.configure(width, height) || !alphaDecoder.configure(width, height)) {
                Log.e(TAG, "Failed to configure decoders");
                cleanup();
                return false;
            }
            
            colorDecoder.setFrameCallback(this);
            alphaDecoder.setFrameCallback(this);
            
            if (!colorDecoder.start() || !alphaDecoder.start()) {
                Log.e(TAG, "Failed to start decoders");
                cleanup();
                return false;
            }
            
            // Create ring buffer
            ringBuffer = new FrameRingBuffer(bufferSize, width, height);
            if (!ringBuffer.initGL()) {
                Log.e(TAG, "Failed to initialize ring buffer GL resources");
                cleanup();
                return false;
            }
            
            // Start threads
            demuxThread = new HandlerThread("BufferedDemux");
            demuxThread.start();
            demuxHandler = new Handler(demuxThread.getLooper());
            
            playbackThread = new HandlerThread("BufferedPlayback");
            playbackThread.start();
            playbackHandler = new Handler(playbackThread.getLooper());
            
            isLoaded.set(true);
            
            Log.i(TAG, "Loaded successfully with " + bufferSize + "-frame buffer");
            
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
        inLoopTransition.set(false);  // Ensure clean state at start
        
        // Reset counters
        framesRendered = 0;
        framesDropped = 0;
        framesBuffered = 0;
        lastPerfLogTime = System.nanoTime();
        
        Log.i(TAG, "▶️ Starting playback");
        
        // Start demux loop (fills buffer)
        demuxHandler.post(this::demuxLoop);
        
        // Start render loop (consumes buffer at frame rate)
        playbackHandler.post(this::renderLoop);
    }
    
    /**
     * Pause playback.
     */
    public void pause() {
        isPlaying.set(false);
        Log.i(TAG, "⏸️ Paused");
    }
    
    /**
     * Stop playback.
     */
    public void stop() {
        shouldStop.set(true);
        isPlaying.set(false);
        currentFrameIndex.set(0);
        inLoopTransition.set(false);  // Clear loop transition state
        
        // Wake up any waiting threads
        synchronized (frameLock) {
            frameLock.notifyAll();
        }
        
        FrameRingBuffer rb = ringBuffer;
        if (rb != null) {
            rb.clear();
        }
        Log.i(TAG, "⏹️ Stopped");
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
    public int getBufferedFrames() {
        FrameRingBuffer rb = ringBuffer;
        return rb != null ? rb.getBufferedCount() : 0;
    }
    
    public SurfaceTextureVp9Decoder getColorDecoder() { return colorDecoder; }
    public SurfaceTextureVp9Decoder getAlphaDecoder() { return alphaDecoder; }
    
    // -------------------------------------------------------------------------
    // Frame availability callback (from SurfaceTextureVp9Decoder)
    // -------------------------------------------------------------------------
    
    /**
     * Called when a decoder has a new frame ready.
     * 
     * CRITICAL ARCHITECTURE CHANGE:
     * Each decoder's frame is processed INDEPENDENTLY to prevent deadlock.
     * We immediately call updateTexImage() and copy to the ring buffer's
     * reserved slot, without waiting for the other decoder.
     * 
     * The ring buffer slot becomes "valid" only when BOTH color and alpha
     * have been written, but each decoder can proceed at its own pace.
     * 
     * FRAME INDEX CALCULATION:
     * We calculate frame index from the timestamp rather than relying on the decoder's
     * reported frame index. This is because the two decoders may output frames at 
     * different rates, causing their FIFO queues to diverge. Using timestamp ensures
     * that color and alpha frames with the same PTS are paired correctly.
     */
    @Override
    public void onFrameAvailable(SurfaceTextureVp9Decoder decoder, int decoderFrameIndex, long timestampNs) {
        if (shouldStop.get()) return;
        
        final boolean isColor = (decoder == colorDecoder);
        final long callbackTimeNs = System.nanoTime();
        
        // Calculate frame index from timestamp - this is the authoritative frame index
        // Both color and alpha streams have the same PTS for corresponding frames,
        // so this ensures they get paired correctly even if decoders run at different speeds
        final long frameDurationNs = (frameRate > 0) ? (long)(1_000_000_000.0 / frameRate) : 33333333L;
        final int frameIndex = (int)Math.round((double)timestampNs / frameDurationNs);
        
        // STALE FRAME DETECTION:
        // After a loop transition, stale frames from the previous loop may still arrive
        // (they were released to SurfaceTexture before flush). Detect and skip them.
        // A frame is stale if we're in loop transition AND it has a high index (from end of video)
        final boolean isStaleFrame;
        if (inLoopTransition.get()) {
            // During loop transition, frames from the tail end of video are stale
            // We expect frames near 0, so anything > totalFrames/2 is definitely stale
            if (totalFrames > 0 && frameIndex > totalFrames / 2) {
                isStaleFrame = true;
                Log.w(TAG, "🗑️ STALE FRAME detected (loop transition): " + (isColor ? "COLOR" : "ALPHA") + 
                      " frameIndex=" + frameIndex + " (expected near 0, totalFrames=" + totalFrames + ")");
            } else {
                // Fresh frame arrived - we can clear the loop transition state
                // Wait until we see a few low-index frames to be sure
                if (frameIndex < bufferSize) {
                    inLoopTransition.set(false);
                    Log.i(TAG, "✅ Loop transition complete - fresh frame " + frameIndex + " arrived");
                }
                isStaleFrame = false;
            }
        } else {
            isStaleFrame = false;
        }
        
        // Log sync diagnostic info - show both decoder's reported index and calculated index
        Log.d(TAG, "📨 onFrameAvailable: " + (isColor ? "COLOR" : "ALPHA") + 
              " decoderIdx=" + decoderFrameIndex + " calcIdx=" + frameIndex + " ts=" + timestampNs +
              (isStaleFrame ? " [STALE-SKIP]" : ""));
        
        // IMMEDIATELY process this frame on the GL thread
        // Don't wait for the other decoder - this prevents SurfaceTexture backpressure
        // MUST always consume from SurfaceTexture even for stale frames (prevents backpressure)
        if (glExecutor != null) {
            glExecutor.executeOnGLThread(() -> {
                long processTimeNs = System.nanoTime();
                long delayMs = (processTimeNs - callbackTimeNs) / 1_000_000;
                
                if (delayMs > 5) {
                    Log.w(TAG, "⚠️ GL thread delay: " + delayMs + "ms for " + 
                          (isColor ? "COLOR" : "ALPHA") + " frame " + frameIndex);
                }
                
                if (isStaleFrame) {
                    // MUST consume the SurfaceTexture frame to prevent backpressure,
                    // but don't write to buffer
                    consumeStaleFrame(decoder, isColor, frameIndex);
                } else {
                    if (isColor) {
                        copyColorToBuffer(frameIndex);
                    } else {
                        copyAlphaToBuffer(frameIndex);
                    }
                }
            });
        }
    }
    
    /**
     * Consume a stale frame from SurfaceTexture without writing to buffer.
     * This is critical to prevent SurfaceTexture backpressure.
     */
    private void consumeStaleFrame(SurfaceTextureVp9Decoder decoder, boolean isColor, int frameIndex) {
        // Always update the texture to consume the frame from SurfaceTexture
        decoder.updateTexImage();
        Log.d(TAG, "🗑️ Consumed stale " + (isColor ? "COLOR" : "ALPHA") + " frame " + frameIndex);
    }
    
    @Override
    public void onDecoderError(SurfaceTextureVp9Decoder decoder, String error) {
        Log.e(TAG, "[" + decoder.getLabel() + "] Error: " + error);
        if (eventCallback != null) {
            eventCallback.onError(error);
        }
    }
    
    /**
     * Copy color frame to ring buffer.
     * Called on GL thread immediately when color decoder has a frame.
     */
    private void copyColorToBuffer(int frameIndex) {
        if (shouldStop.get()) {
            clearPendingColor(frameIndex);
            return;
        }
        
        SurfaceTextureVp9Decoder cd = colorDecoder;
        FrameRingBuffer rb = ringBuffer;
        if (cd == null || rb == null) {
            clearPendingColor(frameIndex);
            return;
        }
        
        // IMMEDIATELY update SurfaceTexture - critical to prevent backpressure
        cd.updateTexImage();
        cd.getTransformMatrix(colorMatrix);
        
        // Write color to the reserved slot
        boolean completed = rb.writeColor(cd.getOesTextureId(), colorMatrix, frameIndex);
        
        if (completed) {
            framesBuffered++;
            lastCopiedFrameIndex = frameIndex;
        }
        
        // Clear pending and notify
        clearPendingColor(frameIndex);
    }
    
    /**
     * Copy alpha frame to ring buffer.
     * Called on GL thread immediately when alpha decoder has a frame.
     */
    private void copyAlphaToBuffer(int frameIndex) {
        if (shouldStop.get()) {
            clearPendingAlpha(frameIndex);
            return;
        }
        
        SurfaceTextureVp9Decoder ad = alphaDecoder;
        FrameRingBuffer rb = ringBuffer;
        if (ad == null || rb == null) {
            clearPendingAlpha(frameIndex);
            return;
        }
        
        // IMMEDIATELY update SurfaceTexture - critical to prevent backpressure
        ad.updateTexImage();
        ad.getTransformMatrix(alphaMatrix);
        
        // Write alpha to the reserved slot
        boolean completed = rb.writeAlpha(ad.getOesTextureId(), alphaMatrix, frameIndex);
        
        if (completed) {
            // If this was the completing write, count it
            // (only one of color/alpha will report completed=true)
            framesBuffered++;
            lastCopiedFrameIndex = frameIndex;
        }
        
        // Clear pending and notify
        clearPendingAlpha(frameIndex);
    }
    
    private void clearPendingColor(int frameIndex) {
        synchronized (frameLock) {
            pendingColorFrames.remove(frameIndex);
            frameLock.notifyAll();
        }
    }
    
    private void clearPendingAlpha(int frameIndex) {
        synchronized (frameLock) {
            pendingAlphaFrames.remove(frameIndex);
            frameLock.notifyAll();
        }
    }
    
    // -------------------------------------------------------------------------
    // Demux loop - fills buffer ahead of playback
    // -------------------------------------------------------------------------
    
    // Track loop iterations to detect stuck loops
    private int loopCount = 0;
    private long lastLoopTime = 0;
    
    private void demuxLoop() {
        Log.i(TAG, "Demux loop started");
        
        decodeFrameIndex = 0;
        loopCount = 0;
        
        while (!shouldStop.get() && isLoaded.get()) {
            // Wait for buffer space - now we use per-decoder pending tracking
            // We can have ONE frame in flight per decoder (SurfaceTexture constraint)
            synchronized (frameLock) {
                while (!shouldStop.get()) {
                    FrameRingBuffer rb = ringBuffer;
                    boolean hasSpace = rb != null && rb.hasSpace();
                    
                    // Check per-decoder pending - allow one pending per decoder max
                    // This prevents SurfaceTexture overflow while allowing parallelism
                    boolean colorCanAccept = pendingColorFrames.size() < 2;  // Allow 1-2 in flight
                    boolean alphaCanAccept = pendingAlphaFrames.size() < 2;
                    
                    if (hasSpace && colorCanAccept && alphaCanAccept) {
                        break;  // Ready to demux next frame
                    }
                    
                    try {
                        frameLock.wait(5);  // Short wait for responsive flow control
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            
            if (shouldStop.get()) break;
            
            // Check that we still have valid state
            FrameRingBuffer rb = ringBuffer;
            if (nativeDemuxerPtr == 0 || colorDecoder == null || alphaDecoder == null || rb == null) {
                Log.w(TAG, "Demux loop: invalid state, exiting");
                break;
            }
            
            // Reserve a slot in the ring buffer BEFORE queueing to decoders
            // This ensures we have a place to put the decoded frame
            if (!rb.reserveSlot(decodeFrameIndex)) {
                // Buffer full - wait and retry
                try { Thread.sleep(5); } catch (InterruptedException e) { break; }
                continue;
            }
            
            // Mark frames as pending BEFORE queueing to decoders
            synchronized (frameLock) {
                pendingColorFrames.add(decodeFrameIndex);
                pendingAlphaFrames.add(decodeFrameIndex);
            }
            
            // Read packets - need to verify we got one color and one alpha
            // The demuxer returns packets in file order, which may not be strictly alternating
            Object[] packet1 = nativeReadPacket(nativeDemuxerPtr);
            if (packet1 == null) {
                // End of file - clear pending since we didn't actually queue anything
                synchronized (frameLock) {
                    pendingColorFrames.remove(decodeFrameIndex);
                    pendingAlphaFrames.remove(decodeFrameIndex);
                }
                
                Log.i(TAG, "🔚 End of file at frame " + decodeFrameIndex + " (total decoded this pass)");
                
                if (loop) {
                    loopCount++;
                    long now = System.nanoTime();
                    long timeSinceLastLoop = (lastLoopTime > 0) ? (now - lastLoopTime) / 1_000_000 : -1;
                    lastLoopTime = now;
                    
                    Log.i(TAG, "🔁 Loop #" + loopCount + " - calling handleLoop() (time since last loop: " + timeSinceLastLoop + "ms)");
                    
                    // Detect stuck loop (immediate EOF after seek)
                    if (decodeFrameIndex == 0) {
                        Log.e(TAG, "❌ STUCK LOOP DETECTED: EOF at frame 0 immediately after seek!");
                        Log.e(TAG, "❌ This indicates the demuxer seek didn't work properly");
                        // Don't infinitely loop - wait a bit and try again, or give up
                        if (loopCount > 5) {
                            Log.e(TAG, "❌ Too many stuck loops, stopping playback");
                            if (eventCallback != null) {
                                eventCallback.onError("Demuxer seek failed - cannot loop video");
                            }
                            break;
                        }
                        try { Thread.sleep(100); } catch (InterruptedException e) { break; }
                    }
                    
                    handleLoop();
                    Log.i(TAG, "🔁 handleLoop() returned - demux loop continuing from decodeFrameIndex=" + decodeFrameIndex);
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
                synchronized (frameLock) {
                    pendingColorFrames.remove(decodeFrameIndex);
                    pendingAlphaFrames.remove(decodeFrameIndex);
                }
                continue;
            }
            
            // Determine which packet is color (stream 0) and which is alpha (stream 1)
            int stream1 = (Integer) packet1[0];
            int stream2 = (Integer) packet2[0];
            
            Object[] colorPacket;
            Object[] alphaPacket;
            
            if (stream1 == 0 && stream2 == 1) {
                // Expected order: color then alpha
                colorPacket = packet1;
                alphaPacket = packet2;
            } else if (stream1 == 1 && stream2 == 0) {
                // Swapped order: alpha then color
                colorPacket = packet2;
                alphaPacket = packet1;
                Log.d(TAG, "📦 Swapped packet order for frame " + decodeFrameIndex + " (alpha came first)");
            } else {
                // Both same stream - serious file format issue
                Log.e(TAG, "⚠️ STREAM MISMATCH: got streams " + stream1 + "," + stream2 + 
                      " for frame " + decodeFrameIndex + " - expected one color (0) and one alpha (1)");
                synchronized (frameLock) {
                    pendingColorFrames.remove(decodeFrameIndex);
                    pendingAlphaFrames.remove(decodeFrameIndex);
                }
                continue;
            }
            
            // Verify frame indices from demuxer match
            int colorFrameIdx = (Integer) colorPacket[4];
            int alphaFrameIdx = (Integer) alphaPacket[4];
            if (colorFrameIdx != alphaFrameIdx) {
                Log.w(TAG, "⚠️ Frame index mismatch: color=" + colorFrameIdx + 
                      ", alpha=" + alphaFrameIdx + " (decode frame " + decodeFrameIndex + ")");
            }
            
            // Queue to decoders
            ByteBuffer colorData = (ByteBuffer) colorPacket[1];
            long colorPts = (Long) colorPacket[3];
            ByteBuffer alphaData = (ByteBuffer) alphaPacket[1];
            long alphaPts = (Long) alphaPacket[3];
            
            if (shouldStop.get()) break;
            
            // Queue packets - these feed the async decoder
            SurfaceTextureVp9Decoder cd = colorDecoder;
            SurfaceTextureVp9Decoder ad = alphaDecoder;
            if (cd == null || ad == null) break;
            
            // Log PTS comparison for sync diagnosis
            if (colorPts != alphaPts) {
                Log.w(TAG, "⚠️ PTS MISMATCH frame " + decodeFrameIndex + 
                      ": color=" + colorPts + "us, alpha=" + alphaPts + 
                      "us, delta=" + (colorPts - alphaPts) + "us");
            }
            
            // Log first few frames after a loop (when decodeFrameIndex < 3)
            if (decodeFrameIndex < 3) {
                Log.i(TAG, "🎬 Queueing frame " + decodeFrameIndex + " (colorPts=" + colorPts + "us, alphaPts=" + alphaPts + "us)");
            }
            
            while (!cd.queueInputPacket(colorData, colorPts, decodeFrameIndex) && !shouldStop.get()) {
                try { Thread.sleep(1); } catch (InterruptedException e) { break; }
            }
            
            while (!ad.queueInputPacket(alphaData, alphaPts, decodeFrameIndex) && !shouldStop.get()) {
                try { Thread.sleep(1); } catch (InterruptedException e) { break; }
            }
            
            decodeFrameIndex++;
        }
        
        Log.i(TAG, "Demux loop ended");
    }
    
    private void handleLoop() {
        Log.i(TAG, "🔄 Looping... starting loop transition");
        
        FrameRingBuffer rb = ringBuffer;
        int preLoopBufferCount = (rb != null) ? rb.getBufferedCount() : -1;
        Log.i(TAG, "🔄 Pre-loop buffer state: count=" + preLoopBufferCount);
        
        // Wait for buffer to drain (with timeout)
        int waitCount = 0;
        while (!shouldStop.get() && waitCount++ < 300) {  // 3 second timeout
            rb = ringBuffer;
            if (rb == null || rb.isEmpty()) {
                break;
            }
            if (waitCount % 50 == 0) {
                Log.d(TAG, "🔄 Waiting for buffer drain: count=" + rb.getBufferedCount() + ", wait=" + waitCount);
            }
            try { Thread.sleep(10); } catch (InterruptedException e) { break; }
        }
        
        if (shouldStop.get()) {
            Log.w(TAG, "🔄 Loop aborted - shouldStop is true");
            return;
        }
        
        if (waitCount >= 300) {
            Log.w(TAG, "🔄 Buffer drain timeout after 3 seconds");
        }
        
        Log.i(TAG, "🔄 Flushing decoders...");
        
        // Set loop transition flag BEFORE flush - any frames arriving after this
        // with high indices are stale and should be discarded
        inLoopTransition.set(true);
        Log.i(TAG, "🔄 Loop transition started - stale frame detection enabled");
        
        // Flush decoders
        if (colorDecoder != null) {
            colorDecoder.flush();
        }
        if (alphaDecoder != null) {
            alphaDecoder.flush();
        }
        
        Log.i(TAG, "🔄 Decoders flushed, clearing buffer...");
        
        // Clear ring buffer to reset any incomplete reserved slots
        rb = ringBuffer;
        if (rb != null) {
            rb.clear();
        }
        
        // Seek demuxer
        Log.i(TAG, "🔄 Seeking demuxer to start...");
        if (nativeDemuxerPtr != 0) {
            nativeSeekToStart(nativeDemuxerPtr);
        }
        
        // Reset state
        synchronized (frameLock) {
            pendingColorFrames.clear();
            pendingAlphaFrames.clear();
            frameLock.notifyAll();
        }
        decodeFrameIndex = 0;
        lastCopiedFrameIndex = -1;
        currentFrameIndex.set(0);
        
        // Reset counters
        framesRendered = 0;
        framesDropped = 0;
        framesBuffered = 0;
        lastPerfLogTime = System.nanoTime();
        
        if (eventCallback != null) {
            eventCallback.onLooped();
        }
        
        Log.i(TAG, "🔄 Loop complete - demux will continue from frame 0");
    }
    
    // -------------------------------------------------------------------------
    // Render loop - consumes buffer at frame rate
    // -------------------------------------------------------------------------
    
    private void renderLoop() {
        Log.i(TAG, "Render loop started");
        
        long frameDurationNs = (long)(1_000_000_000.0 / frameRate);
        long nextFrameTimeNs = System.nanoTime();
        
        // Initial buffer fill delay - wait for at least 5 frames or timeout
        Log.i(TAG, "Waiting for initial buffer fill...");
        int waitCount = 0;
        while (!shouldStop.get()) {
            FrameRingBuffer rb = ringBuffer;  // Local copy for null safety
            if (rb == null) {
                Log.w(TAG, "Ring buffer is null during initial wait, aborting");
                return;
            }
            int buffered = rb.getBufferedCount();
            if (buffered >= 5) {
                break;
            }
            if (waitCount++ > 200) {  // 2 second timeout
                Log.w(TAG, "Initial buffer fill timeout, proceeding with " + buffered + " frames");
                break;
            }
            try { Thread.sleep(10); } catch (InterruptedException e) { break; }
        }
        
        // Check again after wait
        FrameRingBuffer rb = ringBuffer;
        if (rb == null || shouldStop.get()) {
            Log.w(TAG, "Ring buffer null or stopped after initial wait");
            return;
        }
        
        Log.i(TAG, "Buffer ready: " + rb.getBufferedCount() + " frames");
        nextFrameTimeNs = System.nanoTime();
        
        // Track consecutive underruns for diagnostics
        int consecutiveUnderruns = 0;
        int lastLoggedFrame = -1;
        
        while (!shouldStop.get() && isLoaded.get()) {
            // Get local reference for null safety
            rb = ringBuffer;
            if (rb == null) {
                Log.w(TAG, "Ring buffer became null during playback");
                break;
            }
            
            if (!isPlaying.get()) {
                try { Thread.sleep(16); } catch (InterruptedException e) { break; }
                nextFrameTimeNs = System.nanoTime();
                consecutiveUnderruns = 0;
                continue;
            }
            
            long now = System.nanoTime();
            
            if (now >= nextFrameTimeNs) {
                // Time to render
                FrameRingBuffer.FrameSlot slot = rb.readFrame();
                
                if (slot != null && renderCallback != null) {
                    // Log when we successfully read after underruns (recovery)
                    if (consecutiveUnderruns > 10) {
                        Log.i(TAG, "📺 RECOVERED after " + consecutiveUnderruns + " underruns, now at frame " + slot.frameIndex);
                    }
                    
                    // Log significant frame transitions (e.g., loop restart)
                    if (lastLoggedFrame > 0 && slot.frameIndex == 0) {
                        Log.i(TAG, "📺 LOOP DETECTED in render: jumped from frame " + lastLoggedFrame + " to frame 0");
                    }
                    lastLoggedFrame = slot.frameIndex;
                    
                    renderCallback.onFrameReady(slot.frameIndex, slot.colorTextureId, slot.alphaTextureId);
                    currentFrameIndex.set(slot.frameIndex);
                    framesRendered++;
                    consecutiveUnderruns = 0;
                    nextFrameTimeNs += frameDurationNs;
                } else {
                    // Buffer underrun
                    consecutiveUnderruns++;
                    framesDropped++;
                    
                    // Log periodically during extended underruns
                    if (consecutiveUnderruns == 50 || consecutiveUnderruns == 200 || consecutiveUnderruns == 500) {
                        int buffered = rb.getBufferedCount();
                        int nextExpected = rb.getNextExpectedFrameIndex();
                        Log.w(TAG, "📺 EXTENDED UNDERRUN: " + consecutiveUnderruns + " consecutive, " +
                              "buffered=" + buffered + ", nextExpected=" + nextExpected + 
                              ", lastRendered=" + lastLoggedFrame);
                    }
                    
                    try { Thread.sleep(1); } catch (InterruptedException e) { break; }
                }
                
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
            
            // Performance logging
            if (now - lastPerfLogTime >= 2_000_000_000L) {
                double elapsed = (now - lastPerfLogTime) / 1_000_000_000.0;
                double fps = framesRendered / elapsed;
                
                int bufferedCount = rb.getBufferedCount();
                int capacity = rb.getCapacity();
                
                Log.i(TAG, String.format("⏱ PERF: fps=%.1f (target=%.1f), rendered=%d, dropped=%d, buffered=%d/%d",
                    fps, frameRate, framesRendered, framesDropped, bufferedCount, capacity));
                
                if (eventCallback != null) {
                    eventCallback.onBufferState(bufferedCount, capacity);
                }
                
                framesRendered = 0;
                framesDropped = 0;
                lastPerfLogTime = now;
            }
        }
        
        Log.i(TAG, "Render loop ended");
    }
    
    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------
    
    private void cleanup() {
        Log.i(TAG, "🧹 Starting cleanup...");
        
        // First, signal stop to all threads
        shouldStop.set(true);
        
        // Wake up any waiting threads
        synchronized (frameLock) {
            pendingColorFrames.clear();
            pendingAlphaFrames.clear();
            frameLock.notifyAll();
        }
        
        // Stop demux thread first (it feeds the decoders)
        if (demuxThread != null) {
            demuxThread.quitSafely();
            try {
                demuxThread.join(500);  // Wait up to 500ms for thread to finish
            } catch (InterruptedException e) {
                Log.w(TAG, "Interrupted waiting for demux thread");
            }
            demuxThread = null;
            demuxHandler = null;
        }
        
        // Stop playback thread (it reads from ring buffer)
        if (playbackThread != null) {
            playbackThread.quitSafely();
            try {
                playbackThread.join(500);  // Wait up to 500ms for thread to finish
            } catch (InterruptedException e) {
                Log.w(TAG, "Interrupted waiting for playback thread");
            }
            playbackThread = null;
            playbackHandler = null;
        }
        
        // Now safe to release resources
        if (colorDecoder != null) {
            colorDecoder.release();
            colorDecoder = null;
        }
        
        if (alphaDecoder != null) {
            alphaDecoder.release();
            alphaDecoder = null;
        }
        
        if (ringBuffer != null) {
            ringBuffer.release();
            ringBuffer = null;
        }
        
        if (nativeDemuxerPtr != 0) {
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
        }
        
        currentFrameIndex.set(0);
        lastCopiedFrameIndex = -1;
        
        Log.i(TAG, "🧹 Cleanup complete");
    }
    
    // -------------------------------------------------------------------------
    // Native methods
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
