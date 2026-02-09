package com.danielkpl2.webmalphaplayer;

import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Hardware-accelerated VP9 decoder that outputs directly to a SurfaceTexture.
 * 
 * This keeps decoded frames on the GPU - no CPU readback required!
 * The SurfaceTexture provides an OES texture that can be used directly in OpenGL.
 * 
 * Usage:
 * 1. Create decoder on GL thread (so OES texture is created in right context)
 * 2. Call configure() with video dimensions
 * 3. Queue compressed VP9 packets with queueInputPacket()
 * 4. When frame is ready, onFrameAvailable callback is invoked
 * 5. Call updateTexImage() on GL thread to make frame available as OES texture
 * 6. Use getOesTextureId() in your shader
 */
public class SurfaceTextureVp9Decoder implements SurfaceTexture.OnFrameAvailableListener {
    private static final String TAG = "SurfaceVp9Dec";
    private static final String MIME_VP9 = "video/x-vnd.on2.vp9";
    
    // Timeout for codec operations (microseconds)
    private static final long DEQUEUE_TIMEOUT_US = 0; // Non-blocking for async
    private static final long INPUT_TIMEOUT_US = 10000; // 10ms for input
    
    private final String label;
    private MediaCodec codec;
    private MediaFormat format;
    private Surface outputSurface;
    private SurfaceTexture surfaceTexture;
    private int oesTextureId = -1;
    
    private int width;
    private int height;
    
    // State
    private final AtomicBoolean isConfigured = new AtomicBoolean(false);
    private final AtomicBoolean isStarted = new AtomicBoolean(false);
    private final AtomicBoolean isReleased = new AtomicBoolean(false);
    private final AtomicBoolean hasNewFrame = new AtomicBoolean(false);
    
    // Frame tracking - use counter instead of FIFO queue for reliable sync
    // The FIFO queue approach fails when decoders output at different speeds
    private final AtomicInteger nextOutputFrameIndex = new AtomicInteger(0);
    private final AtomicInteger lastRenderedFrameIndex = new AtomicInteger(-1);
    private final AtomicLong lastFrameTimestampNs = new AtomicLong(0);
    
    // PTS-gated release: decoded frames are held in MediaCodec output buffers
    // until the playback clock advances targetPresentationTimeUs past their PTS.
    // This prevents SurfaceTexture from being flooded with frames faster than
    // the render loop can consume them, which would cause frame skipping.
    private final AtomicLong targetPresentationTimeUs = new AtomicLong(0);
    
    // Tracks whether a frame has been released since the last updateTexImage().
    // This lets consumeFrames() know if updateTexImage() will produce a new frame.
    private final AtomicBoolean frameReleasedSinceConsume = new AtomicBoolean(false);
    
    // Keep queue for PTS tracking (but don't use it for frame index)
    private final ConcurrentLinkedQueue<FrameInfo> pendingFrames = new ConcurrentLinkedQueue<>();
    
    // Callback for frame availability
    private FrameAvailableCallback frameCallback;
    
    // Codec thread for async operations
    private HandlerThread codecThread;
    private Handler codecHandler;
    
    // Verbose per-frame logging toggle
    private volatile boolean verbose = false;
    
    // Track active decoders globally
    private static final AtomicInteger activeDecoderCount = new AtomicInteger(0);
    
    // Lock for synchronizing pendingFrames access between queueInputPacket and flush
    private final Object pendingFramesLock = new Object();
    
    /**
     * Frame info for tracking in-flight frames
     */
    private static class FrameInfo {
        final int frameIndex;
        final long presentationTimeUs;
        
        FrameInfo(int frameIndex, long pts) {
            this.frameIndex = frameIndex;
            this.presentationTimeUs = pts;
        }
    }
    
    /**
     * Callback when a new frame is available on the SurfaceTexture.
     * Called on an arbitrary thread - you should sync to GL thread before updateTexImage().
     */
    public interface FrameAvailableCallback {
        void onFrameAvailable(SurfaceTextureVp9Decoder decoder, int frameIndex, long timestampNs);
        void onDecoderError(SurfaceTextureVp9Decoder decoder, String error);
    }
    
    public SurfaceTextureVp9Decoder(String label) {
        this.label = label;
    }
    
    /**
     * Get the maximum number of concurrent VP9 decoder instances.
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
    
    public static int getActiveDecoderCount() {
        return activeDecoderCount.get();
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
     * Get detailed information about available VP9 decoders.
     */
    public static String getVp9DecoderInfo() {
        StringBuilder info = new StringBuilder();
        info.append("VP9 Decoder Information\n");
        info.append("=======================\n");
        
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
     * Configure the decoder. Must be called from GL thread so OES texture is created
     * in the correct EGL context.
     */
    public boolean configure(int width, int height) {
        if (isConfigured.get()) {
            Log.w(TAG, "[" + label + "] Already configured");
            return true;
        }
        
        if (isReleased.get()) {
            Log.e(TAG, "[" + label + "] Cannot configure - decoder released");
            return false;
        }
        
        this.width = width;
        this.height = height;
        
        try {
            // Create OES texture for SurfaceTexture
            int[] textures = new int[1];
            GLES20.glGenTextures(1, textures, 0);
            oesTextureId = textures[0];
            
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            
            Log.i(TAG, "[" + label + "] Created OES texture: " + oesTextureId);
            
            // Create SurfaceTexture attached to our OES texture
            surfaceTexture = new SurfaceTexture(oesTextureId);
            surfaceTexture.setDefaultBufferSize(width, height);
            surfaceTexture.setOnFrameAvailableListener(this);
            
            // Create Surface for MediaCodec output
            outputSurface = new Surface(surfaceTexture);
            
            // Find best VP9 decoder
            String decoderName = findBestVp9Decoder();
            if (decoderName == null) {
                Log.e(TAG, "[" + label + "] No VP9 decoder found!");
                return false;
            }
            
            int maxInstances = getMaxVp9Instances();
            int currentActive = activeDecoderCount.get();
            Log.i(TAG, "[" + label + "] VP9 decoder instances: " + currentActive + "/" + maxInstances);
            
            if (maxInstances > 0 && currentActive >= maxInstances) {
                Log.w(TAG, "[" + label + "] WARNING: At max VP9 decoder limit!");
            }
            
            Log.i(TAG, "[" + label + "] Using VP9 decoder: " + decoderName + " for " + width + "x" + height);
            codec = MediaCodec.createByCodecName(decoderName);
            activeDecoderCount.incrementAndGet();
            
            // Configure format
            format = MediaFormat.createVideoFormat(MIME_VP9, width, height);
            
            // Configure codec WITH Surface (keeps decoded frames on GPU!)
            codec.configure(format, outputSurface, null, 0);
            
            // Start codec thread
            codecThread = new HandlerThread("Codec-" + label);
            codecThread.start();
            codecHandler = new Handler(codecThread.getLooper());
            
            isConfigured.set(true);
            Log.i(TAG, "[" + label + "] Configured with Surface output (GPU-accelerated)");
            return true;
            
        } catch (IOException e) {
            Log.e(TAG, "[" + label + "] Failed to create codec: " + e.getMessage());
            cleanup();
            return false;
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] Configuration error: " + e.getMessage());
            cleanup();
            return false;
        }
    }
    
    /**
     * Start the decoder.
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
            codec.start();
            isStarted.set(true);
            
            // Start output polling
            codecHandler.post(this::pollOutput);
            
            Log.i(TAG, "[" + label + "] Started");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] Failed to start: " + e.getMessage());
            return false;
        }
    }
    
    /**
     * Queue a compressed VP9 packet for decoding.
     * 
     * @param data Compressed VP9 data
     * @param offset Offset in data array
     * @param size Size of data
     * @param presentationTimeUs Presentation timestamp in microseconds
     * @param frameIndex Frame index for tracking
     * @return true if queued successfully
     */
    public boolean queueInputPacket(byte[] data, int offset, int size, long presentationTimeUs, int frameIndex) {
        // Use lock to synchronize with flush() - prevents race condition where
        // pendingFrames.offer() runs after flush()'s pendingFrames.clear()
        synchronized (pendingFramesLock) {
            if (!isStarted.get() || isReleased.get()) {
                return false;
            }
            
            try {
                int inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
                if (inputIndex < 0) {
                    // No buffer available
                    return false;
                }
                
                ByteBuffer inputBuffer = codec.getInputBuffer(inputIndex);
                if (inputBuffer == null) {
                    return false;
                }
                
                inputBuffer.clear();
                inputBuffer.put(data, offset, size);
                
                // Track this frame - synchronized with flush to prevent race condition
                pendingFrames.offer(new FrameInfo(frameIndex, presentationTimeUs));
                
                codec.queueInputBuffer(inputIndex, 0, size, presentationTimeUs, 0);
                return true;
                
            } catch (Exception e) {
                Log.e(TAG, "[" + label + "] Queue error: " + e.getMessage());
                return false;
            }
        }
    }
    
    /**
     * Queue a compressed VP9 packet from ByteBuffer.
     */
    public boolean queueInputPacket(ByteBuffer data, long presentationTimeUs, int frameIndex) {
        // Use lock to synchronize with flush() - prevents race condition where
        // pendingFrames.offer() runs after flush()'s pendingFrames.clear()
        synchronized (pendingFramesLock) {
            if (!isStarted.get() || isReleased.get()) {
                return false;
            }
            
            try {
                int inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
                if (inputIndex < 0) {
                    return false;
                }
                
                ByteBuffer inputBuffer = codec.getInputBuffer(inputIndex);
                if (inputBuffer == null) {
                    return false;
                }
                
                inputBuffer.clear();
                data.rewind();
                int size = data.remaining();  // Get size BEFORE put!
                inputBuffer.put(data);
                data.rewind();
                
                if (verbose) Log.d(TAG, "[" + label + "] Queued frame " + frameIndex + ", size=" + size + ", pts=" + presentationTimeUs);
                
                // Track this frame - synchronized with flush to prevent race condition
                pendingFrames.offer(new FrameInfo(frameIndex, presentationTimeUs));
                
                codec.queueInputBuffer(inputIndex, 0, size, presentationTimeUs, 0);
                return true;
                
            } catch (Exception e) {
                Log.e(TAG, "[" + label + "] Queue error: " + e.getMessage());
                return false;
            }
        }
    }
    
    /**
     * Poll for decoded output and release to Surface.
     * 
     * CRITICAL: Frames are only released to the SurfaceTexture when their PTS
     * is <= targetPresentationTimeUs. This ensures the SurfaceTexture receives
     * exactly one frame per render cycle, preventing frame skipping caused by
     * SurfaceTexture always returning only the latest frame via updateTexImage().
     * 
     * Holding output buffers creates natural backpressure on MediaCodec, which
     * throttles the decode pipeline to match the playback rate.
     */
    private void pollOutput() {
        if (isReleased.get() || !isStarted.get()) {
            return;
        }
        
        try {
            MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
            
            while (!isReleased.get() && isStarted.get()) {
                // CRITICAL: Only release ONE frame per render cycle.
                // If we already released a frame that hasn't been consumed by
                // updateTexImage() yet, don't release another — it would overwrite
                // the current frame in SurfaceTexture, and the two decoders could
                // end up showing different temporal frames (color/alpha mismatch).
                if (frameReleasedSinceConsume.get()) {
                    break; // Wait for consumeFrames() to clear this flag
                }
                
                int outputIndex = codec.dequeueOutputBuffer(bufferInfo, DEQUEUE_TIMEOUT_US);
                
                if (outputIndex >= 0) {
                    long pts = bufferInfo.presentationTimeUs;
                    long target = targetPresentationTimeUs.get();
                    
                    if (pts <= target) {
                        // This frame is due or past due — release to SurfaceTexture
                        codec.releaseOutputBuffer(outputIndex, true);
                        frameReleasedSinceConsume.set(true);
                        
                        if (verbose) Log.d(TAG, "[" + label + "] Released frame, pts=" + pts + 
                              ", target=" + target + ", flags=" + bufferInfo.flags);
                        
                        // Only ONE frame per cycle — break after releasing
                        break;
                    } else {
                        // This frame is ahead of the playback clock — hold it
                        heldOutputIndex = outputIndex;
                        heldOutputPts = pts;
                        heldOutputFlags = bufferInfo.flags;
                        break;
                    }
                    
                } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat newFormat = codec.getOutputFormat();
                    Log.i(TAG, "[" + label + "] Output format changed: " + newFormat);
                    
                } else if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                    break;
                }
            }
        } catch (IllegalStateException e) {
            if (!isReleased.get()) {
                Log.w(TAG, "[" + label + "] Poll exception: " + e.getMessage());
            }
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] Poll error: " + e.getMessage());
            if (frameCallback != null) {
                frameCallback.onDecoderError(this, e.getMessage());
            }
        }
        
        // Schedule next poll — check held buffer or dequeue new
        if (!isReleased.get() && isStarted.get()) {
            if (heldOutputIndex >= 0) {
                codecHandler.postDelayed(this::checkHeldBuffer, 1);
            } else {
                codecHandler.postDelayed(this::pollOutput, 1);
            }
        }
    }
    
    // Held output buffer state (only accessed from codec thread)
    private int heldOutputIndex = -1;
    private long heldOutputPts = 0;
    private int heldOutputFlags = 0;
    
    /**
     * Check if a held output buffer can now be released.
     */
    private void checkHeldBuffer() {
        if (isReleased.get() || !isStarted.get()) {
            if (heldOutputIndex >= 0) {
                try { codec.releaseOutputBuffer(heldOutputIndex, false); } catch (Exception ignored) {}
                heldOutputIndex = -1;
            }
            return;
        }
        
        if (heldOutputIndex >= 0) {
            // Don't release if a previous frame hasn't been consumed yet
            if (frameReleasedSinceConsume.get()) {
                // Wait for consumeFrames() to clear the flag
                if (!isReleased.get() && isStarted.get()) {
                    codecHandler.postDelayed(this::checkHeldBuffer, 1);
                }
                return;
            }
            
            long target = targetPresentationTimeUs.get();
            if (heldOutputPts <= target) {
                // Target has caught up — release to SurfaceTexture
                try {
                    codec.releaseOutputBuffer(heldOutputIndex, true);
                    frameReleasedSinceConsume.set(true);
                    
                    if (verbose) Log.d(TAG, "[" + label + "] Released held frame, pts=" + heldOutputPts + 
                          ", target=" + target);
                } catch (Exception e) {
                    Log.w(TAG, "[" + label + "] Error releasing held buffer: " + e.getMessage());
                }
                heldOutputIndex = -1;
                
                // Don't immediately poll for more — wait for consume first
                // Schedule check to resume polling after consume clears the flag
                if (!isReleased.get() && isStarted.get()) {
                    codecHandler.postDelayed(this::pollOutput, 1);
                }
            } else {
                if (!isReleased.get() && isStarted.get()) {
                    codecHandler.postDelayed(this::checkHeldBuffer, 1);
                }
            }
        } else {
            if (!isReleased.get() && isStarted.get()) {
                codecHandler.post(this::pollOutput);
            }
        }
    }
    
    /**
     * Called by SurfaceTexture when a new frame is available.
     * 
     * IMPORTANT: We use the frame index from pendingFrames queue (which contains the
     * actual frame index passed to queueInputPacket) instead of an independent counter.
     * 
     * MediaCodec guarantees frames are output in FIFO order for each decoder instance.
     * Using the queued frame index ensures color and alpha decoders report matching
     * frame indices for frames with the same presentation timestamp.
     */
    @Override
    public void onFrameAvailable(SurfaceTexture surfaceTexture) {
        if (isReleased.get()) return;
        
        hasNewFrame.set(true);
        
        // Get the frame info from the queue - this contains the ACTUAL frame index
        // that was passed to queueInputPacket. MediaCodec outputs frames in FIFO order,
        // so the poll() will return the correct frame info for this output.
        FrameInfo frameInfo = pendingFrames.poll();
        int frameIndex;
        if (frameInfo != null) {
            frameIndex = frameInfo.frameIndex;
        } else {
            // Fallback if queue is empty (shouldn't happen in normal operation)
            frameIndex = nextOutputFrameIndex.getAndIncrement();
            Log.w(TAG, "[" + label + "] ⚠️ pendingFrames empty! Using counter fallback: " + frameIndex);
        }
        
        long timestampNs = surfaceTexture.getTimestamp();
        
        if (verbose) Log.d(TAG, "[" + label + "] onFrameAvailable: frameIndex=" + frameIndex + ", ts=" + timestampNs);
        
        lastFrameTimestampNs.set(timestampNs);
        lastRenderedFrameIndex.set(frameIndex);
        
        if (frameCallback != null) {
            frameCallback.onFrameAvailable(this, frameIndex, timestampNs);
        }
    }
    
    /**
     * Update the OES texture with the latest frame.
     * MUST be called from GL thread!
     * 
     * IMPORTANT: Always call surfaceTexture.updateTexImage() when processing
     * an onFrameAvailable callback, regardless of hasNewFrame state.
     * The hasNewFrame flag can miss frames if multiple arrive before processing.
     */
    public void updateTexImage() {
        if (surfaceTexture != null) {
            // Clear the flag (for hasNewFrame() queries)
            boolean wasNew = hasNewFrame.getAndSet(false);
            try {
                // ALWAYS call updateTexImage when this method is invoked
                // from onFrameAvailable processing. The SurfaceTexture holds
                // the latest frame and we must consume it.
                surfaceTexture.updateTexImage();
                if (!wasNew) {
                    // This indicates we called updateTexImage when hasNewFrame was false
                    // which suggests a timing issue - log it for diagnosis
                    Log.w(TAG, "[" + label + "] updateTexImage called with hasNewFrame=false (timing issue?)");
                }
            } catch (Exception e) {
                Log.w(TAG, "[" + label + "] updateTexImage error: " + e.getMessage());
            }
        }
    }
    
    /**
     * Check if a new frame is available.
     */
    public boolean hasNewFrame() {
        return hasNewFrame.get();
    }
    
    /**
     * Get the OES texture ID for use in shader.
     */
    public int getOesTextureId() {
        return oesTextureId;
    }
    
    /**
     * Get the transformation matrix from SurfaceTexture.
     * Use this in your vertex shader for proper texture coordinates.
     */
    public void getTransformMatrix(float[] matrix) {
        if (surfaceTexture != null) {
            surfaceTexture.getTransformMatrix(matrix);
        }
    }
    
    /**
     * Get the timestamp of the last rendered frame in nanoseconds.
     */
    public long getLastFrameTimestampNs() {
        return lastFrameTimestampNs.get();
    }
    
    /**
     * Get the index of the last rendered frame.
     */
    public int getLastRenderedFrameIndex() {
        return lastRenderedFrameIndex.get();
    }
    
    /**
     * Set the callback for frame availability.
     */
    public void setFrameCallback(FrameAvailableCallback callback) {
        this.frameCallback = callback;
    }
    
    /**
     * Enable/disable verbose per-frame logging.
     */
    public void setVerbose(boolean verbose) {
        this.verbose = verbose;
    }
    
    /**
     * Set the target presentation time. Decoded frames with PTS <= this value
     * will be released to the SurfaceTexture. Frames ahead of this will be held
     * in MediaCodec output buffers, creating natural backpressure.
     * 
     * Called by the playback loop as the clock advances.
     */
    public void setTargetPresentationTimeUs(long ptsUs) {
        targetPresentationTimeUs.set(ptsUs);
    }
    
    /**
     * Check if a frame has been released to SurfaceTexture since the last call to
     * clearFrameReleased(). Used by consumeFrames() to know if updateTexImage()
     * will produce a new frame.
     */
    public boolean wasFrameReleased() {
        return frameReleasedSinceConsume.get();
    }
    
    /**
     * Clear the frame-released flag. Called after updateTexImage().
     */
    public void clearFrameReleased() {
        frameReleasedSinceConsume.set(false);
    }
    
    /**
     * Flush the decoder (for seeking/looping).
     * After flush, the codec is ready to accept new input immediately.
     */
    public void flush() {
        if (isReleased.get()) {
            Log.w(TAG, "[" + label + "] flush() called but already released");
            return;
        }
        
        Log.i(TAG, "[" + label + "] Flushing decoder...");
        
        try {
            // CRITICAL: Acquire lock first to prevent race condition with queueInputPacket
            // This ensures no items are added to pendingFrames during or after clear()
            synchronized (pendingFramesLock) {
                // Stop accepting new input INSIDE the lock
                isStarted.set(false);
                
                // Clear tracking state BEFORE codec flush
                pendingFrames.clear();
                nextOutputFrameIndex.set(0);  // Reset counter for next playback cycle
                lastRenderedFrameIndex.set(-1);
                hasNewFrame.set(false);
                lastFrameTimestampNs.set(0);
                targetPresentationTimeUs.set(0);
                frameReleasedSinceConsume.set(false);
                heldOutputIndex = -1; // Will be invalid after flush anyway
            }
            
            // Small delay to let pollOutput() exit its loop (outside lock to avoid blocking)
            try { Thread.sleep(10); } catch (InterruptedException ignored) {}
            
            // Now safe to flush the codec
            codec.flush();
            
            // Re-enable and restart polling
            synchronized (pendingFramesLock) {
                isStarted.set(true);
            }
            
            if (codecHandler != null) {
                codecHandler.post(this::pollOutput);
            }
            
            Log.i(TAG, "[" + label + "] Flushed and restarted successfully");
        } catch (IllegalStateException e) {
            Log.e(TAG, "[" + label + "] Flush failed (illegal state): " + e.getMessage());
            // Try to recover by restarting
            synchronized (pendingFramesLock) {
                isStarted.set(true);
            }
            if (codecHandler != null) {
                codecHandler.post(this::pollOutput);
            }
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] Flush error: " + e.getMessage());
            synchronized (pendingFramesLock) {
                isStarted.set(true);
            }
        }
    }
    
    /**
     * Release the decoder and all resources.
     */
    public void release() {
        if (isReleased.getAndSet(true)) {
            return;
        }
        
        Log.i(TAG, "[" + label + "] Releasing");
        
        isStarted.set(false);
        isConfigured.set(false);
        
        cleanup();
        
        Log.i(TAG, "[" + label + "] Released");
    }
    
    private void cleanup() {
        // Stop codec thread
        if (codecThread != null) {
            codecThread.quitSafely();
            codecThread = null;
            codecHandler = null;
        }
        
        // Release codec
        if (codec != null) {
            try {
                codec.stop();
                codec.release();
                activeDecoderCount.decrementAndGet();
            } catch (Exception e) {
                Log.w(TAG, "[" + label + "] Codec cleanup error: " + e.getMessage());
            }
            codec = null;
        }
        
        // Release Surface
        if (outputSurface != null) {
            outputSurface.release();
            outputSurface = null;
        }
        
        // Release SurfaceTexture
        if (surfaceTexture != null) {
            surfaceTexture.setOnFrameAvailableListener(null);
            surfaceTexture.release();
            surfaceTexture = null;
        }
        
        // Delete OES texture
        if (oesTextureId > 0) {
            int[] textures = {oesTextureId};
            GLES20.glDeleteTextures(1, textures, 0);
            oesTextureId = -1;
        }
        
        pendingFrames.clear();
        frameCallback = null;
    }
    
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public String getLabel() { return label; }
    public boolean isConfigured() { return isConfigured.get(); }
    public boolean isStarted() { return isStarted.get(); }
    public boolean isReleased() { return isReleased.get(); }
    
    /**
     * Find the best VP9 decoder (prefer hardware).
     */
    private String findBestVp9Decoder() {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        MediaCodecInfo[] codecInfos = codecList.getCodecInfos();
        
        String hwDecoder = null;
        String swDecoder = null;
        
        for (MediaCodecInfo codecInfo : codecInfos) {
            if (codecInfo.isEncoder()) continue;
            
            String[] supportedTypes = codecInfo.getSupportedTypes();
            for (String type : supportedTypes) {
                if (type.equalsIgnoreCase(MIME_VP9)) {
                    String name = codecInfo.getName();
                    
                    // Skip secure decoders
                    if (name.contains(".secure")) continue;
                    
                    // Check if hardware accelerated
                    boolean isHardware = false;
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        isHardware = codecInfo.isHardwareAccelerated();
                    } else {
                        isHardware = !name.toLowerCase().contains("sw") && 
                                    !name.toLowerCase().contains("google") &&
                                    !name.toLowerCase().contains("android");
                    }
                    
                    if (isHardware) {
                        if (hwDecoder == null) {
                            hwDecoder = name;
                            Log.d(TAG, "Found hardware VP9 decoder: " + name);
                        }
                    } else {
                        if (swDecoder == null) {
                            swDecoder = name;
                            Log.d(TAG, "Found software VP9 decoder: " + name);
                        }
                    }
                }
            }
        }
        
        // Prefer hardware decoder
        String selected = hwDecoder != null ? hwDecoder : swDecoder;
        Log.i(TAG, "Selected VP9 decoder: " + selected + " (hw=" + hwDecoder + ", sw=" + swDecoder + ")");
        return selected;
    }
}
