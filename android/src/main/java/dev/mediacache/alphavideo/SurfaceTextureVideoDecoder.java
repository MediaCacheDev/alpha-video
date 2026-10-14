package dev.mediacache.alphavideo;

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
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Hardware-accelerated video decoder (VP9/AV1) that outputs directly to a SurfaceTexture.
 * 
 * This keeps decoded frames on the GPU - no CPU readback required!
 * The SurfaceTexture provides an OES texture that can be used directly in OpenGL.
 * 
 * Usage:
 * 1. Create decoder on GL thread (so OES texture is created in right context)
 * 2. Call configure() with video dimensions
 * 3. Queue compressed packets with queueInputPacket()
 * 4. When frame is ready, onFrameAvailable callback is invoked
 * 5. Call updateTexImage() on GL thread to make frame available as OES texture
 * 6. Use getOesTextureId() in your shader
 */
public class SurfaceTextureVideoDecoder implements SurfaceTexture.OnFrameAvailableListener {
    private static final String TAG = "SurfaceVideoDec";
    public static final String MIME_VP9 = "video/x-vnd.on2.vp9";
    public static final String MIME_AV1 = "video/av01";
    
    // Timeout for codec operations (microseconds)
    private static final long DEQUEUE_TIMEOUT_US = 0; // Non-blocking for async
    private static final long INPUT_TIMEOUT_US = 10000; // 10ms for input
    private static final long FLUSH_TIMEOUT_MS = 500;
    
    private final String label;
    private final String mimeType;
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
    private final AtomicInteger nextOutputFrameIndex = new AtomicInteger(0);
    private final AtomicInteger lastRenderedFrameIndex = new AtomicInteger(-1);
    private final AtomicLong lastFrameTimestampNs = new AtomicLong(0);
    
    // PTS-gated release
    private final AtomicLong targetPresentationTimeUs = new AtomicLong(0);
    private final AtomicBoolean frameReleasedSinceConsume = new AtomicBoolean(false);
    // Outputs before this are discarded unshown: after a seek, the frames from the keyframe up to the target
    private final AtomicLong dropBeforeUs = new AtomicLong(Long.MIN_VALUE);
    
    private final ConcurrentLinkedQueue<FrameInfo> pendingFrames = new ConcurrentLinkedQueue<>();
    
    private FrameAvailableCallback frameCallback;
    
    private HandlerThread codecThread;
    private Handler codecHandler;
    
    private volatile boolean verbose = false;
    
    private static final AtomicInteger activeDecoderCount = new AtomicInteger(0);
    
    private final Object pendingFramesLock = new Object();
    // Guarded by pendingFramesLock
    private boolean endOfStreamQueued = false;
    
    private static class FrameInfo {
        final int frameIndex;
        final long presentationTimeUs;
        
        FrameInfo(int frameIndex, long pts) {
            this.frameIndex = frameIndex;
            this.presentationTimeUs = pts;
        }
    }
    
    public interface FrameAvailableCallback {
        void onFrameAvailable(SurfaceTextureVideoDecoder decoder, int frameIndex, long timestampNs);
        void onDecoderError(SurfaceTextureVideoDecoder decoder, String error);
    }
    
    public SurfaceTextureVideoDecoder(String label) {
        this.label = label;
        this.mimeType = MIME_VP9;
    }
    
    public SurfaceTextureVideoDecoder(String label, String mimeType) {
        this.label = label;
        this.mimeType = mimeType;
    }
    
    // -------------------------------------------------------------------------
    // Static utility methods
    // -------------------------------------------------------------------------
    
    public static int getMaxInstances(String mime) {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        for (MediaCodecInfo codecInfo : codecList.getCodecInfos()) {
            if (codecInfo.isEncoder()) continue;
            for (String type : codecInfo.getSupportedTypes()) {
                if (type.equalsIgnoreCase(mime)) {
                    try {
                        MediaCodecInfo.CodecCapabilities caps = codecInfo.getCapabilitiesForType(mime);
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                            return caps.getMaxSupportedInstances();
                        }
                    } catch (Exception e) {
                        Log.e(TAG, "Error getting max instances for " + mime, e);
                    }
                }
            }
        }
        return -1;
    }
    
    /** @deprecated Use getMaxInstances(MIME_VP9) */
    public static int getMaxVp9Instances() {
        return getMaxInstances(MIME_VP9);
    }
    
    public static int getActiveDecoderCount() {
        return activeDecoderCount.get();
    }
    
    public static boolean isHardwareDecoderAvailable(String mime) {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
        for (MediaCodecInfo info : codecList.getCodecInfos()) {
            if (info.isEncoder()) continue;
            
            for (String type : info.getSupportedTypes()) {
                if (mime.equalsIgnoreCase(type)) {
                    String name = info.getName();
                    String lowerName = name.toLowerCase();
                    
                    if (lowerName.contains(".secure") || lowerName.contains("_secure")) {
                        continue;
                    }
                    
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        if (info.isHardwareAccelerated()) {
                            return true;
                        }
                    } else {
                        if (!lowerName.contains("sw") && !lowerName.contains("google")) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }
    
    /**
     * Whether a decoder for {@code mime} lists the stream's FFmpeg profile. Android VP9 decoders
     * rarely list the 4:4:4 profiles 1 and 3, and MediaCodec has no AV1 profile beyond Main.
     * An unknown (negative) profile counts as supported.
     */
    public static boolean isProfileSupported(String mime, int avProfile) {
        if (avProfile < 0) return true;
        if (MIME_AV1.equalsIgnoreCase(mime)) return avProfile == 0;
        if (avProfile > 3) return false;
        int profile = MediaCodecInfo.CodecProfileLevel.VP9Profile0 << avProfile;  // 1, 2, 4, 8
        
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        for (MediaCodecInfo info : codecList.getCodecInfos()) {
            if (info.isEncoder()) continue;
            for (String type : info.getSupportedTypes()) {
                if (!type.equalsIgnoreCase(mime)) continue;
                for (MediaCodecInfo.CodecProfileLevel pl : info.getCapabilitiesForType(type).profileLevels) {
                    if (pl.profile == profile) return true;
                }
            }
        }
        return false;
    }
    
    /** @deprecated Use isHardwareDecoderAvailable(MIME_VP9) */
    public static boolean isVp9HardwareDecoderAvailable() {
        return isHardwareDecoderAvailable(MIME_VP9);
    }
    
    public static boolean isAv1HardwareDecoderAvailable() {
        return isHardwareDecoderAvailable(MIME_AV1);
    }
    
    public static String getDecoderInfo() {
        StringBuilder info = new StringBuilder();
        info.append("Video Decoder Information\n");
        info.append("========================\n");
        
        String[] mimes = { MIME_VP9, MIME_AV1 };
        String[] labels = { "VP9", "AV1" };
        
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
        MediaCodecInfo[] codecInfos = codecList.getCodecInfos();
        
        for (int m = 0; m < mimes.length; m++) {
            String mime = mimes[m];
            String codecLabel = labels[m];
            int decoderCount = 0;
            
            info.append("\n--- ").append(codecLabel).append(" (").append(mime).append(") ---\n");
            
            for (MediaCodecInfo codecInfo : codecInfos) {
                if (codecInfo.isEncoder()) continue;
                
                for (String type : codecInfo.getSupportedTypes()) {
                    if (type.equalsIgnoreCase(mime)) {
                        decoderCount++;
                        info.append("\nDecoder: ").append(codecInfo.getName()).append("\n");
                        
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                            info.append("  Hardware accelerated: ").append(codecInfo.isHardwareAccelerated()).append("\n");
                            info.append("  Software only: ").append(codecInfo.isSoftwareOnly()).append("\n");
                            info.append("  Vendor: ").append(codecInfo.isVendor()).append("\n");
                        }
                        
                        try {
                            MediaCodecInfo.CodecCapabilities caps = codecInfo.getCapabilitiesForType(mime);
                            
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                                int maxInstances = caps.getMaxSupportedInstances();
                                info.append("  Max concurrent instances: ").append(maxInstances).append("\n");
                                info.append("  --> Max dual-stream players: ").append(maxInstances / 2).append("\n");
                            }
                            
                            int[] colorFormats = caps.colorFormats;
                            info.append("  Color formats: ");
                            for (int i = 0; i < Math.min(colorFormats.length, 5); i++) {
                                info.append(colorFormats[i]);
                                if (i < colorFormats.length - 1 && i < 4) info.append(", ");
                            }
                            if (colorFormats.length > 5) info.append("...");
                            info.append("\n");
                            
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
            
            info.append("\nTotal ").append(codecLabel).append(" decoders found: ").append(decoderCount).append("\n");
        }
        
        info.append("\nCurrently active decoders: ").append(activeDecoderCount.get()).append("\n");
        
        return info.toString();
    }
    
    /** @deprecated Use getDecoderInfo() */
    public static String getVp9DecoderInfo() {
        return getDecoderInfo();
    }
    
    // -------------------------------------------------------------------------
    // Instance methods
    // -------------------------------------------------------------------------
    
    /** @param avColorRange the stream's FFmpeg AVColorRange; with avColorSpace, sets MediaCodec's colour keys */
    public boolean configure(int width, int height, int avColorRange, int avColorSpace) {
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
            int[] textures = new int[1];
            GLES20.glGenTextures(1, textures, 0);
            oesTextureId = textures[0];
            
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            
            logV("[" + label + "] Created OES texture: " + oesTextureId);
            
            surfaceTexture = new SurfaceTexture(oesTextureId);
            surfaceTexture.setDefaultBufferSize(width, height);
            surfaceTexture.setOnFrameAvailableListener(this);
            
            outputSurface = new Surface(surfaceTexture);
            
            String decoderName = findBestDecoder();
            if (decoderName == null) {
                Log.e(TAG, "[" + label + "] No decoder found for " + mimeType);
                return false;
            }
            
            int maxInstances = getMaxInstances(mimeType);
            int currentActive = activeDecoderCount.get();
            logV("[" + label + "] Decoder instances: " + currentActive + "/" + maxInstances + " (mime=" + mimeType + ")");
            
            if (maxInstances > 0 && currentActive >= maxInstances) {
                Log.w(TAG, "[" + label + "] WARNING: At max decoder limit!");
            }
            
            logV("[" + label + "] Using decoder: " + decoderName + " for " + width + "x" + height + " (" + mimeType + ")");
            codec = MediaCodec.createByCodecName(decoderName);
            activeDecoderCount.incrementAndGet();
            
            format = MediaFormat.createVideoFormat(mimeType, width, height);
            MediaCodecColor.apply(format, avColorRange, avColorSpace, width, height);
            codec.configure(format, outputSurface, null, 0);
            
            codecThread = new HandlerThread("Codec-" + label);
            codecThread.start();
            codecHandler = new Handler(codecThread.getLooper());
            
            isConfigured.set(true);
            logV("[" + label + "] Configured with Surface output (GPU-accelerated)");
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
            codecHandler.post(this::pollOutput);
            logV("[" + label + "] Started");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] Failed to start: " + e.getMessage());
            return false;
        }
    }
    
    public boolean queueInputPacket(byte[] data, int offset, int size, long presentationTimeUs, int frameIndex) {
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
                inputBuffer.put(data, offset, size);
                
                pendingFrames.offer(new FrameInfo(frameIndex, presentationTimeUs));
                codec.queueInputBuffer(inputIndex, 0, size, presentationTimeUs, 0);
                return true;
                
            } catch (Exception e) {
                Log.e(TAG, "[" + label + "] Queue error: " + e.getMessage());
                return false;
            }
        }
    }
    
    public boolean queueInputPacket(ByteBuffer data, long presentationTimeUs, int frameIndex) {
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
                int size = data.remaining();
                inputBuffer.put(data);
                data.rewind();
                
                logV("[" + label + "] Queued frame " + frameIndex + ", size=" + size + ", pts=" + presentationTimeUs);
                
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
     * Tells the decoder no more input follows, so it outputs the frames it still holds back (AV1
     * decoders keep some until then). Until the next flush, no packets are accepted.
     *
     * @return false if the decoder had no free input buffer; try again
     */
    public boolean queueEndOfStream() {
        synchronized (pendingFramesLock) {
            if (!isStarted.get() || isReleased.get() || endOfStreamQueued) return true;
            try {
                int inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
                if (inputIndex < 0) return false;
                codec.queueInputBuffer(inputIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                endOfStreamQueued = true;
                logV("[" + label + "] Queued end of stream");
            } catch (Exception e) {
                Log.e(TAG, "[" + label + "] End-of-stream error: " + e.getMessage());
            }
            return true;
        }
    }

    private void pollOutput() {
        if (isReleased.get() || !isStarted.get()) {
            return;
        }
        
        try {
            MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
            
            while (!isReleased.get() && isStarted.get()) {
                if (frameReleasedSinceConsume.get()) {
                    break;
                }
                
                int outputIndex = codec.dequeueOutputBuffer(bufferInfo, DEQUEUE_TIMEOUT_US);
                
                if (outputIndex >= 0) {
                    long pts = bufferInfo.presentationTimeUs;
                    long target = targetPresentationTimeUs.get();
                    
                    boolean endOfStream = (bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    if (endOfStream && bufferInfo.size == 0) {
                        codec.releaseOutputBuffer(outputIndex, false);
                        continue;
                    }
                    
                    if (pts < dropBeforeUs.get()) {
                        codec.releaseOutputBuffer(outputIndex, false);
                        pendingFrames.poll();
                        continue;
                    }
                    if (pts <= target) {
                        codec.releaseOutputBuffer(outputIndex, true);
                        frameReleasedSinceConsume.set(true);
                        
                        logV("[" + label + "] Released frame, pts=" + pts + 
                              ", target=" + target + ", flags=" + bufferInfo.flags);
                        break;
                    } else {
                        heldOutputIndex = outputIndex;
                        heldOutputPts = pts;
                        heldOutputFlags = bufferInfo.flags;
                        break;
                    }
                    
                } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat newFormat = codec.getOutputFormat();
                    logV("[" + label + "] Output format changed: " + newFormat);
                    
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
        
        if (!isReleased.get() && isStarted.get()) {
            if (heldOutputIndex >= 0) {
                codecHandler.postDelayed(this::checkHeldBuffer, 1);
            } else {
                codecHandler.postDelayed(this::pollOutput, 1);
            }
        }
    }
    
    private int heldOutputIndex = -1;
    private long heldOutputPts = 0;
    private int heldOutputFlags = 0;
    
    private void checkHeldBuffer() {
        if (isReleased.get() || !isStarted.get()) {
            if (heldOutputIndex >= 0) {
                try { codec.releaseOutputBuffer(heldOutputIndex, false); } catch (Exception ignored) {}
                heldOutputIndex = -1;
            }
            return;
        }
        
        if (heldOutputIndex >= 0) {
            if (frameReleasedSinceConsume.get()) {
                if (!isReleased.get() && isStarted.get()) {
                    codecHandler.postDelayed(this::checkHeldBuffer, 1);
                }
                return;
            }
            
            long target = targetPresentationTimeUs.get();
            if (heldOutputPts <= target) {
                try {
                    codec.releaseOutputBuffer(heldOutputIndex, true);
                    frameReleasedSinceConsume.set(true);
                    
                    logV("[" + label + "] Released held frame, pts=" + heldOutputPts + 
                          ", target=" + target);
                } catch (Exception e) {
                    Log.w(TAG, "[" + label + "] Error releasing held buffer: " + e.getMessage());
                }
                heldOutputIndex = -1;
                
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
    
    @Override
    public void onFrameAvailable(SurfaceTexture surfaceTexture) {
        if (isReleased.get()) return;
        
        hasNewFrame.set(true);
        
        FrameInfo frameInfo = pendingFrames.poll();
        int frameIndex;
        if (frameInfo != null) {
            frameIndex = frameInfo.frameIndex;
        } else {
            frameIndex = nextOutputFrameIndex.getAndIncrement();
            Log.w(TAG, "[" + label + "] pendingFrames empty! Using counter fallback: " + frameIndex);
        }
        
        long timestampNs = surfaceTexture.getTimestamp();
        
        logV("[" + label + "] onFrameAvailable: frameIndex=" + frameIndex + ", ts=" + timestampNs);
        
        lastFrameTimestampNs.set(timestampNs);
        lastRenderedFrameIndex.set(frameIndex);
        
        if (frameCallback != null) {
            frameCallback.onFrameAvailable(this, frameIndex, timestampNs);
        }
    }
    
    public void updateTexImage() {
        if (surfaceTexture != null) {
            boolean wasNew = hasNewFrame.getAndSet(false);
            try {
                surfaceTexture.updateTexImage();
                if (!wasNew) {
                    Log.w(TAG, "[" + label + "] updateTexImage called with hasNewFrame=false (timing issue?)");
                }
            } catch (Exception e) {
                Log.w(TAG, "[" + label + "] updateTexImage error: " + e.getMessage());
            }
        }
    }
    
    public boolean hasNewFrame() {
        return hasNewFrame.get();
    }
    
    public int getOesTextureId() {
        return oesTextureId;
    }
    
    public void getTransformMatrix(float[] matrix) {
        if (surfaceTexture != null) {
            surfaceTexture.getTransformMatrix(matrix);
        }
    }
    
    public long getLastFrameTimestampNs() {
        return lastFrameTimestampNs.get();
    }
    
    /** The presentation time of the frame the last updateTexImage() latched; GL thread. */
    public long getLatchedTimestampNs() {
        return surfaceTexture != null ? surfaceTexture.getTimestamp() : -1;
    }
    
    public int getLastRenderedFrameIndex() {
        return lastRenderedFrameIndex.get();
    }
    
    public void setFrameCallback(FrameAvailableCallback callback) {
        this.frameCallback = callback;
    }
    
    public void setVerbose(boolean verbose) {
        this.verbose = verbose;
    }
    
    /** Lifecycle and per-frame chatter, only with verbose. Warnings and errors always log. */
    private void logV(String message) {
        if (verbose) Log.i(TAG, message);
    }
    
    public void setTargetPresentationTimeUs(long ptsUs) {
        targetPresentationTimeUs.set(ptsUs);
    }

    /** Discards decoded frames earlier than ptsUs without showing them, until the next flush. */
    public void setDropBeforeUs(long ptsUs) {
        dropBeforeUs.set(ptsUs);
    }
    
    public boolean wasFrameReleased() {
        return frameReleasedSinceConsume.get();
    }
    
    public void clearFrameReleased() {
        frameReleasedSinceConsume.set(false);
    }
    
    /**
     * Drops every queued and decoded frame. Runs on the codec thread, between output polls: a poll
     * holding an output buffer across the flush would afterwards wait on a buffer the codec has
     * taken back, and show nothing until the playhead reached that buffer's time again.
     */
    public void flush() {
        Handler handler = codecHandler;
        if (isReleased.get() || handler == null) {
            Log.w(TAG, "[" + label + "] flush() called but already released");
            return;
        }
        
        logV("[" + label + "] Flushing decoder...");
        CountDownLatch flushed = new CountDownLatch(1);
        handler.post(() -> {
            try {
                flushOnCodecThread(handler);
            } finally {
                flushed.countDown();
            }
        });
        try {
            if (!flushed.await(FLUSH_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                Log.w(TAG, "[" + label + "] Flush still running after " + FLUSH_TIMEOUT_MS + " ms");
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
    
    private void flushOnCodecThread(Handler handler) {
        if (isReleased.get()) return;
        // The poll restarts below, after the flush
        handler.removeCallbacksAndMessages(null);
        synchronized (pendingFramesLock) {
            pendingFrames.clear();
            nextOutputFrameIndex.set(0);
            lastRenderedFrameIndex.set(-1);
            hasNewFrame.set(false);
            lastFrameTimestampNs.set(0);
            targetPresentationTimeUs.set(0);
            dropBeforeUs.set(Long.MIN_VALUE);
            frameReleasedSinceConsume.set(false);
            heldOutputIndex = -1;
            endOfStreamQueued = false;
            try {
                codec.flush();
                logV("[" + label + "] Flushed and restarted successfully");
            } catch (Exception e) {
                Log.e(TAG, "[" + label + "] Flush error: " + e.getMessage());
            }
        }
        handler.post(this::pollOutput);
    }
    
    public void release() {
        if (isReleased.getAndSet(true)) {
            return;
        }
        
        logV("[" + label + "] Releasing");
        
        isStarted.set(false);
        isConfigured.set(false);
        
        cleanup();
        
        logV("[" + label + "] Released");
    }
    
    private void cleanup() {
        if (codecThread != null) {
            codecThread.quitSafely();
            codecThread = null;
            codecHandler = null;
        }
        
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
        
        if (outputSurface != null) {
            outputSurface.release();
            outputSurface = null;
        }
        
        if (surfaceTexture != null) {
            surfaceTexture.setOnFrameAvailableListener(null);
            surfaceTexture.release();
            surfaceTexture = null;
        }
        
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
    public String getMimeType() { return mimeType; }
    public boolean isConfigured() { return isConfigured.get(); }
    public boolean isStarted() { return isStarted.get(); }
    public boolean isReleased() { return isReleased.get(); }
    
    private String findBestDecoder() {
        MediaCodecList codecList = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        MediaCodecInfo[] codecInfos = codecList.getCodecInfos();
        
        String hwDecoder = null;
        String swDecoder = null;
        
        for (MediaCodecInfo codecInfo : codecInfos) {
            if (codecInfo.isEncoder()) continue;
            
            for (String type : codecInfo.getSupportedTypes()) {
                if (type.equalsIgnoreCase(mimeType)) {
                    String name = codecInfo.getName();
                    
                    if (name.contains(".secure")) continue;
                    
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
                            logV("Found hardware decoder for " + mimeType + ": " + name);
                        }
                    } else {
                        if (swDecoder == null) {
                            swDecoder = name;
                            logV("Found software decoder for " + mimeType + ": " + name);
                        }
                    }
                }
            }
        }
        
        String selected = hwDecoder != null ? hwDecoder : swDecoder;
        logV("Selected decoder for " + mimeType + ": " + selected + " (hw=" + hwDecoder + ", sw=" + swDecoder + ")");
        return selected;
    }
}
