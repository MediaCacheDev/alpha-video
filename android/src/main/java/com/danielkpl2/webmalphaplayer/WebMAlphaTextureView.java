package com.danielkpl2.webmalphaplayer;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.util.AttributeSet;
import android.util.Log;
import android.view.TextureView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * TextureView-based WebM player that integrates properly with React Native's view hierarchy.
 * Unlike GLSurfaceView, TextureView:
 * - Respects z-ordering and stacking
 * - Works correctly inside ScrollView
 * - Supports proper clipping
 * - Participates in the normal view hierarchy
 * 
 * Uses a dedicated GL render thread for OpenGL ES operations.
 */
public class WebMAlphaTextureView extends TextureView implements 
    TextureView.SurfaceTextureListener,
    WebMAlphaPlayerNative.PlayerListener {
    
    private static final String TAG = "WebMAlphaTexView";
    
    // C++ player handle - volatile for thread visibility
    private volatile long playerId = 0;
    private volatile boolean isReleased = false;
    
    // EGL/OpenGL resources (managed on render thread)
    private EGLDisplay eglDisplay;
    private EGLContext eglContext;
    private EGLSurface eglSurface;
    private EGLConfig eglConfig;
    
    // OpenGL resources
    private int programId;
    private int yTextureId, uTextureId, vTextureId, aTextureId;
    private FloatBuffer vertexBuffer;
    private int currentWidth, currentHeight;
    
    // Surface state
    private SurfaceTexture surfaceTexture;
    private final AtomicBoolean surfaceAvailable = new AtomicBoolean(false);
    private int surfaceWidth, surfaceHeight;
    
    // Current frame data (protected by frameDataLock)
    private final Object frameDataLock = new Object();
    private volatile ByteBuffer pendingY, pendingU, pendingV, pendingA;
    private volatile int pendingWidth, pendingHeight;
    private volatile boolean hasNewFrame = false;
    
    // Reusable buffers to avoid allocations during playback
    private ByteBuffer reuseY, reuseU, reuseV, reuseA;
    
    // Configuration
    private boolean autoplay = true;
    private boolean loop = true;
    private double playbackSpeed = 1.0;
    private int decodeThreads = 4;
    private int cacheSize = 250;

    // Render thread - handles all GL operations
    private ExecutorService renderThread;
    private final AtomicBoolean renderPending = new AtomicBoolean(false);
    
    // Native loading executor
    private ExecutorService nativeLoadExecutor;
    private final Object nativeLoadLock = new Object();
    private Future<?> currentLoadTask;
    
    // Store pending source for re-load after reattach
    private String pendingSourcePath = null;
    private boolean pendingSourceIsUrl = false;
    
    // Callbacks
    private PlayerCallback callback;
    
    public interface PlayerCallback {
        void onLoad(int totalFrames, int width, int height);
        void onError(String error);
        void onPreloadComplete(int frames, double timeMs);
    }
    
    public WebMAlphaTextureView(Context context) {
        super(context);
        init();
    }
    
    public WebMAlphaTextureView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }
    
    private void init() {
        setSurfaceTextureListener(this);
        setOpaque(false);  // Enable transparency
        
        // Create executors
        renderThread = createRenderThread();
        nativeLoadExecutor = createLoadExecutor();
        
        // Create C++ player
        playerId = WebMAlphaPlayerNative.nativeCreate(
            decodeThreads, cacheSize, true, loop, playbackSpeed
        );
        WebMAlphaPlayerNative.nativeSetListener(playerId, this);
        
        Log.i(TAG, "Created TextureView player: " + playerId + 
              ", FFmpeg: " + WebMAlphaPlayerNative.nativeGetFFmpegVersion());
    }
    
    private ExecutorService createRenderThread() {
        return Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "WebMGLRender");
            t.setPriority(Thread.MAX_PRIORITY);
            return t;
        });
    }
    
    private ExecutorService createLoadExecutor() {
        return Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "WebMAlphaNativeLoad");
            t.setPriority(Thread.NORM_PRIORITY - 1);
            return t;
        });
    }
    
    //------------------------------------------------------------------------------
    // Public API
    //------------------------------------------------------------------------------
    
    public void setCallback(PlayerCallback callback) {
        this.callback = callback;
    }
    
    public void setAutoplay(boolean autoplay) {
        this.autoplay = autoplay;
    }
    
    public void setLoop(boolean loop) {
        this.loop = loop;
    }
    
    public void setPaused(boolean paused) {
        if (paused) {
            pause();
        } else if (autoplay) {
            play();
        }
    }
    
    public void loadFromFile(String path) {
        pendingSourcePath = path;
        pendingSourceIsUrl = false;
        
        if (isReleased) {
            Log.i(TAG, "🔄 Re-initializing released view for loadFromFile");
            initializePlayer();
        }
        
        enqueueNativeLoad(() -> WebMAlphaPlayerNative.nativeLoadFromFile(playerId, path),
            "from file");
    }
    
    public void loadFromBytes(byte[] data) {
        pendingSourcePath = null;
        
        if (isReleased) {
            Log.i(TAG, "🔄 Re-initializing released view for loadFromBytes");
            initializePlayer();
        }
        
        enqueueNativeLoad(() -> WebMAlphaPlayerNative.nativeLoadFromBytes(playerId, data),
            "from bytes");
    }
    
    public void loadFromUrl(String url) {
        pendingSourcePath = url;
        pendingSourceIsUrl = true;
        
        if (isReleased) {
            Log.i(TAG, "🔄 Re-initializing released view for loadFromUrl");
            initializePlayer();
        }
        
        enqueueNativeLoad(() -> WebMAlphaPlayerNative.nativeLoadFromUrl(playerId, url),
            "from url");
    }
    
    public void play() {
        if (isReleased || playerId == 0) return;
        WebMAlphaPlayerNative.nativePlay(playerId);
    }
    
    public void pause() {
        if (isReleased || playerId == 0) return;
        WebMAlphaPlayerNative.nativePause(playerId);
    }
    
    public void stop() {
        if (isReleased || playerId == 0) return;
        WebMAlphaPlayerNative.nativeStop(playerId);
    }
    
    public void seekToFrame(int frame) {
        if (isReleased || playerId == 0) return;
        WebMAlphaPlayerNative.nativeSeekToFrame(playerId, frame);
    }
    
    public boolean isLoaded() {
        if (isReleased || playerId == 0) return false;
        return WebMAlphaPlayerNative.nativeIsLoaded(playerId);
    }
    
    public boolean isPlaying() {
        if (isReleased || playerId == 0) return false;
        return WebMAlphaPlayerNative.nativeIsPlaying(playerId);
    }
    
    public int getTotalFrames() {
        if (isReleased || playerId == 0) return 0;
        return WebMAlphaPlayerNative.nativeGetTotalFrames(playerId);
    }
    
    public int getVideoWidth() {
        if (isReleased || playerId == 0) return 0;
        return WebMAlphaPlayerNative.nativeGetWidth(playerId);
    }
    
    public int getVideoHeight() {
        if (isReleased || playerId == 0) return 0;
        return WebMAlphaPlayerNative.nativeGetHeight(playerId);
    }
    
    //------------------------------------------------------------------------------
    // SurfaceTextureListener
    //------------------------------------------------------------------------------
    
    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
        Log.i(TAG, "🎨 Surface available: " + width + "x" + height);
        this.surfaceTexture = surface;
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        surfaceAvailable.set(true);
        
        // Initialize EGL on render thread
        renderThread.execute(this::initEGL);
    }
    
    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {
        Log.i(TAG, "🔄 Surface size changed: " + width + "x" + height);
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        
        // Update viewport on render thread
        renderThread.execute(() -> {
            if (eglSurface != null && eglContext != null) {
                makeCurrent();
                GLES20.glViewport(0, 0, width, height);
            }
        });
    }
    
    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
        Log.i(TAG, "🗑️ Surface destroyed");
        surfaceAvailable.set(false);
        
        // Clean up EGL on render thread and wait for completion
        try {
            renderThread.submit(this::destroyEGL).get();
        } catch (Exception e) {
            Log.e(TAG, "Error waiting for EGL cleanup: " + e.getMessage());
        }
        
        this.surfaceTexture = null;
        return true;  // Return true to let TextureView release the SurfaceTexture
    }
    
    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surface) {
        // Called when updateTexImage() is called, usually nothing to do here
    }
    
    //------------------------------------------------------------------------------
    // EGL Setup (runs on render thread)
    //------------------------------------------------------------------------------
    
    private void initEGL() {
        Log.i(TAG, "🔧 Initializing EGL");
        
        // Get display
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            Log.e(TAG, "Unable to get EGL display");
            return;
        }
        
        // Initialize
        int[] version = new int[2];
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            Log.e(TAG, "Unable to initialize EGL");
            return;
        }
        
        // Choose config with alpha support
        int[] configAttribs = {
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_NONE
        };
        
        EGLConfig[] configs = new EGLConfig[1];
        int[] numConfigs = new int[1];
        if (!EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfigs, 0)) {
            Log.e(TAG, "Unable to choose EGL config");
            return;
        }
        eglConfig = configs[0];
        
        // Create context
        int[] contextAttribs = {
            EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL14.EGL_NONE
        };
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT, contextAttribs, 0);
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            Log.e(TAG, "Unable to create EGL context");
            return;
        }
        
        // Create window surface from SurfaceTexture
        int[] surfaceAttribs = { EGL14.EGL_NONE };
        eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surfaceTexture, surfaceAttribs, 0);
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            Log.e(TAG, "Unable to create EGL surface");
            return;
        }
        
        // Make current and initialize GL resources
        makeCurrent();
        initGL();
        
        Log.i(TAG, "✅ EGL initialized successfully");
    }
    
    private void destroyEGL() {
        Log.i(TAG, "🗑️ Destroying EGL");
        
        if (eglDisplay != null) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            
            if (eglSurface != null) {
                EGL14.eglDestroySurface(eglDisplay, eglSurface);
                eglSurface = null;
            }
            
            if (eglContext != null) {
                EGL14.eglDestroyContext(eglDisplay, eglContext);
                eglContext = null;
            }
            
            EGL14.eglTerminate(eglDisplay);
            eglDisplay = null;
        }
        
        // Reset GL state
        programId = 0;
        yTextureId = uTextureId = vTextureId = aTextureId = 0;
        currentWidth = currentHeight = 0;
    }
    
    private void makeCurrent() {
        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            Log.e(TAG, "eglMakeCurrent failed");
        }
    }
    
    //------------------------------------------------------------------------------
    // OpenGL Setup (runs on render thread after EGL init)
    //------------------------------------------------------------------------------
    
    private void initGL() {
        GLES20.glClearColor(0f, 0f, 0f, 0f);
        
        // Create shader program
        String vertexShader = 
            "attribute vec4 aPosition;\n" +
            "attribute vec2 aTexCoord;\n" +
            "varying vec2 vTexCoord;\n" +
            "void main() {\n" +
            "    gl_Position = aPosition;\n" +
            "    vTexCoord = aTexCoord;\n" +
            "}\n";
        
        String fragmentShader = 
            "precision mediump float;\n" +
            "varying vec2 vTexCoord;\n" +
            "uniform sampler2D yTexture;\n" +
            "uniform sampler2D uTexture;\n" +
            "uniform sampler2D vTexture;\n" +
            "uniform sampler2D aTexture;\n" +
            "void main() {\n" +
            "    float y = texture2D(yTexture, vTexCoord).r;\n" +
            "    float u = texture2D(uTexture, vTexCoord).r - 0.5;\n" +
            "    float v = texture2D(vTexture, vTexCoord).r - 0.5;\n" +
            "    float a = texture2D(aTexture, vTexCoord).r;\n" +
            "    float r = clamp(y + 1.402 * v, 0.0, 1.0);\n" +
            "    float g = clamp(y - 0.344136 * u - 0.714136 * v, 0.0, 1.0);\n" +
            "    float b = clamp(y + 1.772 * u, 0.0, 1.0);\n" +
            "    gl_FragColor = vec4(r * a, g * a, b * a, a);\n" +
            "}\n";
        
        programId = createProgram(vertexShader, fragmentShader);
        
        // Create textures
        int[] textures = new int[4];
        GLES20.glGenTextures(4, textures, 0);
        yTextureId = textures[0];
        uTextureId = textures[1];
        vTextureId = textures[2];
        aTextureId = textures[3];
        
        // Set up vertex buffer (fullscreen quad with flipped Y for TextureView)
        float[] vertices = {
            -1f, -1f, 0f, 1f,  // bottom-left
             1f, -1f, 1f, 1f,  // bottom-right
            -1f,  1f, 0f, 0f,  // top-left
             1f,  1f, 1f, 0f   // top-right
        };
        vertexBuffer = ByteBuffer.allocateDirect(vertices.length * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .put(vertices);
        vertexBuffer.position(0);
        
        // Enable blending for transparency
        GLES20.glEnable(GLES20.GL_BLEND);
        GLES20.glBlendFunc(GLES20.GL_ONE, GLES20.GL_ONE_MINUS_SRC_ALPHA);
        
        // Set viewport
        GLES20.glViewport(0, 0, surfaceWidth, surfaceHeight);
        
        Log.i(TAG, "✅ OpenGL initialized");
    }
    
    private int createProgram(String vertexSource, String fragmentSource) {
        int vertexShader = loadShader(GLES20.GL_VERTEX_SHADER, vertexSource);
        int fragmentShader = loadShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource);
        
        int program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, vertexShader);
        GLES20.glAttachShader(program, fragmentShader);
        GLES20.glLinkProgram(program);
        
        return program;
    }
    
    private int loadShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        return shader;
    }
    
    //------------------------------------------------------------------------------
    // Rendering
    //------------------------------------------------------------------------------
    
    private void requestRender() {
        // Prevent queuing multiple render calls
        if (renderPending.compareAndSet(false, true)) {
            renderThread.execute(this::doRender);
        }
    }
    
    private void doRender() {
        renderPending.set(false);
        
        if (!surfaceAvailable.get() || eglSurface == null || eglContext == null || isReleased) {
            return;
        }
        
        try {
            makeCurrent();
            
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            
            // Grab frame data under lock
            ByteBuffer y, u, v, a;
            int width, height;
            synchronized (frameDataLock) {
                if (isReleased || !hasNewFrame || pendingY == null) {
                    // Still need to swap buffers to show cleared frame
                    EGL14.eglSwapBuffers(eglDisplay, eglSurface);
                    return;
                }
                
                y = pendingY;
                u = pendingU;
                v = pendingV;
                a = pendingA;
                width = pendingWidth;
                height = pendingHeight;
                hasNewFrame = false;
            }
            
            if (y == null || u == null || v == null || a == null || width <= 0 || height <= 0) {
                EGL14.eglSwapBuffers(eglDisplay, eglSurface);
                return;
            }
            
            // Update textures
            GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1);
            
            // Y texture
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, yTextureId);
            if (currentWidth != width || currentHeight != height) {
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE,
                    width, height, 0, GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, y);
                setupTexture();
            } else {
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, y);
            }
            
            // U texture
            int uvWidth = width / 2;
            int uvHeight = height / 2;
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, uTextureId);
            if (currentWidth != width || currentHeight != height) {
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE,
                    uvWidth, uvHeight, 0, GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, u);
                setupTexture();
            } else {
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                    GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, u);
            }
            
            // V texture
            GLES20.glActiveTexture(GLES20.GL_TEXTURE2);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, vTextureId);
            if (currentWidth != width || currentHeight != height) {
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE,
                    uvWidth, uvHeight, 0, GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, v);
                setupTexture();
            } else {
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                    GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, v);
            }
            
            // A texture
            GLES20.glActiveTexture(GLES20.GL_TEXTURE3);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, aTextureId);
            if (currentWidth != width || currentHeight != height) {
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE,
                    width, height, 0, GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, a);
                setupTexture();
            } else {
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, a);
            }
            
            currentWidth = width;
            currentHeight = height;
            
            // Draw
            GLES20.glUseProgram(programId);
            
            int positionHandle = GLES20.glGetAttribLocation(programId, "aPosition");
            int texCoordHandle = GLES20.glGetAttribLocation(programId, "aTexCoord");
            
            vertexBuffer.position(0);
            GLES20.glVertexAttribPointer(positionHandle, 2, GLES20.GL_FLOAT, false, 16, vertexBuffer);
            GLES20.glEnableVertexAttribArray(positionHandle);
            
            vertexBuffer.position(2);
            GLES20.glVertexAttribPointer(texCoordHandle, 2, GLES20.GL_FLOAT, false, 16, vertexBuffer);
            GLES20.glEnableVertexAttribArray(texCoordHandle);
            
            GLES20.glUniform1i(GLES20.glGetUniformLocation(programId, "yTexture"), 0);
            GLES20.glUniform1i(GLES20.glGetUniformLocation(programId, "uTexture"), 1);
            GLES20.glUniform1i(GLES20.glGetUniformLocation(programId, "vTexture"), 2);
            GLES20.glUniform1i(GLES20.glGetUniformLocation(programId, "aTexture"), 3);
            
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
            
            // Swap buffers
            EGL14.eglSwapBuffers(eglDisplay, eglSurface);
            
        } catch (Exception e) {
            Log.w(TAG, "Render exception: " + e.getMessage());
        }
    }
    
    private void setupTexture() {
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
    }
    
    //------------------------------------------------------------------------------
    // PlayerListener callbacks (called from C++ thread)
    //------------------------------------------------------------------------------
    
    @Override
    public void onLoad(int totalFrames, int width, int height) {
        if (isReleased) return;
        
        Log.i(TAG, "🎬 Loaded: " + totalFrames + " frames, " + width + "x" + height);
        
        post(() -> {
            if (isReleased) return;
            
            if (callback != null) {
                callback.onLoad(totalFrames, width, height);
            }
            
            // Render first frame
            if (playerId != 0 && !isReleased) {
                WebMFrame frame = WebMAlphaPlayerNative.nativeGetFrame(playerId, 0);
                if (frame != null && !isReleased) {
                    synchronized (frameDataLock) {
                        if (isReleased) return;
                        
                        reuseY = copyToReusable(frame.getYPlane(), reuseY);
                        reuseU = copyToReusable(frame.getUPlane(), reuseU);
                        reuseV = copyToReusable(frame.getVPlane(), reuseV);
                        reuseA = copyToReusable(frame.getAPlane(), reuseA);
                        
                        pendingY = reuseY;
                        pendingU = reuseU;
                        pendingV = reuseV;
                        pendingA = reuseA;
                        pendingWidth = frame.getWidth();
                        pendingHeight = frame.getHeight();
                        hasNewFrame = true;
                    }
                    if (!isReleased) {
                        requestRender();
                        Log.i(TAG, "🚀 First frame rendered via TextureView pipeline");
                    }
                }
            }
            
            if (autoplay && playerId != 0 && !isReleased) {
                play();
            }
        });
    }
    
    @Override
    public void onFrame(int frameIndex, ByteBuffer yPlane, ByteBuffer uPlane, 
                       ByteBuffer vPlane, ByteBuffer aPlane, int width, int height) {
        if (isReleased) return;
        
        synchronized (frameDataLock) {
            if (isReleased) return;
            
            reuseY = copyToReusable(yPlane, reuseY);
            reuseU = copyToReusable(uPlane, reuseU);
            reuseV = copyToReusable(vPlane, reuseV);
            reuseA = copyToReusable(aPlane, reuseA);
            
            pendingY = reuseY;
            pendingU = reuseU;
            pendingV = reuseV;
            pendingA = reuseA;
            pendingWidth = width;
            pendingHeight = height;
            hasNewFrame = true;
        }
        
        if (!isReleased) {
            requestRender();
        }
    }
    
    private ByteBuffer copyToReusable(ByteBuffer src, ByteBuffer reuse) {
        if (src == null) return null;
        src.rewind();
        int needed = src.remaining();
        
        if (reuse != null && reuse.capacity() >= needed) {
            reuse.clear();
            reuse.put(src);
            reuse.rewind();
            return reuse;
        }
        
        ByteBuffer dst = ByteBuffer.allocateDirect(needed);
        dst.put(src);
        dst.rewind();
        return dst;
    }
    
    @Override
    public void onPlay() {
        if (isReleased) return;
        Log.d(TAG, "Playing");
    }
    
    @Override
    public void onPause() {
        if (isReleased) return;
        Log.d(TAG, "Paused");
    }
    
    @Override
    public void onStop() {
        if (isReleased) return;
        Log.d(TAG, "Stopped");
    }
    
    @Override
    public void onLoop() {
        if (isReleased) return;
        Log.d(TAG, "Looped");
    }
    
    @Override
    public void onEnd() {
        if (isReleased) return;
        Log.d(TAG, "Ended");
    }
    
    @Override
    public void onError(String error) {
        if (isReleased) return;
        Log.e(TAG, "Error: " + error);
        post(() -> {
            if (callback != null && !isReleased) {
                callback.onError(error);
            }
        });
    }
    
    @Override
    public void onPreloadComplete(int totalFrames, double timeMs) {
        if (isReleased) return;
        Log.i(TAG, "✅ Preload complete: " + totalFrames + " frames in " + timeMs + "ms");
        post(() -> {
            if (callback != null && !isReleased) {
                callback.onPreloadComplete(totalFrames, timeMs);
            }
        });
    }
    
    //------------------------------------------------------------------------------
    // Lifecycle
    //------------------------------------------------------------------------------
    
    private void initializePlayer() {
        if (nativeLoadExecutor == null || nativeLoadExecutor.isShutdown()) {
            nativeLoadExecutor = createLoadExecutor();
        }
        
        if (renderThread == null || renderThread.isShutdown()) {
            renderThread = createRenderThread();
        }
        
        if (playerId == 0) {
            playerId = WebMAlphaPlayerNative.nativeCreate(
                decodeThreads, cacheSize, true, loop, playbackSpeed
            );
            WebMAlphaPlayerNative.nativeSetListener(playerId, this);
        }
        
        isReleased = false;
    }
    
    public void release() {
        Log.i(TAG, "🗑️ Full release of player " + playerId);
        
        final long pid = playerId;
        playerId = 0;
        isReleased = true;
        
        cancelNativeLoad();
        
        // Clean up native player on executor
        if (pid != 0) {
            synchronized (nativeLoadLock) {
                if (nativeLoadExecutor != null && !nativeLoadExecutor.isShutdown()) {
                    nativeLoadExecutor.submit(() -> {
                        WebMAlphaPlayerNative.nativeDestroy(pid);
                        Log.i(TAG, "🗑️ Native player destroyed: " + pid);
                    });
                    nativeLoadExecutor.shutdown();
                } else {
                    new Thread(() -> {
                        WebMAlphaPlayerNative.nativeDestroy(pid);
                        Log.i(TAG, "🗑️ Native player destroyed (bg): " + pid);
                    }, "WebMDestroy").start();
                }
            }
        }
        
        // Shutdown render thread
        if (renderThread != null && !renderThread.isShutdown()) {
            renderThread.shutdown();
        }
    }
    
    private void softRelease() {
        Log.i(TAG, "📦 Soft release of player " + playerId);
        
        cancelNativeLoad();
        
        synchronized (frameDataLock) {
            isReleased = true;
            hasNewFrame = false;
            pendingY = pendingU = pendingV = pendingA = null;
            currentWidth = 0;
            currentHeight = 0;
        }
        
        final long pid = playerId;
        if (pid != 0) {
            synchronized (nativeLoadLock) {
                if (nativeLoadExecutor == null || nativeLoadExecutor.isShutdown()) {
                    nativeLoadExecutor = createLoadExecutor();
                }
                nativeLoadExecutor.submit(() -> {
                    WebMAlphaPlayerNative.nativeStop(pid);
                    WebMAlphaPlayerNative.nativeUnload(pid);
                    Log.i(TAG, "📦 Soft release cleanup complete for player " + pid);
                });
            }
        }
    }
    
    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        Log.i(TAG, "🔄 View attached (isReleased=" + isReleased + ", playerId=" + playerId + ")");
        
        if (isReleased && playerId != 0) {
            isReleased = false;
            
            if (renderThread == null || renderThread.isShutdown()) {
                renderThread = createRenderThread();
            }
            
            if (pendingSourcePath != null) {
                Log.i(TAG, "🔄 Re-loading source: " + pendingSourcePath);
                if (pendingSourceIsUrl) {
                    loadFromUrl(pendingSourcePath);
                } else {
                    loadFromFile(pendingSourcePath);
                }
            }
        } else if (isReleased && playerId == 0) {
            initializePlayer();
            
            if (pendingSourcePath != null) {
                Log.i(TAG, "🔄 Re-loading source: " + pendingSourcePath);
                if (pendingSourceIsUrl) {
                    loadFromUrl(pendingSourcePath);
                } else {
                    loadFromFile(pendingSourcePath);
                }
            }
        }
    }
    
    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        Log.i(TAG, "📤 View detached");
        softRelease();
    }
    
    //------------------------------------------------------------------------------
    // Helpers
    //------------------------------------------------------------------------------
    
    private interface NativeLoadCommand {
        boolean run();
    }
    
    private void enqueueNativeLoad(NativeLoadCommand command, String label) {
        synchronized (nativeLoadLock) {
            if (isReleased || playerId == 0) {
                Log.w(TAG, "Ignoring load - player released");
                return;
            }
            
            if (nativeLoadExecutor == null || nativeLoadExecutor.isShutdown()) {
                nativeLoadExecutor = createLoadExecutor();
            }
            
            cancelNativeLoadLocked();
            final long pid = playerId;
            currentLoadTask = nativeLoadExecutor.submit(() -> {
                if (isReleased || playerId != pid) {
                    Log.w(TAG, "Load aborted - player released or changed");
                    return;
                }
                boolean success = command.run();
                if (!success && !isReleased) {
                    Log.e(TAG, "Failed to load video " + label);
                    post(() -> {
                        if (callback != null && !isReleased) {
                            callback.onError("Failed to load video " + label);
                        }
                    });
                }
            });
        }
    }
    
    private void cancelNativeLoad() {
        synchronized (nativeLoadLock) {
            cancelNativeLoadLocked();
        }
    }
    
    private void cancelNativeLoadLocked() {
        if (currentLoadTask != null) {
            currentLoadTask.cancel(true);
            currentLoadTask = null;
        }
    }
}
