package dev.mediacache.alphavideo;

import java.nio.ByteBuffer;

/**
 * Represents a decoded YUVA frame from the C++ player
 * Uses direct ByteBuffers for zero-copy access to plane data
 */
public class WebMFrame {
    
    private final ByteBuffer yPlane;
    private final ByteBuffer uPlane;
    private final ByteBuffer vPlane;
    private final ByteBuffer aPlane;
    private final int width;
    private final int height;
    private final int frameIndex;
    private final double timestamp;
    
    public WebMFrame(
        ByteBuffer yPlane,
        ByteBuffer uPlane,
        ByteBuffer vPlane,
        ByteBuffer aPlane,
        int width,
        int height,
        int frameIndex,
        double timestamp
    ) {
        this.yPlane = yPlane;
        this.uPlane = uPlane;
        this.vPlane = vPlane;
        this.aPlane = aPlane;
        this.width = width;
        this.height = height;
        this.frameIndex = frameIndex;
        this.timestamp = timestamp;
    }
    
    public ByteBuffer getYPlane() { return yPlane; }
    public ByteBuffer getUPlane() { return uPlane; }
    public ByteBuffer getVPlane() { return vPlane; }
    public ByteBuffer getAPlane() { return aPlane; }
    
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getFrameIndex() { return frameIndex; }
    public double getTimestamp() { return timestamp; }
    
    // Convenience getters for plane sizes
    public int getYSize() { return width * height; }
    public int getUVSize() { return ((width + 1) / 2) * ((height + 1) / 2); }
    public int getASize() { return width * height; }
}
