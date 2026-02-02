package com.danielkpl2.webmalphaplayer;

import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Dual-stream VP9 alpha player using MediaCodec hardware decoding.
 * 
 * For WebM files where alpha is stored as a second VP9 video stream:
 * - Stream 0: Color video (YUV420)
 * - Stream 1: Alpha video (grayscale, Y plane = alpha)
 * 
 * Uses FFmpeg for demuxing (via JNI) and MediaCodec for hardware decoding.
 */
public class DualStreamMediaCodecPlayer {
    private static final String TAG = "DualStreamMC";
    
    // Native demuxer handle
    private long nativeDemuxerPtr = 0;
    
    // MediaCodec decoders
    private MediaCodecVp9Decoder colorDecoder;
    private MediaCodecVp9Decoder alphaDecoder;
    
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
    
    // Frame synchronization
    // Maps frame index -> decoded color frame
    private final ConcurrentHashMap<Integer, MediaCodecVp9Decoder.DecodedFrame> colorFrameBuffer = new ConcurrentHashMap<>();
    // Maps frame index -> decoded alpha frame (Y plane only used)
    private final ConcurrentHashMap<Integer, MediaCodecVp9Decoder.DecodedFrame> alphaFrameBuffer = new ConcurrentHashMap<>();
    
    // Composite frames ready for rendering
    private final LinkedBlockingQueue<CompositeFrame> outputFrameQueue = new LinkedBlockingQueue<>(60);
    
    // Flow control: track frames in pipeline to prevent racing ahead
    // Total in-flight = pendingColorFrames + pendingAlphaFrames + outputQueue
    private final AtomicInteger pendingColorFrames = new AtomicInteger(0);
    private final AtomicInteger pendingAlphaFrames = new AtomicInteger(0);
    private static final int TARGET_BUFFER_FRAMES = 15;  // Keep ~15 frames buffered ahead
    private static final int MAX_BUFFER_FRAMES = 25;     // Pause demux when buffer exceeds this
    private static final int MIN_BUFFER_FRAMES = 8;      // Resume demux when buffer drops below this
    
    // Track the highest frame index that has been successfully composited
    // Used to detect and discard duplicate/orphaned frames
    private final AtomicInteger lastCompositedFrameIndex = new AtomicInteger(-1);
    
    // Threading
    private HandlerThread demuxThread;
    private Handler demuxHandler;
    private HandlerThread compositeThread;
    private Handler compositeHandler;
    private HandlerThread playbackThread;
    private Handler playbackHandler;
    
    // Configuration
    private boolean loop = true;
    private FrameCallback frameCallback;
    private EventCallback eventCallback;
    
    /**
     * Composite YUVA frame ready for OpenGL rendering.
     */
    public static class CompositeFrame {
        public final int frameIndex;
        public final ByteBuffer yPlane;
        public final ByteBuffer uPlane;
        public final ByteBuffer vPlane;
        public final ByteBuffer aPlane;
        public final int width;
        public final int height;
        
        public CompositeFrame(int frameIndex, ByteBuffer y, ByteBuffer u, ByteBuffer v, ByteBuffer a,
                             int width, int height) {
            this.frameIndex = frameIndex;
            this.yPlane = y;
            this.uPlane = u;
            this.vPlane = v;
            this.aPlane = a;
            this.width = width;
            this.height = height;
        }
    }
    
    public interface FrameCallback {
        void onFrame(int frameIndex, ByteBuffer y, ByteBuffer u, ByteBuffer v, ByteBuffer a,
                    int width, int height);
    }
    
    public interface EventCallback {
        void onLoaded(int totalFrames, int width, int height, double frameRate);
        void onPlay();
        void onPause();
        void onLoop();
        void onEnd();
        void onError(String error);
    }
    
    static {
        // Load native library (same as WebMAlphaPlayerNative)
        System.loadLibrary("webmalphaplayer");
    }
    
    public DualStreamMediaCodecPlayer() {
        colorDecoder = new MediaCodecVp9Decoder("color");
        alphaDecoder = new MediaCodecVp9Decoder("alpha");
    }
    
    /**
     * Load a dual-stream WebM file.
     * @param filePath Path to the WebM file
     * @return true if loaded successfully
     */
    public boolean loadFromFile(String filePath) {
        if (isLoaded.get()) {
            unload();
        }
        
        shouldStop.set(false);
        
        // Initialize native demuxer
        nativeDemuxerPtr = nativeCreateDemuxer(filePath);
        if (nativeDemuxerPtr == 0) {
            Log.e(TAG, "Failed to create native demuxer for: " + filePath);
            if (eventCallback != null) {
                eventCallback.onError("Failed to open file");
            }
            return false;
        }
        
        // Get video info
        int[] videoInfo = nativeGetVideoInfo(nativeDemuxerPtr);
        if (videoInfo == null || videoInfo.length < 5) {
            Log.e(TAG, "Failed to get video info");
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
            if (eventCallback != null) {
                eventCallback.onError("Invalid video format");
            }
            return false;
        }
        
        width = videoInfo[0];
        height = videoInfo[1];
        totalFrames = videoInfo[2];
        frameRate = videoInfo[3] / 1000.0; // Stored as rate * 1000
        int streamCount = videoInfo[4];
        
        Log.i(TAG, "Video info: " + width + "x" + height + ", " + totalFrames + " frames, " + 
              frameRate + " fps, " + streamCount + " streams");
        
        if (streamCount < 2) {
            Log.e(TAG, "Not a dual-stream file (only " + streamCount + " stream)");
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
            if (eventCallback != null) {
                eventCallback.onError("Not a dual-stream file");
            }
            return false;
        }
        
        duration = totalFrames / frameRate;
        
        // Configure MediaCodec decoders
        if (!colorDecoder.configure(width, height)) {
            Log.e(TAG, "Failed to configure color decoder");
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
            if (eventCallback != null) {
                eventCallback.onError("Failed to configure color decoder");
            }
            return false;
        }
        
        if (!alphaDecoder.configure(width, height)) {
            Log.e(TAG, "Failed to configure alpha decoder");
            colorDecoder.release();
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
            if (eventCallback != null) {
                eventCallback.onError("Failed to configure alpha decoder");
            }
            return false;
        }
        
        // Set up frame callbacks for synchronization
        colorDecoder.setFrameCallback(new MediaCodecVp9Decoder.DecodedFrameCallback() {
            @Override
            public void onFrameDecoded(MediaCodecVp9Decoder.DecodedFrame frame) {
                onColorFrameDecoded(frame);
            }
            
            @Override
            public void onDecoderError(String error) {
                Log.e(TAG, "Color decoder error: " + error);
            }
        });
        
        alphaDecoder.setFrameCallback(new MediaCodecVp9Decoder.DecodedFrameCallback() {
            @Override
            public void onFrameDecoded(MediaCodecVp9Decoder.DecodedFrame frame) {
                onAlphaFrameDecoded(frame);
            }
            
            @Override
            public void onDecoderError(String error) {
                Log.e(TAG, "Alpha decoder error: " + error);
            }
        });
        
        // Start decoders
        if (!colorDecoder.start() || !alphaDecoder.start()) {
            Log.e(TAG, "Failed to start decoders");
            release();
            if (eventCallback != null) {
                eventCallback.onError("Failed to start decoders");
            }
            return false;
        }
        
        // Start background threads
        demuxThread = new HandlerThread("DemuxThread");
        demuxThread.start();
        demuxHandler = new Handler(demuxThread.getLooper());
        
        compositeThread = new HandlerThread("CompositeThread");
        compositeThread.start();
        compositeHandler = new Handler(compositeThread.getLooper());
        
        playbackThread = new HandlerThread("PlaybackThread");
        playbackThread.start();
        playbackHandler = new Handler(playbackThread.getLooper());
        
        isLoaded.set(true);
        
        // Start demuxing
        demuxHandler.post(this::demuxLoop);
        
        if (eventCallback != null) {
            eventCallback.onLoaded(totalFrames, width, height, frameRate);
        }
        
        Log.i(TAG, "Loaded successfully: " + totalFrames + " frames, " + width + "x" + height);
        return true;
    }
    
    /**
     * Start playback.
     */
    public void play() {
        if (!isLoaded.get() || isPlaying.get()) return;
        
        isPlaying.set(true);
        playbackHandler.post(this::playbackLoop);
        
        if (eventCallback != null) {
            eventCallback.onPlay();
        }
    }
    
    /**
     * Pause playback.
     */
    public void pause() {
        isPlaying.set(false);
        
        if (eventCallback != null) {
            eventCallback.onPause();
        }
    }
    
    /**
     * Stop and reset to beginning.
     */
    public void stop() {
        isPlaying.set(false);
        currentFrameIndex.set(0);
        
        // Flush decoders
        colorDecoder.flush();
        alphaDecoder.flush();
        
        // Clear buffers and reset pending counts
        colorFrameBuffer.clear();
        alphaFrameBuffer.clear();
        outputFrameQueue.clear();
        pendingColorFrames.set(0);
        pendingAlphaFrames.set(0);
        lastCompositedFrameIndex.set(-1);
        
        // Seek demuxer to start
        if (nativeDemuxerPtr != 0) {
            nativeSeekToStart(nativeDemuxerPtr);
        }
    }
    
    /**
     * Release all resources.
     */
    public void unload() {
        Log.i(TAG, "Unloading");
        
        shouldStop.set(true);
        isPlaying.set(false);
        isLoaded.set(false);
        
        // Stop threads
        if (demuxThread != null) {
            demuxThread.quitSafely();
            demuxThread = null;
            demuxHandler = null;
        }
        
        if (compositeThread != null) {
            compositeThread.quitSafely();
            compositeThread = null;
            compositeHandler = null;
        }
        
        if (playbackThread != null) {
            playbackThread.quitSafely();
            playbackThread = null;
            playbackHandler = null;
        }
        
        // Release decoders
        if (colorDecoder != null) {
            colorDecoder.release();
        }
        if (alphaDecoder != null) {
            alphaDecoder.release();
        }
        
        // Release native demuxer
        if (nativeDemuxerPtr != 0) {
            nativeDestroyDemuxer(nativeDemuxerPtr);
            nativeDemuxerPtr = 0;
        }
        
        // Clear buffers and reset pending counts
        colorFrameBuffer.clear();
        alphaFrameBuffer.clear();
        outputFrameQueue.clear();
        pendingColorFrames.set(0);
        pendingAlphaFrames.set(0);
        lastCompositedFrameIndex.set(-1);
        
        Log.i(TAG, "Unloaded");
    }
    
    public void release() {
        unload();
        colorDecoder = null;
        alphaDecoder = null;
    }
    
    // -------------------------------------------------------------------------
    // Getters
    // -------------------------------------------------------------------------
    
    public boolean isLoaded() { return isLoaded.get(); }
    public boolean isPlaying() { return isPlaying.get(); }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getTotalFrames() { return totalFrames; }
    public double getFrameRate() { return frameRate; }
    public double getDuration() { return duration; }
    public int getCurrentFrame() { return currentFrameIndex.get(); }
    
    public void setLoop(boolean loop) { this.loop = loop; }
    public void setFrameCallback(FrameCallback callback) { this.frameCallback = callback; }
    public void setEventCallback(EventCallback callback) { this.eventCallback = callback; }
    
    // -------------------------------------------------------------------------
    // Internal: Demuxing
    // -------------------------------------------------------------------------
    
    /**
     * Get total frames in the pipeline (pending decode + output queue).
     * This controls the demux pace to avoid racing ahead of playback.
     */
    private int getBufferedFrameCount() {
        // Count frames in various stages:
        // - pendingColorFrames: packets sent to color decoder, not yet decoded
        // - colorFrameBuffer: decoded but waiting for alpha pair
        // - outputFrameQueue: composited, waiting for playback
        // We use color frames as the reference since color and alpha are 1:1
        return pendingColorFrames.get() + colorFrameBuffer.size() + outputFrameQueue.size();
    }
    
    private void demuxLoop() {
        Log.i(TAG, "Demux loop started");
        
        int packetCount = 0;
        int colorPackets = 0;
        int alphaPackets = 0;
        int lastLoggedFrame = -1;
        
        while (!shouldStop.get() && nativeDemuxerPtr != 0) {
            // === FLOW CONTROL ===
            // Pause demuxing when we have enough frames buffered
            // This prevents racing way ahead of playback
            int buffered = getBufferedFrameCount();
            if (buffered >= MAX_BUFFER_FRAMES) {
                // Buffer is full enough - wait for playback to consume some frames
                int lastBuffered = buffered;
                int noProgressCount = 0;
                final int MAX_NO_PROGRESS_ITERATIONS = 100; // ~1.6 seconds with 16ms sleep
                
                while (buffered > MIN_BUFFER_FRAMES && !shouldStop.get() && isPlaying.get()) {
                    try {
                        Thread.sleep(16); // ~1 frame at 60fps
                    } catch (InterruptedException ignored) {}
                    buffered = getBufferedFrameCount();
                    
                    // Check for progress - if buffered count doesn't decrease for too long,
                    // there may be orphaned frames causing a deadlock
                    if (buffered >= lastBuffered) {
                        noProgressCount++;
                        if (noProgressCount >= MAX_NO_PROGRESS_ITERATIONS) {
                            Log.w(TAG, "Flow control timeout - no progress for " + 
                                  (noProgressCount * 16) + "ms (buffered=" + buffered + 
                                  ", colorBuffer=" + colorFrameBuffer.size() + 
                                  ", alphaBuffer=" + alphaFrameBuffer.size() + 
                                  ", queue=" + outputFrameQueue.size() + 
                                  ", pending=" + pendingColorFrames.get() + "/" + pendingAlphaFrames.get() + ")");
                            
                            // Clear potential orphaned frames from buffers
                            // These are frames that will never get their pair
                            int orphansCleared = 0;
                            int lastComposited = lastCompositedFrameIndex.get();
                            for (Integer frameIdx : colorFrameBuffer.keySet()) {
                                if (frameIdx <= lastComposited) {
                                    colorFrameBuffer.remove(frameIdx);
                                    orphansCleared++;
                                }
                            }
                            for (Integer frameIdx : alphaFrameBuffer.keySet()) {
                                if (frameIdx <= lastComposited) {
                                    alphaFrameBuffer.remove(frameIdx);
                                    orphansCleared++;
                                }
                            }
                            if (orphansCleared > 0) {
                                Log.w(TAG, "Cleared " + orphansCleared + " orphaned frames");
                            }
                            break; // Exit wait loop and continue demuxing
                        }
                    } else {
                        // Progress was made, reset counter
                        noProgressCount = 0;
                        lastBuffered = buffered;
                    }
                }
                if (shouldStop.get()) break;
            }
            
            // Read next packet
            VP9Packet packet = nativeReadPacket(nativeDemuxerPtr);
            
            if (packet == null) {
                // End of file
                Log.i(TAG, "End of file reached, packets: " + packetCount + 
                      " (color: " + colorPackets + ", alpha: " + alphaPackets + ")");
                
                if (loop) {
                    // === LOOPING ===
                    // Wait for all pending frames to be decoded and consumed
                    
                    // Step 1: Wait for all pending packets to be decoded
                    Log.d(TAG, "EOF - waiting for pending frames to decode (pending=" + 
                          pendingColorFrames.get() + "/" + pendingAlphaFrames.get() + ")...");
                    int waitCount = 0;
                    int maxWait = 300; // Max ~3 seconds
                    while ((pendingColorFrames.get() > 0 || pendingAlphaFrames.get() > 0) && 
                           waitCount < maxWait && !shouldStop.get()) {
                        try {
                            Thread.sleep(10);
                            waitCount++;
                        } catch (InterruptedException ignored) {}
                    }
                    
                    if (shouldStop.get()) break;
                    
                    // Step 2: Wait for playback to consume the buffered frames
                    Log.d(TAG, "Waiting for playback to consume frames (queue=" + outputFrameQueue.size() + ")...");
                    while (outputFrameQueue.size() > 2 && !shouldStop.get() && isPlaying.get()) {
                        try {
                            Thread.sleep(16);
                        } catch (InterruptedException ignored) {}
                    }
                    
                    if (shouldStop.get()) break;
                    
                    Log.d(TAG, "Looping - flushing decoders");
                    
                    // Flush decoders
                    colorDecoder.flush();
                    alphaDecoder.flush();
                    
                    // Clear frame buffers and reset pending counts
                    colorFrameBuffer.clear();
                    alphaFrameBuffer.clear();
                    pendingColorFrames.set(0);
                    pendingAlphaFrames.set(0);
                    
                    // Reset composited frame tracking for new loop
                    lastCompositedFrameIndex.set(-1);
                    
                    // Seek to start
                    nativeSeekToStart(nativeDemuxerPtr);
                    
                    packetCount = 0;
                    colorPackets = 0;
                    alphaPackets = 0;
                    lastLoggedFrame = -1;
                    
                    Log.i(TAG, "Loop restart complete");
                    continue;
                } else {
                    // Not looping - signal end of stream to decoders
                    colorDecoder.signalEndOfStream();
                    alphaDecoder.signalEndOfStream();
                    break;
                }
            }
            
            // Queue to appropriate decoder and track pending frames
            if (packet.streamIndex == 0) {
                pendingColorFrames.incrementAndGet();
                colorDecoder.queueInputPacket(
                    packet.data, 
                    packet.frameIndex,
                    packet.presentationTimeUs, 
                    packet.isKeyFrame
                );
                colorPackets++;
            } else if (packet.streamIndex == 1) {
                pendingAlphaFrames.incrementAndGet();
                alphaDecoder.queueInputPacket(
                    packet.data, 
                    packet.frameIndex,
                    packet.presentationTimeUs, 
                    packet.isKeyFrame
                );
                alphaPackets++;
            }
            
            packetCount++;
            
            // Log progress periodically
            if (packet.frameIndex / 30 > lastLoggedFrame / 30) {
                Log.d(TAG, "Demuxing frame " + packet.frameIndex + ", buffered=" + getBufferedFrameCount());
                lastLoggedFrame = packet.frameIndex;
            }
        }
        
        Log.i(TAG, "Demux loop ended");
    }
    
    // -------------------------------------------------------------------------
    // Internal: Frame synchronization
    // -------------------------------------------------------------------------
    
    private void onColorFrameDecoded(MediaCodecVp9Decoder.DecodedFrame frame) {
        pendingColorFrames.decrementAndGet();
        
        // Skip frames that have already been composited (duplicate detection)
        int lastComposited = lastCompositedFrameIndex.get();
        if (frame.frameIndex <= lastComposited && lastComposited >= 0) {
            // This frame was already processed - it's a duplicate
            if (frame.frameIndex % 30 == 0) {
                Log.d(TAG, "Color frame " + frame.frameIndex + " skipped (already composited " + lastComposited + ")");
            }
            return;
        }
        
        if (frame.frameIndex % 30 == 0) {
            Log.d(TAG, "Color frame decoded: " + frame.frameIndex + ", pending=" + pendingColorFrames.get());
        }
        colorFrameBuffer.put(frame.frameIndex, frame);
        tryCompositeFrame(frame.frameIndex);
    }
    
    private void onAlphaFrameDecoded(MediaCodecVp9Decoder.DecodedFrame frame) {
        pendingAlphaFrames.decrementAndGet();
        
        // Skip frames that have already been composited (duplicate detection)
        int lastComposited = lastCompositedFrameIndex.get();
        if (frame.frameIndex <= lastComposited && lastComposited >= 0) {
            // This frame was already processed - it's a duplicate
            if (frame.frameIndex % 30 == 0) {
                Log.d(TAG, "Alpha frame " + frame.frameIndex + " skipped (already composited " + lastComposited + ")");
            }
            return;
        }
        
        if (frame.frameIndex % 30 == 0) {
            Log.d(TAG, "Alpha frame decoded: " + frame.frameIndex + ", pending=" + pendingAlphaFrames.get());
        }
        alphaFrameBuffer.put(frame.frameIndex, frame);
        tryCompositeFrame(frame.frameIndex);
    }
    
    // Composite performance tracking
    private volatile long lastCompositeLogTime = 0;
    private volatile int compositeCount = 0;
    private volatile long totalCompositeTimeNs = 0;
    
    private void tryCompositeFrame(int frameIndex) {
        MediaCodecVp9Decoder.DecodedFrame colorFrame = colorFrameBuffer.get(frameIndex);
        MediaCodecVp9Decoder.DecodedFrame alphaFrame = alphaFrameBuffer.get(frameIndex);
        
        if (colorFrame != null && alphaFrame != null) {
            // Both frames available - remove from buffers first to prevent double-processing
            colorFrameBuffer.remove(frameIndex);
            alphaFrameBuffer.remove(frameIndex);
            
            // Update the last composited frame index
            // This helps detect and discard duplicate frames
            lastCompositedFrameIndex.updateAndGet(current -> Math.max(current, frameIndex));
            
            if (frameIndex % 30 == 0) {
                Log.d(TAG, "Compositing frame " + frameIndex + 
                      " (queue=" + outputFrameQueue.size() + ", buffered=" + getBufferedFrameCount() + ")");
            }
            
            // Composite on background thread
            compositeHandler.post(() -> doComposite(frameIndex, colorFrame, alphaFrame));
        }
    }
    
    private void doComposite(int frameIndex, 
                            MediaCodecVp9Decoder.DecodedFrame colorFrame,
                            MediaCodecVp9Decoder.DecodedFrame alphaFrame) {
        long startNs = System.nanoTime();
        
        // Create composite frame
        // Alpha is stored in Y plane of alpha frame (it's grayscale VP9)
        CompositeFrame composite = new CompositeFrame(
            frameIndex,
            colorFrame.yPlane,
            colorFrame.uPlane,
            colorFrame.vPlane,
            alphaFrame.yPlane, // Y plane of alpha video = alpha channel
            colorFrame.width,
            colorFrame.height
        );
        
        // Queue for playback
        if (!outputFrameQueue.offer(composite)) {
            Log.w(TAG, "Output queue full, dropping frame " + frameIndex);
        }
        
        long endNs = System.nanoTime();
        totalCompositeTimeNs += (endNs - startNs);
        compositeCount++;
        
        // Log composite performance periodically
        long now = System.currentTimeMillis();
        if (now - lastCompositeLogTime >= 2000 && compositeCount > 0) {
            double avgCompositeMs = totalCompositeTimeNs / 1_000_000.0 / compositeCount;
            Log.i(TAG, String.format("⏱ COMPOSITE: avgTime=%.2fms, count=%d", avgCompositeMs, compositeCount));
            lastCompositeLogTime = now;
            // Don't reset - keep accumulating for session average
        }
    }
    
    // -------------------------------------------------------------------------
    // Internal: Playback
    // -------------------------------------------------------------------------
    
    private void playbackLoop() {
        Log.i(TAG, "Playback loop started, frameRate=" + frameRate);
        
        // Use nanoseconds for precise timing
        final long frameDurationNs = (long)(1_000_000_000.0 / frameRate);
        long nextFrameTimeNs = System.nanoTime();
        int framesRendered = 0;
        int framesDropped = 0;
        int starvationCount = 0;  // Frames where queue was empty
        long lastLogTime = System.currentTimeMillis();
        
        // Performance metrics
        long totalPollTimeNs = 0;
        long totalRenderTimeNs = 0;
        long totalWaitTimeNs = 0;
        long maxRenderTimeNs = 0;
        int pollCount = 0;
        long fpsWindowStart = System.nanoTime();
        int fpsWindowFrames = 0;
        
        while (!shouldStop.get() && isPlaying.get()) {
            long loopStartNs = System.nanoTime();
            
            // Wait until it's time for the next frame
            long waitNs = nextFrameTimeNs - loopStartNs;
            if (waitNs > 1_000_000) { // > 1ms to wait
                try {
                    Thread.sleep(waitNs / 1_000_000, (int)(waitNs % 1_000_000));
                } catch (InterruptedException ignored) {}
            }
            totalWaitTimeNs += Math.max(0, waitNs);
            
            long nowNs = System.nanoTime();
            
            // Check if it's time for the next frame
            if (nowNs >= nextFrameTimeNs) {
                // Try to get a frame (non-blocking or very short wait)
                long pollStartNs = System.nanoTime();
                CompositeFrame frame = null;
                try {
                    frame = outputFrameQueue.poll(5, TimeUnit.MILLISECONDS);
                } catch (InterruptedException ignored) {}
                long pollEndNs = System.nanoTime();
                totalPollTimeNs += (pollEndNs - pollStartNs);
                pollCount++;
                
                if (frame != null) {
                    currentFrameIndex.set(frame.frameIndex);
                    
                    // Deliver frame to renderer and measure time
                    long renderStartNs = System.nanoTime();
                    if (frameCallback != null) {
                        frameCallback.onFrame(
                            frame.frameIndex,
                            frame.yPlane, frame.uPlane, frame.vPlane, frame.aPlane,
                            frame.width, frame.height
                        );
                    }
                    long renderEndNs = System.nanoTime();
                    long renderTimeNs = renderEndNs - renderStartNs;
                    totalRenderTimeNs += renderTimeNs;
                    maxRenderTimeNs = Math.max(maxRenderTimeNs, renderTimeNs);
                    
                    framesRendered++;
                    fpsWindowFrames++;
                    
                    // Check for end of video
                    if (frame.frameIndex >= totalFrames - 1) {
                        if (loop) {
                            currentFrameIndex.set(0);
                            if (eventCallback != null) {
                                eventCallback.onLoop();
                            }
                        } else {
                            isPlaying.set(false);
                            if (eventCallback != null) {
                                eventCallback.onEnd();
                            }
                            break;
                        }
                    }
                } else {
                    // No frame available - buffer underrun
                    framesDropped++;
                    if (outputFrameQueue.isEmpty()) {
                        starvationCount++;
                    }
                }
                
                // Schedule next frame - always advance by frame duration
                // to maintain consistent timing even if we drop frames
                nextFrameTimeNs += frameDurationNs;
                
                // If we're too far behind (>100ms), reset timing to prevent catchup burst
                if (nowNs - nextFrameTimeNs > 100_000_000) {
                    Log.w(TAG, "Playback too far behind, resetting timing");
                    nextFrameTimeNs = nowNs + frameDurationNs;
                }
            }
            
            // Log performance stats periodically
            long nowMs = System.currentTimeMillis();
            if (nowMs - lastLogTime >= 2000) {
                // Calculate actual FPS over this window
                long fpsWindowNs = System.nanoTime() - fpsWindowStart;
                double actualFps = (fpsWindowFrames > 0) ? (fpsWindowFrames * 1_000_000_000.0 / fpsWindowNs) : 0;
                
                double avgPollMs = pollCount > 0 ? (totalPollTimeNs / 1_000_000.0 / pollCount) : 0;
                double avgRenderMs = framesRendered > 0 ? (totalRenderTimeNs / 1_000_000.0 / framesRendered) : 0;
                double maxRenderMs = maxRenderTimeNs / 1_000_000.0;
                
                Log.i(TAG, String.format("⏱ PERF: fps=%.1f (target=%.1f), rendered=%d, dropped=%d, starved=%d",
                      actualFps, frameRate, framesRendered, framesDropped, starvationCount));
                Log.i(TAG, String.format("⏱ TIMING: avgPoll=%.1fms, avgRender=%.1fms, maxRender=%.1fms",
                      avgPollMs, avgRenderMs, maxRenderMs));
                Log.i(TAG, String.format("⏱ BUFFER: queue=%d, colorBuf=%d, alphaBuf=%d, pending=%d/%d",
                      outputFrameQueue.size(), colorFrameBuffer.size(), alphaFrameBuffer.size(),
                      pendingColorFrames.get(), pendingAlphaFrames.get()));
                
                // Reset FPS window
                fpsWindowStart = System.nanoTime();
                fpsWindowFrames = 0;
                lastLogTime = nowMs;
            }
        }
        
        Log.i(TAG, "Playback loop ended: rendered=" + framesRendered + ", dropped=" + framesDropped);
    }
    
    // -------------------------------------------------------------------------
    // Native methods (JNI to C++ FFmpeg demuxer)
    // -------------------------------------------------------------------------
    
    /**
     * VP9 packet from demuxer.
     */
    public static class VP9Packet {
        public byte[] data;
        public int streamIndex;       // 0 = color, 1 = alpha
        public int frameIndex;
        public long presentationTimeUs;
        public boolean isKeyFrame;
    }
    
    // Create demuxer for file
    private static native long nativeCreateDemuxer(String filePath);
    
    // Destroy demuxer
    private static native void nativeDestroyDemuxer(long demuxerPtr);
    
    // Get video info: [width, height, totalFrames, frameRate*1000, streamCount]
    private static native int[] nativeGetVideoInfo(long demuxerPtr);
    
    // Read next VP9 packet (returns null at EOF)
    private static native VP9Packet nativeReadPacket(long demuxerPtr);
    
    // Seek to start of file
    private static native void nativeSeekToStart(long demuxerPtr);
    
    // Check if file is dual-stream (has 2 video streams)
    public static native boolean nativeIsDualStream(String filePath);
}
