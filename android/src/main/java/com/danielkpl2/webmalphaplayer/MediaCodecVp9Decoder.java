package com.danielkpl2.webmalphaplayer;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Hardware-accelerated VP9 decoder using Android MediaCodec.
 * Used for dual-stream WebM files where alpha is stored as a second VP9 stream.
 * 
 * This decoder is designed to be used in pairs - one for color, one for alpha.
 * Synchronization is handled by frame index at a higher level.
 */
public class MediaCodecVp9Decoder {
    private static final String TAG = "MediaCodecVp9";
    private static final String MIME_VP9 = "video/x-vnd.on2.vp9";
    
    // Track active decoder instances globally
    private static final AtomicInteger activeDecoderCount = new AtomicInteger(0);
    
    /**
     * Get information about VP9 decoder capabilities on this device.
     * Returns a string with details about max instances, hardware support, etc.
     */
    public static String getVp9DecoderInfo() {
        StringBuilder info = new StringBuilder();
        info.append("=== VP9 Decoder Info ===\n");
        
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
        MediaCodecInfo[] codecInfos = codecList.getCodecInfos();
        
        int vp9DecoderCount = 0;
        
        for (MediaCodecInfo codecInfo : codecInfos) {
            if (codecInfo.isEncoder()) continue;
            
            String[] supportedTypes = codecInfo.getSupportedTypes();
            for (String type : supportedTypes) {
                if (type.equalsIgnoreCase(MIME_VP9)) {
                    vp9DecoderCount++;
                    info.append("\nDecoder: ").append(codecInfo.getName()).append("\n");
                    
                    // Check if hardware accelerated
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        info.append("  Hardware accelerated: ").append(codecInfo.isHardwareAccelerated()).append("\n");
                        info.append("  Software only: ").append(codecInfo.isSoftwareOnly()).append("\n");
                        info.append("  Vendor: ").append(codecInfo.isVendor()).append("\n");
                    }
                    
                    try {
                        MediaCodecInfo.CodecCapabilities caps = codecInfo.getCapabilitiesForType(MIME_VP9);
                        
                        // Get max supported instances (API 23+)
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                            int maxInstances = caps.getMaxSupportedInstances();
                            info.append("  Max concurrent instances: ").append(maxInstances).append("\n");
                            info.append("  --> Max dual-stream players: ").append(maxInstances / 2).append("\n");
                        }
                        
                        // Get supported color formats
                        int[] colorFormats = caps.colorFormats;
                        info.append("  Color formats: ");
                        for (int i = 0; i < Math.min(colorFormats.length, 5); i++) {
                            info.append(colorFormats[i]);
                            if (i < colorFormats.length - 1 && i < 4) info.append(", ");
                        }
                        if (colorFormats.length > 5) info.append("...");
                        info.append("\n");
                        
                        // Video capabilities
                        MediaCodecInfo.VideoCapabilities videoCaps = caps.getVideoCapabilities();
                        if (videoCaps != null) {
                            info.append("  Width range: ").append(videoCaps.getSupportedWidths()).append("\n");
                            info.append("  Height range: ").append(videoCaps.getSupportedHeights()).append("\n");
                        }
                        
                    } catch (Exception e) {
                        info.append("  Error getting capabilities: ").append(e.getMessage()).append("\n");
                    }
                }
            }
        }
        
        info.append("\nTotal VP9 decoders found: ").append(vp9DecoderCount).append("\n");
        info.append("Currently active decoders: ").append(activeDecoderCount.get()).append("\n");
        
        return info.toString();
    }
    
    /**
     * Get the maximum number of concurrent VP9 decoder instances supported.
     * Returns -1 if unable to determine.
     */
    public static int getMaxVp9Instances() {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        MediaCodecInfo[] codecInfos = codecList.getCodecInfos();
        
        for (MediaCodecInfo codecInfo : codecInfos) {
            if (codecInfo.isEncoder()) continue;
            
            String[] supportedTypes = codecInfo.getSupportedTypes();
            for (String type : supportedTypes) {
                if (type.equalsIgnoreCase(MIME_VP9)) {
                    try {
                        MediaCodecInfo.CodecCapabilities caps = codecInfo.getCapabilitiesForType(MIME_VP9);
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                            return caps.getMaxSupportedInstances();
                        }
                    } catch (Exception e) {
                        Log.e(TAG, "Error getting max instances", e);
                    }
                }
            }
        }
        return -1;
    }
    
    /**
     * Get the current number of active decoder instances.
     */
    public static int getActiveDecoderCount() {
        return activeDecoderCount.get();
    }
    
    // Queue sizes
    private static final int MAX_INPUT_QUEUE_SIZE = 30;
    private static final int MAX_OUTPUT_QUEUE_SIZE = 30;
    
    // Timeout for codec operations (microseconds)
    private static final long DEQUEUE_TIMEOUT_US = 10000; // 10ms
    
    private MediaCodec codec;
    private MediaFormat format;
    
    private int width;
    private int height;
    private final String label; // "color" or "alpha" for logging
    
    // State
    private final AtomicBoolean isConfigured = new AtomicBoolean(false);
    private final AtomicBoolean isStarted = new AtomicBoolean(false);
    private final AtomicBoolean isFlushing = new AtomicBoolean(false);
    private final AtomicInteger pendingInputCount = new AtomicInteger(0);
    
    // Decoded frame output
    private final LinkedBlockingQueue<DecodedFrame> outputQueue = new LinkedBlockingQueue<>(MAX_OUTPUT_QUEUE_SIZE);
    
    // Track frame index by presentation time (PTS -> frameIndex)
    // This is needed because MediaCodec processes asynchronously
    private final ConcurrentHashMap<Long, Integer> ptsToFrameIndex = new ConcurrentHashMap<>();
    
    // Background threads for async codec operations
    private HandlerThread inputThread;
    private Handler inputHandler;
    private HandlerThread outputThread;
    private Handler outputHandler;
    
    // Callback for decoded frames (optional, alternative to polling outputQueue)
    private DecodedFrameCallback frameCallback;
    
    /**
     * Represents a decoded YUV frame from MediaCodec.
     * The frame data is copied from the codec's output buffer.
     */
    public static class DecodedFrame {
        public final int frameIndex;
        public final ByteBuffer yPlane;
        public final ByteBuffer uPlane;
        public final ByteBuffer vPlane;
        public final int width;
        public final int height;
        public final int yStride;
        public final int uvStride;
        public final long presentationTimeUs;
        
        public DecodedFrame(int frameIndex, ByteBuffer y, ByteBuffer u, ByteBuffer v,
                          int width, int height, int yStride, int uvStride, long pts) {
            this.frameIndex = frameIndex;
            this.yPlane = y;
            this.uPlane = u;
            this.vPlane = v;
            this.width = width;
            this.height = height;
            this.yStride = yStride;
            this.uvStride = uvStride;
            this.presentationTimeUs = pts;
        }
    }
    
    public interface DecodedFrameCallback {
        void onFrameDecoded(DecodedFrame frame);
        void onDecoderError(String error);
    }
    
    public MediaCodecVp9Decoder(String label) {
        this.label = label;
    }
    
    /**
     * Check if VP9 hardware decoder is available on this device.
     */
    public static boolean isVp9HardwareDecoderAvailable() {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
        for (MediaCodecInfo info : codecList.getCodecInfos()) {
            if (info.isEncoder()) continue;
            
            for (String type : info.getSupportedTypes()) {
                if (MIME_VP9.equalsIgnoreCase(type)) {
                    String name = info.getName();
                    String lowerName = name.toLowerCase();
                    
                    // Skip secure decoders - they require DRM setup
                    if (lowerName.contains(".secure") || lowerName.contains("_secure")) {
                        continue;
                    }
                    
                    // Check if it's hardware-accelerated
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        if (info.isHardwareAccelerated()) {
                            Log.i(TAG, "Found hardware VP9 decoder: " + name);
                            return true;
                        }
                    } else {
                        // Pre-Q: assume hardware if name doesn't contain "sw" or "google"
                        if (!lowerName.contains("sw") && !lowerName.contains("google")) {
                            Log.i(TAG, "Found probable hardware VP9 decoder: " + name);
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }
    
    /**
     * Configure the decoder for the given video dimensions.
     * Must be called before start().
     */
    public boolean configure(int width, int height) {
        if (isConfigured.get()) {
            Log.w(TAG, "[" + label + "] Already configured, releasing first");
            release();
        }
        
        this.width = width;
        this.height = height;
        
        try {
            // Find the best VP9 decoder (prefer hardware)
            String decoderName = findBestVp9Decoder();
            if (decoderName == null) {
                Log.e(TAG, "[" + label + "] No VP9 decoder found!");
                return false;
            }
            
            Log.i(TAG, "[" + label + "] Using VP9 decoder: " + decoderName + " for " + width + "x" + height);
            
            // Check if we have capacity for more decoders
            int maxInstances = getMaxVp9Instances();
            int currentActive = activeDecoderCount.get();
            Log.i(TAG, "[" + label + "] VP9 decoder instances: " + currentActive + "/" + maxInstances + 
                  " (need 2 per dual-stream player)");
            
            if (maxInstances > 0 && currentActive >= maxInstances) {
                Log.w(TAG, "[" + label + "] WARNING: At max VP9 decoder limit! Playback may fail.");
            }
            
            codec = MediaCodec.createByCodecName(decoderName);
            int newCount = activeDecoderCount.incrementAndGet();
            Log.i(TAG, "[" + label + "] Created decoder, active count now: " + newCount);
            
            // Configure format
            format = MediaFormat.createVideoFormat(MIME_VP9, width, height);
            
            // Request YUV420 output
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, 
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            
            // Configure codec without surface (we want raw YUV output)
            codec.configure(format, null, null, 0);
            
            isConfigured.set(true);
            Log.i(TAG, "[" + label + "] Configured successfully");
            return true;
            
        } catch (IOException e) {
            Log.e(TAG, "[" + label + "] Failed to create codec: " + e.getMessage());
            return false;
        } catch (MediaCodec.CodecException e) {
            Log.e(TAG, "[" + label + "] Codec configuration failed: " + e.getMessage());
            return false;
        }
    }
    
    /**
     * Start the decoder. Must be called after configure().
     */
    public boolean start() {
        if (!isConfigured.get()) {
            Log.e(TAG, "[" + label + "] Cannot start - not configured");
            return false;
        }
        
        if (isStarted.get()) {
            Log.w(TAG, "[" + label + "] Already started");
            return true;
        }
        
        try {
            // Start background threads
            inputThread = new HandlerThread("VP9Input-" + label);
            inputThread.start();
            inputHandler = new Handler(inputThread.getLooper());
            
            outputThread = new HandlerThread("VP9Output-" + label);
            outputThread.start();
            outputHandler = new Handler(outputThread.getLooper());
            
            // Start codec
            codec.start();
            isStarted.set(true);
            
            // Start output polling
            startOutputPolling();
            
            Log.i(TAG, "[" + label + "] Started successfully");
            return true;
            
        } catch (MediaCodec.CodecException e) {
            Log.e(TAG, "[" + label + "] Failed to start: " + e.getMessage());
            return false;
        }
    }
    
    /**
     * Queue a VP9 packet for decoding.
     * @param packetData Raw VP9 bitstream data
     * @param frameIndex Frame index for synchronization
     * @param presentationTimeUs Presentation timestamp in microseconds
     * @param isKeyFrame Whether this is a keyframe (for error recovery)
     * @return true if packet was queued successfully
     */
    public boolean queueInputPacket(byte[] packetData, int frameIndex, long presentationTimeUs, boolean isKeyFrame) {
        if (!isStarted.get() || isFlushing.get()) {
            return false;
        }
        
        // Queue on input thread to avoid blocking caller
        pendingInputCount.incrementAndGet();
        inputHandler.post(() -> {
            try {
                doQueueInputPacket(packetData, frameIndex, presentationTimeUs, isKeyFrame);
            } finally {
                pendingInputCount.decrementAndGet();
            }
        });
        
        return true;
    }
    
    private void doQueueInputPacket(byte[] packetData, int frameIndex, long presentationTimeUs, boolean isKeyFrame) {
        if (!isStarted.get() || codec == null || isFlushing.get()) return;
        
        try {
            // Get input buffer with timeout
            int inputBufferIndex = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT_US * 10);
            if (inputBufferIndex < 0) {
                // Timeout or flushing - this is normal during flush
                if (!isFlushing.get()) {
                    Log.w(TAG, "[" + label + "] No input buffer available for frame " + frameIndex);
                }
                return;
            }
            
            // Check again if we're flushing (could have started during dequeue)
            if (isFlushing.get()) return;
            
            ByteBuffer inputBuffer = codec.getInputBuffer(inputBufferIndex);
            if (inputBuffer == null) {
                Log.e(TAG, "[" + label + "] Null input buffer");
                return;
            }
            
            // Copy packet data
            inputBuffer.clear();
            inputBuffer.put(packetData);
            
            // Store frame index mapping for output lookup
            // (MediaCodec preserves presentation time, so we can recover frame index later)
            ptsToFrameIndex.put(presentationTimeUs, frameIndex);
            
            // Queue for decoding
            int flags = isKeyFrame ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
            codec.queueInputBuffer(inputBufferIndex, 0, packetData.length, presentationTimeUs, flags);
            
        } catch (MediaCodec.CodecException e) {
            Log.e(TAG, "[" + label + "] Error queuing packet: " + e.getMessage());
            if (frameCallback != null) {
                frameCallback.onDecoderError("Queue error: " + e.getMessage());
            }
        } catch (IllegalStateException e) {
            // This can happen if codec was flushed while we were waiting
            // for input buffer - just ignore it
            Log.d(TAG, "[" + label + "] Queue cancelled (likely flush): " + e.getMessage());
        }
    }
    
    /**
     * Signal end of stream (no more input packets).
     */
    public void signalEndOfStream() {
        if (!isStarted.get() || codec == null) return;
        
        pendingInputCount.incrementAndGet();
        inputHandler.post(() -> {
            try {
                // Check if flush is in progress - if so, skip EOS signaling
                if (isFlushing.get()) {
                    Log.d(TAG, "[" + label + "] EOS signaling skipped (flushing)");
                    return;
                }
                
                int inputBufferIndex = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT_US * 10);
                if (inputBufferIndex >= 0) {
                    codec.queueInputBuffer(inputBufferIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    Log.i(TAG, "[" + label + "] End of stream signaled");
                }
            } catch (MediaCodec.CodecException e) {
                Log.e(TAG, "[" + label + "] Error signaling EOS: " + e.getMessage());
            } catch (IllegalStateException e) {
                // This can happen if codec was flushed while we were waiting
                Log.d(TAG, "[" + label + "] EOS signaling cancelled (likely flush): " + e.getMessage());
            } finally {
                pendingInputCount.decrementAndGet();
            }
        });
    }
    
    /**
     * Flush the decoder (for seeking or looping).
     * This method blocks until all pending input operations complete.
     */
    public void flush() {
        if (!isStarted.get() || codec == null) return;
        
        isFlushing.set(true);
        
        // Wait for all pending input operations to complete
        int waitCount = 0;
        while (pendingInputCount.get() > 0 && waitCount < 100) {
            try {
                Thread.sleep(5);
                waitCount++;
            } catch (InterruptedException ignored) {}
        }
        
        if (pendingInputCount.get() > 0) {
            Log.w(TAG, "[" + label + "] Flush: still " + pendingInputCount.get() + " pending inputs after wait");
        }
        
        try {
            codec.flush();
            outputQueue.clear();
            ptsToFrameIndex.clear();  // Clear frame index tracking
            // Reset diagnostic counters
            pollCount = 0;
            decodedCount = 0;
            lastPollLogTime = 0;
            // Reset performance counters
            totalProcessTimeNs = 0;
            maxProcessTimeNs = 0;
            processCount = 0;
            lastPerfLogTime = 0;
            Log.i(TAG, "[" + label + "] Flushed");
        } catch (MediaCodec.CodecException e) {
            Log.e(TAG, "[" + label + "] Flush error: " + e.getMessage());
        } catch (IllegalStateException e) {
            Log.e(TAG, "[" + label + "] Flush illegal state: " + e.getMessage());
        } finally {
            isFlushing.set(false);
        }
    }
    
    /**
     * Get the next decoded frame (blocking with timeout).
     * @param timeoutMs Maximum time to wait in milliseconds
     * @return Decoded frame or null if timeout/error
     */
    public DecodedFrame pollDecodedFrame(long timeoutMs) {
        try {
            return outputQueue.poll(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            return null;
        }
    }
    
    /**
     * Get the number of decoded frames available.
     */
    public int getDecodedFrameCount() {
        return outputQueue.size();
    }
    
    /**
     * Set callback for decoded frames (alternative to polling).
     */
    public void setFrameCallback(DecodedFrameCallback callback) {
        this.frameCallback = callback;
    }
    
    /**
     * Release all resources.
     */
    public void release() {
        Log.i(TAG, "[" + label + "] Releasing");
        
        isStarted.set(false);
        isConfigured.set(false);
        
        // Stop threads
        if (inputThread != null) {
            inputThread.quitSafely();
            inputThread = null;
            inputHandler = null;
        }
        
        if (outputThread != null) {
            outputThread.quitSafely();
            outputThread = null;
            outputHandler = null;
        }
        
        // Release codec
        if (codec != null) {
            try {
                codec.stop();
                codec.release();
                int newCount = activeDecoderCount.decrementAndGet();
                Log.i(TAG, "[" + label + "] Released decoder, active count now: " + newCount);
            } catch (Exception e) {
                Log.w(TAG, "[" + label + "] Error releasing codec: " + e.getMessage());
            }
            codec = null;
        }
        
        outputQueue.clear();
        ptsToFrameIndex.clear();
        frameCallback = null;
        
        Log.i(TAG, "[" + label + "] Released");
    }
    
    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------
    
    private String findBestVp9Decoder() {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
        
        String hardwareDecoder = null;
        String softwareDecoder = null;
        
        for (MediaCodecInfo info : codecList.getCodecInfos()) {
            if (info.isEncoder()) continue;
            
            for (String type : info.getSupportedTypes()) {
                if (MIME_VP9.equalsIgnoreCase(type)) {
                    String name = info.getName();
                    String lowerName = name.toLowerCase();
                    
                    // Skip secure decoders - they require DRM setup (crypto/descrambler)
                    // and will fail with "Crypto or descrambler should be given for secure codec"
                    if (lowerName.contains(".secure") || lowerName.contains("_secure")) {
                        Log.d(TAG, "Skipping secure decoder: " + name);
                        continue;
                    }
                    
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        if (info.isHardwareAccelerated()) {
                            // Prefer c2 codecs over OMX codecs (newer, more stable)
                            if (hardwareDecoder == null || lowerName.startsWith("c2.")) {
                                hardwareDecoder = name;
                                Log.d(TAG, "Found hardware VP9 decoder: " + name);
                            }
                        } else if (info.isSoftwareOnly()) {
                            if (softwareDecoder == null) {
                                softwareDecoder = name;
                                Log.d(TAG, "Found software VP9 decoder: " + name);
                            }
                        }
                    } else {
                        // Pre-Q heuristic
                        if (!lowerName.contains("sw") && !lowerName.contains("google")) {
                            if (hardwareDecoder == null || lowerName.startsWith("c2.")) {
                                hardwareDecoder = name;
                                Log.d(TAG, "Found probable hardware VP9 decoder: " + name);
                            }
                        } else {
                            if (softwareDecoder == null) {
                                softwareDecoder = name;
                                Log.d(TAG, "Found software VP9 decoder: " + name);
                            }
                        }
                    }
                }
            }
        }
        
        String selected = hardwareDecoder != null ? hardwareDecoder : softwareDecoder;
        Log.i(TAG, "Selected VP9 decoder: " + selected + " (hw=" + hardwareDecoder + ", sw=" + softwareDecoder + ")");
        
        // Prefer hardware
        return selected;
    }
    
    private void startOutputPolling() {
        if (outputHandler == null) return;
        
        outputHandler.post(this::pollOutput);
    }
    
    // Diagnostic counters for tracking decoder throughput
    private volatile int pollCount = 0;
    private volatile int decodedCount = 0;
    private volatile long lastPollLogTime = 0;
    
    // Decode performance tracking
    private volatile long totalProcessTimeNs = 0;
    private volatile long maxProcessTimeNs = 0;
    private volatile int processCount = 0;
    private volatile long lastPerfLogTime = 0;
    
    private void pollOutput() {
        if (!isStarted.get() || codec == null || isFlushing.get()) {
            // Reschedule if still running
            if (isStarted.get() && outputHandler != null) {
                outputHandler.postDelayed(this::pollOutput, 5);
            }
            return;
        }
        
        pollCount++;
        
        try {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            // Use longer timeout for large frames (1080p decoding is slow in emulator)
            int outputBufferIndex = codec.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT_US * 5);
            
            if (outputBufferIndex >= 0) {
                // Got decoded frame
                decodedCount++;
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    Log.i(TAG, "[" + label + "] End of stream received");
                    codec.releaseOutputBuffer(outputBufferIndex, false);
                } else if (info.size > 0) {
                    processOutputBuffer(outputBufferIndex, info);
                } else {
                    codec.releaseOutputBuffer(outputBufferIndex, false);
                }
            } else if (outputBufferIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat newFormat = codec.getOutputFormat();
                Log.i(TAG, "[" + label + "] Output format changed: " + newFormat);
            } else if (outputBufferIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                // Log periodically when no output available
                long now = System.currentTimeMillis();
                if (now - lastPollLogTime > 2000) {
                    Log.d(TAG, "[" + label + "] Output poll: no output available (polls=" + pollCount + 
                          ", decoded=" + decodedCount + ", pending=" + pendingInputCount.get() + ")");
                    lastPollLogTime = now;
                }
            }
            
        } catch (MediaCodec.CodecException e) {
            Log.e(TAG, "[" + label + "] Output poll error: " + e.getMessage());
            // Log diagnostic state
            Log.e(TAG, "[" + label + "] State: polls=" + pollCount + ", decoded=" + decodedCount);
        } catch (IllegalStateException e) {
            // Can happen during flush - ignore
            Log.d(TAG, "[" + label + "] Output poll cancelled (likely flush)");
        }
        
        // Continue polling
        if (isStarted.get() && outputHandler != null) {
            outputHandler.post(this::pollOutput);
        }
    }
    
    private void processOutputBuffer(int bufferIndex, MediaCodec.BufferInfo info) {
        if (isFlushing.get()) {
            // Don't process during flush
            try {
                codec.releaseOutputBuffer(bufferIndex, false);
            } catch (Exception ignored) {}
            return;
        }
        
        long processStartNs = System.nanoTime();
        android.media.Image image = null;
        boolean bufferReleased = false;
        
        try {
            // Get output image
            image = codec.getOutputImage(bufferIndex);
            if (image == null) {
                Log.w(TAG, "[" + label + "] Null output image at buffer " + bufferIndex);
                codec.releaseOutputBuffer(bufferIndex, false);
                bufferReleased = true;
                return;
            }
            
            // Extract YUV planes
            android.media.Image.Plane[] planes = image.getPlanes();
            if (planes.length < 3) {
                Log.e(TAG, "[" + label + "] Unexpected plane count: " + planes.length);
                image.close();
                image = null;
                codec.releaseOutputBuffer(bufferIndex, false);
                bufferReleased = true;
                return;
            }
            
            int frameWidth = image.getWidth();
            int frameHeight = image.getHeight();
            
            // Copy Y plane
            ByteBuffer yBuffer = planes[0].getBuffer();
            int yStride = planes[0].getRowStride();
            ByteBuffer yCopy = ByteBuffer.allocateDirect(frameWidth * frameHeight);
            copyPlane(yBuffer, yStride, yCopy, frameWidth, frameHeight);
            
            // Copy U plane (half resolution)
            ByteBuffer uBuffer = planes[1].getBuffer();
            int uvStride = planes[1].getRowStride();
            int uvPixelStride = planes[1].getPixelStride();
            int uvWidth = frameWidth / 2;
            int uvHeight = frameHeight / 2;
            ByteBuffer uCopy = ByteBuffer.allocateDirect(uvWidth * uvHeight);
            copyPlaneWithPixelStride(uBuffer, uvStride, uvPixelStride, uCopy, uvWidth, uvHeight);
            
            // Copy V plane
            ByteBuffer vBuffer = planes[2].getBuffer();
            ByteBuffer vCopy = ByteBuffer.allocateDirect(uvWidth * uvHeight);
            copyPlaneWithPixelStride(vBuffer, uvStride, uvPixelStride, vCopy, uvWidth, uvHeight);
            
            // Look up frame index from the PTS mapping
            // This is more reliable than calculating from PTS because it handles any frame rate
            Integer mappedFrameIndex = ptsToFrameIndex.remove(info.presentationTimeUs);
            int frameIndex;
            if (mappedFrameIndex != null) {
                frameIndex = mappedFrameIndex;
            } else {
                // Fallback: calculate from PTS (shouldn't happen normally)
                Log.w(TAG, "[" + label + "] Frame index not found for PTS " + info.presentationTimeUs + ", using fallback calculation");
                frameIndex = (int)(info.presentationTimeUs * 30 / 1_000_000);
            }
            
            // Create decoded frame
            DecodedFrame frame = new DecodedFrame(
                frameIndex,
                yCopy, uCopy, vCopy,
                frameWidth, frameHeight,
                frameWidth, uvWidth,
                info.presentationTimeUs
            );
            
            // Release image and buffer BEFORE delivering frame
            image.close();
            image = null;
            codec.releaseOutputBuffer(bufferIndex, false);
            bufferReleased = true;
            
            // Track processing time
            long processEndNs = System.nanoTime();
            long processTimeNs = processEndNs - processStartNs;
            totalProcessTimeNs += processTimeNs;
            maxProcessTimeNs = Math.max(maxProcessTimeNs, processTimeNs);
            processCount++;
            
            // Log decode performance periodically
            long now = System.currentTimeMillis();
            if (now - lastPerfLogTime >= 2000 && processCount > 0) {
                double avgProcessMs = totalProcessTimeNs / 1_000_000.0 / processCount;
                double maxProcessMs = maxProcessTimeNs / 1_000_000.0;
                Log.i(TAG, String.format("[%s] ⏱ DECODE: avgProcess=%.1fms, maxProcess=%.1fms, decoded=%d", 
                      label, avgProcessMs, maxProcessMs, processCount));
                lastPerfLogTime = now;
            }
            
            // Deliver frame (after releasing codec resources)
            if (frameCallback != null) {
                frameCallback.onFrameDecoded(frame);
            } else {
                // Queue for polling
                if (!outputQueue.offer(frame)) {
                    Log.w(TAG, "[" + label + "] Output queue full, dropping frame " + frameIndex);
                }
            }
            
        } catch (IllegalStateException e) {
            // Codec was flushed while processing - ignore
            Log.d(TAG, "[" + label + "] Output processing cancelled (flush)");
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] Error processing output: " + e.getMessage());
            e.printStackTrace();
        } finally {
            // Ensure resources are always released
            if (image != null) {
                try {
                    image.close();
                } catch (Exception ignored) {}
            }
            if (!bufferReleased) {
                try {
                    codec.releaseOutputBuffer(bufferIndex, false);
                } catch (Exception ignored) {}
            }
        }
    }
    
    private void copyPlane(ByteBuffer src, int srcStride, ByteBuffer dst, int width, int height) {
        src.rewind();
        dst.clear();
        
        if (srcStride == width) {
            // Direct copy
            int size = width * height;
            for (int i = 0; i < size; i++) {
                dst.put(src.get());
            }
        } else {
            // Row by row
            for (int row = 0; row < height; row++) {
                src.position(row * srcStride);
                for (int col = 0; col < width; col++) {
                    dst.put(src.get());
                }
            }
        }
        dst.rewind();
    }
    
    private void copyPlaneWithPixelStride(ByteBuffer src, int rowStride, int pixelStride, 
                                          ByteBuffer dst, int width, int height) {
        src.rewind();
        dst.clear();
        
        if (pixelStride == 1) {
            copyPlane(src, rowStride, dst, width, height);
        } else {
            // Handle interleaved UV (NV12/NV21)
            for (int row = 0; row < height; row++) {
                int rowStart = row * rowStride;
                for (int col = 0; col < width; col++) {
                    src.position(rowStart + col * pixelStride);
                    dst.put(src.get());
                }
            }
        }
        dst.rewind();
    }
}
