package com.danielkpl2.webmalphaplayer;

import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * Ring buffer for decoded video frame pairs (color + alpha).
 * 
 * This solves the SurfaceTexture single-frame limitation by immediately copying
 * decoded frames from OES textures to 2D textures in a circular buffer.
 * 
 * Producer (decode thread): calls writeFrame() to copy OES → 2D texture
 * Consumer (render thread): calls readFrame() to get texture IDs for compositing
 * 
 * Buffer allows decode to run ahead of render, absorbing timing variations.
 */
public class FrameRingBuffer {
    private static final String TAG = "FrameRingBuffer";
    
    // Buffer slot containing a color+alpha texture pair
    public static class FrameSlot {
        int colorTextureId = -1;
        int alphaTextureId = -1;
        int fboId = -1;  // FBO for rendering to these textures
        int frameIndex = -1;
        long timestampNs = 0;
        boolean colorWritten = false;  // True when color has been written
        boolean alphaWritten = false;  // True when alpha has been written
        boolean valid = false;         // True when both color AND alpha are written
        boolean reserved = false;      // True when slot is reserved for a frame
    }
    
    private final int capacity;
    private final int width;
    private final int height;
    private final FrameSlot[] slots;
    
    // Ring buffer indices
    private int writeIndex = 0;  // Hint for next slot to check for reservation
    private int readIndex = 0;   // (unused, kept for compatibility)
    private int count = 0;       // Number of valid (complete) frames in buffer
    private int nextReadFrameIndex = 0;  // Next expected frame index for sequential reading
    
    // Blit shader for copying OES → 2D texture
    private int blitProgram = -1;
    private int blitPositionHandle;
    private int blitTexCoordHandle;
    private int blitTextureHandle;
    private int blitMatrixHandle;
    private FloatBuffer blitVertexBuffer;
    
    private boolean initialized = false;
    private final Object lock = new Object();
    
    // Vertex data for fullscreen quad
    private static final float[] BLIT_VERTICES = {
        -1f, -1f, 0f, 0f,  // bottom-left
         1f, -1f, 1f, 0f,  // bottom-right
        -1f,  1f, 0f, 1f,  // top-left
         1f,  1f, 1f, 1f   // top-right
    };
    
    // Simple OES → 2D blit shaders
    private static final String BLIT_VERTEX_SHADER =
        "attribute vec4 aPosition;\n" +
        "attribute vec2 aTexCoord;\n" +
        "uniform mat4 uTexMatrix;\n" +
        "varying vec2 vTexCoord;\n" +
        "void main() {\n" +
        "    gl_Position = aPosition;\n" +
        "    vTexCoord = (uTexMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;\n" +
        "}\n";
    
    private static final String BLIT_FRAGMENT_SHADER =
        "#extension GL_OES_EGL_image_external : require\n" +
        "precision mediump float;\n" +
        "varying vec2 vTexCoord;\n" +
        "uniform samplerExternalOES uTexture;\n" +
        "void main() {\n" +
        "    gl_FragColor = texture2D(uTexture, vTexCoord);\n" +
        "}\n";
    
    public FrameRingBuffer(int capacity, int width, int height) {
        this.capacity = capacity;
        this.width = width;
        this.height = height;
        this.slots = new FrameSlot[capacity];
        
        for (int i = 0; i < capacity; i++) {
            slots[i] = new FrameSlot();
        }
        
        Log.i(TAG, "Created ring buffer: capacity=" + capacity + ", " + width + "x" + height);
    }
    
    /**
     * Initialize GL resources. Must be called on GL thread.
     */
    public boolean initGL() {
        if (initialized) return true;
        
        Log.i(TAG, "Initializing GL resources for " + capacity + " slots");
        
        // Create blit shader
        blitProgram = createProgram(BLIT_VERTEX_SHADER, BLIT_FRAGMENT_SHADER);
        if (blitProgram == 0) {
            Log.e(TAG, "Failed to create blit shader");
            return false;
        }
        
        blitPositionHandle = GLES20.glGetAttribLocation(blitProgram, "aPosition");
        blitTexCoordHandle = GLES20.glGetAttribLocation(blitProgram, "aTexCoord");
        blitTextureHandle = GLES20.glGetUniformLocation(blitProgram, "uTexture");
        blitMatrixHandle = GLES20.glGetUniformLocation(blitProgram, "uTexMatrix");
        
        // Create vertex buffer
        blitVertexBuffer = ByteBuffer.allocateDirect(BLIT_VERTICES.length * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .put(BLIT_VERTICES);
        blitVertexBuffer.position(0);
        
        // Create textures and FBOs for each slot
        for (int i = 0; i < capacity; i++) {
            FrameSlot slot = slots[i];
            
            // Create color texture (2D, not OES)
            int[] textures = new int[2];
            GLES20.glGenTextures(2, textures, 0);
            slot.colorTextureId = textures[0];
            slot.alphaTextureId = textures[1];
            
            // Configure color texture
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, slot.colorTextureId);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, width, height, 0,
                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
            
            // Configure alpha texture
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, slot.alphaTextureId);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, width, height, 0,
                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
            
            // Create FBO for this slot
            int[] fbos = new int[1];
            GLES20.glGenFramebuffers(1, fbos, 0);
            slot.fboId = fbos[0];
            
            int error = GLES20.glGetError();
            if (error != GLES20.GL_NO_ERROR) {
                Log.e(TAG, "GL error creating slot " + i + ": " + error);
            }
        }
        
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
        
        initialized = true;
        Log.i(TAG, "GL resources initialized successfully");
        return true;
    }
    
    /**
     * Reserve a slot for a specific frame index.
     * Call this before decoding to claim a slot for the frame.
     * 
     * @param frameIndex Frame index to reserve for
     * @return true if reserved, false if buffer is full
     */
    public boolean reserveSlot(int frameIndex) {
        synchronized (lock) {
            // Count reserved-but-not-complete slots
            int reservedCount = 0;
            for (FrameSlot s : slots) {
                if (s.reserved && !s.valid) {
                    reservedCount++;
                }
            }
            
            // Check if we have space (valid frames + reserved frames must be < capacity)
            if (count + reservedCount >= capacity) {
                Log.d(TAG, "Cannot reserve - buffer full (count=" + count + ", reserved=" + reservedCount + ")");
                return false;
            }
            
            // Find the next available slot (not reserved and not valid)
            int slotIndex = -1;
            for (int i = 0; i < capacity; i++) {
                int idx = (writeIndex + i) % capacity;
                FrameSlot s = slots[idx];
                if (!s.reserved && !s.valid) {
                    slotIndex = idx;
                    break;
                }
            }
            
            if (slotIndex == -1) {
                Log.w(TAG, "No available slot found despite space check");
                return false;
            }
            
            FrameSlot slot = slots[slotIndex];
            slot.frameIndex = frameIndex;
            slot.timestampNs = 0;
            slot.colorWritten = false;
            slot.alphaWritten = false;
            slot.valid = false;
            slot.reserved = true;
            
            Log.d(TAG, "🔖 Reserved slot " + slotIndex + " for frame " + frameIndex);
            return true;
        }
    }
    
    /**
     * Write color component to a reserved slot.
     * Call updateTexImage() on the color decoder BEFORE calling this!
     * Must be called on GL thread.
     * 
     * @param colorOesTexId OES texture with color frame
     * @param colorMatrix Transform matrix from color SurfaceTexture
     * @param frameIndex Frame index (must match reserved slot)
     * @return true if written and slot completed, false otherwise
     */
    public boolean writeColor(int colorOesTexId, float[] colorMatrix, int frameIndex) {
        synchronized (lock) {
            // Find the slot reserved for this frame
            FrameSlot slot = findReservedSlot(frameIndex);
            if (slot == null) {
                Log.w(TAG, "No slot reserved for color frame " + frameIndex);
                return false;
            }
            
            // Blit color OES → color 2D texture
            blitOesToTexture(colorOesTexId, slot.colorTextureId, slot.fboId, colorMatrix);
            slot.colorWritten = true;
            slot.timestampNs = System.nanoTime();
            
            Log.d(TAG, "🎨 Wrote color for frame " + frameIndex + " (alpha=" + slot.alphaWritten + ")");
            
            // Check if slot is now complete
            if (slot.colorWritten && slot.alphaWritten) {
                completeSlot(slot, frameIndex);
                return true;
            }
            return false;
        }
    }
    
    /**
     * Write alpha component to a reserved slot.
     * Call updateTexImage() on the alpha decoder BEFORE calling this!
     * Must be called on GL thread.
     * 
     * @param alphaOesTexId OES texture with alpha frame
     * @param alphaMatrix Transform matrix from alpha SurfaceTexture
     * @param frameIndex Frame index (must match reserved slot)
     * @return true if written and slot completed, false otherwise
     */
    public boolean writeAlpha(int alphaOesTexId, float[] alphaMatrix, int frameIndex) {
        synchronized (lock) {
            // Find the slot reserved for this frame
            FrameSlot slot = findReservedSlot(frameIndex);
            if (slot == null) {
                Log.w(TAG, "No slot reserved for alpha frame " + frameIndex);
                return false;
            }
            
            // Blit alpha OES → alpha 2D texture
            blitOesToTexture(alphaOesTexId, slot.alphaTextureId, slot.fboId, alphaMatrix);
            slot.alphaWritten = true;
            
            Log.d(TAG, "⬜ Wrote alpha for frame " + frameIndex + " (color=" + slot.colorWritten + ")");
            
            // Check if slot is now complete
            if (slot.colorWritten && slot.alphaWritten) {
                completeSlot(slot, frameIndex);
                return true;
            }
            return false;
        }
    }
    
    /**
     * Find a reserved slot for the given frame index.
     * Searches from writeIndex backwards to find matching reservation.
     */
    private FrameSlot findReservedSlot(int frameIndex) {
        // Check current write slot first (most common case)
        FrameSlot slot = slots[writeIndex];
        if (slot.reserved && !slot.valid && slot.frameIndex == frameIndex) {
            return slot;
        }
        
        // Search other slots (in case of out-of-order callbacks)
        for (int i = 0; i < capacity; i++) {
            slot = slots[i];
            if (slot.reserved && !slot.valid && slot.frameIndex == frameIndex) {
                return slot;
            }
        }
        return null;
    }
    
    /**
     * Complete a slot after both color and alpha are written.
     */
    private void completeSlot(FrameSlot slot, int frameIndex) {
        slot.valid = true;
        // Keep reserved=true until the slot is read to prevent re-reservation
        
        // Verify frame is in expected sequence
        int expectedNextFrame = 0;
        for (FrameSlot s : slots) {
            if (s.valid && s.frameIndex >= expectedNextFrame) {
                expectedNextFrame = s.frameIndex + 1;
            }
        }
        
        count++;
        
        // Log first frame after clear (loop restart detection)
        if (justCleared && frameIndex == 0) {
            long timeSinceClear = (System.nanoTime() - lastClearTimeNs) / 1_000_000;
            Log.i(TAG, "🔄 First frame after clear: frame 0 completed " + timeSinceClear + "ms after buffer clear");
            justCleared = false;
        }
        
        // Log sync info - this helps diagnose color/alpha frame alignment issues
        Log.d(TAG, "📥 Completed frame " + frameIndex + ", count=" + count + 
              ", nextExpected=" + expectedNextFrame + ", nextRead=" + nextReadFrameIndex);
    }
    
    /**
     * Write a frame pair from OES textures to the buffer.
     * Must be called on GL thread with textures ready.
     * (Legacy method - prefer reserveSlot/writeColor/writeAlpha for independent writes)
     * 
     * @param colorOesTexId OES texture with color frame (call updateTexImage first!)
     * @param alphaOesTexId OES texture with alpha frame (call updateTexImage first!)
     * @param colorMatrix Transform matrix from color SurfaceTexture
     * @param alphaMatrix Transform matrix from alpha SurfaceTexture  
     * @param frameIndex Frame index
     * @param timestampNs Timestamp in nanoseconds
     * @return true if written, false if buffer is full
     */
    public boolean writeFrame(int colorOesTexId, int alphaOesTexId, 
                              float[] colorMatrix, float[] alphaMatrix,
                              int frameIndex, long timestampNs) {
        synchronized (lock) {
            if (count >= capacity) {
                Log.w(TAG, "Buffer full, dropping frame " + frameIndex);
                return false;
            }
            
            FrameSlot slot = slots[writeIndex];
            
            // Blit color OES → color 2D texture
            blitOesToTexture(colorOesTexId, slot.colorTextureId, slot.fboId, colorMatrix);
            
            // Blit alpha OES → alpha 2D texture  
            blitOesToTexture(alphaOesTexId, slot.alphaTextureId, slot.fboId, alphaMatrix);
            
            slot.frameIndex = frameIndex;
            slot.timestampNs = timestampNs;
            slot.colorWritten = true;
            slot.alphaWritten = true;
            slot.valid = true;
            slot.reserved = false;
            
            writeIndex = (writeIndex + 1) % capacity;
            count++;
            
            Log.d(TAG, "📥 Wrote frame " + frameIndex + " to slot, count=" + count);
            return true;
        }
    }
    
    /**
     * Read the next frame from the buffer for rendering.
     * Frames are returned in sequential order by frameIndex.
     * 
     * @return FrameSlot with texture IDs, or null if buffer is empty or next frame not ready
     */
    public FrameSlot readFrame() {
        synchronized (lock) {
            if (count <= 0) {
                return null;
            }
            
            // Find the slot with the next expected frame index
            FrameSlot slot = null;
            for (int i = 0; i < capacity; i++) {
                FrameSlot s = slots[i];
                if (s.valid && s.frameIndex == nextReadFrameIndex) {
                    slot = s;
                    break;
                }
            }
            
            if (slot == null) {
                // Next frame not ready yet - buffer underrun
                return null;
            }
            
            // Mark slot as available for reuse
            slot.valid = false;
            slot.colorWritten = false;
            slot.alphaWritten = false;
            slot.reserved = false;
            
            nextReadFrameIndex++;
            count--;
            
            Log.d(TAG, "📤 Read frame " + slot.frameIndex + " from slot, count=" + count);
            return slot;
        }
    }
    
    /**
     * Peek at the next frame without consuming it.
     * Returns the frame with nextReadFrameIndex if it's ready.
     */
    public FrameSlot peekFrame() {
        synchronized (lock) {
            if (count <= 0) return null;
            // Find the slot with the next expected frame index
            for (int i = 0; i < capacity; i++) {
                FrameSlot s = slots[i];
                if (s.valid && s.frameIndex == nextReadFrameIndex) {
                    return s;
                }
            }
            return null;
        }
    }
    
    /**
     * Get the number of buffered frames.
     */
    public int getBufferedCount() {
        synchronized (lock) {
            return count;
        }
    }
    
    /**
     * Check if buffer has space for a new frame.
     * Returns true if there's an unreserved, invalid slot.
     */
    public boolean hasSpace() {
        synchronized (lock) {
            // Count reserved slots that aren't yet valid
            int reservedCount = 0;
            for (FrameSlot slot : slots) {
                if (slot.reserved && !slot.valid) {
                    reservedCount++;
                }
            }
            return (count + reservedCount) < capacity;
        }
    }
    
    /**
     * Check if buffer is empty.
     */
    public boolean isEmpty() {
        synchronized (lock) {
            return count <= 0;
        }
    }
    
    // Track when buffer was last cleared for diagnostic purposes
    private long lastClearTimeNs = 0;
    private boolean justCleared = false;
    
    /**
     * Clear all frames from buffer.
     */
    public void clear() {
        synchronized (lock) {
            for (FrameSlot slot : slots) {
                slot.valid = false;
                slot.colorWritten = false;
                slot.alphaWritten = false;
                slot.reserved = false;
                slot.frameIndex = -1;
                slot.timestampNs = 0;
            }
            writeIndex = 0;
            readIndex = 0;
            nextReadFrameIndex = 0;
            count = 0;
            lastClearTimeNs = System.nanoTime();
            justCleared = true;
            Log.i(TAG, "🧹 Buffer cleared - ready for new frames (nextReadFrameIndex=0)");
        }
    }
    
    /**
     * Release all GL resources. Must be called on GL thread.
     */
    public void release() {
        if (!initialized) return;
        
        Log.i(TAG, "Releasing GL resources");
        
        for (FrameSlot slot : slots) {
            if (slot.colorTextureId > 0) {
                int[] textures = {slot.colorTextureId, slot.alphaTextureId};
                GLES20.glDeleteTextures(2, textures, 0);
            }
            if (slot.fboId > 0) {
                int[] fbos = {slot.fboId};
                GLES20.glDeleteFramebuffers(1, fbos, 0);
            }
            slot.colorTextureId = -1;
            slot.alphaTextureId = -1;
            slot.fboId = -1;
        }
        
        if (blitProgram > 0) {
            GLES20.glDeleteProgram(blitProgram);
            blitProgram = -1;
        }
        
        initialized = false;
    }
    
    // -------------------------------------------------------------------------
    // Private helper methods
    // -------------------------------------------------------------------------
    
    /**
     * Blit from OES texture to 2D texture using FBO.
     */
    private void blitOesToTexture(int oesTexId, int destTexId, int fboId, float[] texMatrix) {
        // Save current viewport
        int[] savedViewport = new int[4];
        GLES20.glGetIntegerv(GLES20.GL_VIEWPORT, savedViewport, 0);
        
        // Bind FBO and attach destination texture
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fboId);
        GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0,
            GLES20.GL_TEXTURE_2D, destTexId, 0);
        
        int status = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER);
        if (status != GLES20.GL_FRAMEBUFFER_COMPLETE) {
            Log.e(TAG, "FBO not complete: " + status);
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
            // Restore viewport
            GLES20.glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
            return;
        }
        
        // Set viewport to texture size
        GLES20.glViewport(0, 0, width, height);
        
        // Use blit shader
        GLES20.glUseProgram(blitProgram);
        
        // Bind OES texture
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTexId);
        GLES20.glUniform1i(blitTextureHandle, 0);
        
        // Set transform matrix
        GLES20.glUniformMatrix4fv(blitMatrixHandle, 1, false, texMatrix, 0);
        
        // Set up vertex attributes
        blitVertexBuffer.position(0);
        GLES20.glVertexAttribPointer(blitPositionHandle, 2, GLES20.GL_FLOAT, false, 16, blitVertexBuffer);
        GLES20.glEnableVertexAttribArray(blitPositionHandle);
        
        blitVertexBuffer.position(2);
        GLES20.glVertexAttribPointer(blitTexCoordHandle, 2, GLES20.GL_FLOAT, false, 16, blitVertexBuffer);
        GLES20.glEnableVertexAttribArray(blitTexCoordHandle);
        
        // Draw
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        
        // CRITICAL: Ensure GPU completes the blit before we mark the slot as valid
        // Without this, the render thread may read partially-written textures,
        // causing "black fringing" artifacts where color and alpha are out of sync.
        // glFinish() blocks until all GL commands have completed on the GPU.
        GLES20.glFinish();
        
        // Cleanup
        GLES20.glDisableVertexAttribArray(blitPositionHandle);
        GLES20.glDisableVertexAttribArray(blitTexCoordHandle);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0);
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
        
        // Restore viewport
        GLES20.glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    }
    
    private int createProgram(String vertexSource, String fragmentSource) {
        int vertexShader = loadShader(GLES20.GL_VERTEX_SHADER, vertexSource);
        if (vertexShader == 0) return 0;
        
        int fragmentShader = loadShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource);
        if (fragmentShader == 0) {
            GLES20.glDeleteShader(vertexShader);
            return 0;
        }
        
        int program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, vertexShader);
        GLES20.glAttachShader(program, fragmentShader);
        GLES20.glLinkProgram(program);
        
        int[] linkStatus = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linkStatus, 0);
        if (linkStatus[0] != GLES20.GL_TRUE) {
            Log.e(TAG, "Program link error: " + GLES20.glGetProgramInfoLog(program));
            GLES20.glDeleteProgram(program);
            program = 0;
        }
        
        GLES20.glDeleteShader(vertexShader);
        GLES20.glDeleteShader(fragmentShader);
        
        return program;
    }
    
    private int loadShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        
        int[] compileStatus = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, compileStatus, 0);
        if (compileStatus[0] != GLES20.GL_TRUE) {
            Log.e(TAG, "Shader compile error: " + GLES20.glGetShaderInfoLog(shader));
            GLES20.glDeleteShader(shader);
            return 0;
        }
        
        return shader;
    }
    
    public int getCapacity() { return capacity; }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public boolean isInitialized() { return initialized; }
    
    /**
     * Get the next expected frame index for sequential reading.
     * Useful for diagnostics during buffer underruns.
     */
    public int getNextExpectedFrameIndex() {
        synchronized (lock) {
            return nextReadFrameIndex;
        }
    }
}
