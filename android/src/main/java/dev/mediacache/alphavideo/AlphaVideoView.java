package dev.mediacache.alphavideo;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.graphics.SurfaceTexture;
import android.net.Uri;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.util.AttributeSet;
import android.util.Log;
import android.view.TextureView;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
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
public class AlphaVideoView extends TextureView implements 
    TextureView.SurfaceTextureListener,
    WebMAlphaPlayerNative.PlayerListener {
    
    private static final String TAG = "AlphaVideoView";
    
    // Global decode queue - serializes non-stream decoding across all player instances
    // This prevents CPU saturation and crashes when many players are created simultaneously
    private static ExecutorService globalDecodeQueue;
    private static final Object globalDecodeQueueLock = new Object();
    private static int globalDecodeQueueRefCount = 0;
    
    // C++ player handle - volatile for thread visibility
    private volatile long playerId = 0;
    private volatile boolean isReleased = false;
    
    // Dual-stream player — direct OES texture compositing (no FBO ring buffer)
    private BufferedDualStreamPlayer bufferedPlayer;
    private volatile boolean isDualStreamMode = false;
    
    // EGL/OpenGL resources (managed on render thread)
    private EGLDisplay eglDisplay;
    private EGLContext eglContext;
    private EGLSurface eglSurface;
    private EGLConfig eglConfig;
    
    // OpenGL resources
    private int programId;  // YUVA shader (ByteBuffer mode)
    private int oesProgramId;  // OES shader (Surface mode - GPU accelerated)
    // Surface mode converting raw YUV itself: the driver may convert software decoders' buffers as
    // BT.601 whatever their dataspace (Adreno with c2.android.av1-dav1d). 0 without GL_EXT_YUV_target.
    private int yuvOesProgramId;
    private int glesVersion;
    private int yTextureId, uTextureId, vTextureId, aTextureId;
    private FloatBuffer vertexBuffer;
    private FloatBuffer oesVertexBuffer;  // Vertex buffer with transform matrix support
    private int currentWidth, currentHeight;
    
    // OES texture transformation matrices (from SurfaceTexture)
    // The YUVA shader's YUV to RGB, set from the clip on load. Until then, limited-range BT.601.
    private volatile float[] yuvToRgb = {
        1.164384f, 1.164384f, 1.164384f, 0f,
        0f, -0.391762f, 2.017232f, 0f,
        1.596027f, -0.812968f, 0f, 0f,
        -0.874202f, 0.531668f, -1.085631f, 1f,
    };
    private final float[] colorTransformMatrix = new float[16];
    private final float[] alphaTransformMatrix = new float[16];
    
    // Surface state
    private SurfaceTexture surfaceTexture;
    private final AtomicBoolean surfaceAvailable = new AtomicBoolean(false);
    private boolean glReady = false;  // Main thread: EGL is set up for the current surface
    private int surfaceWidth, surfaceHeight;
    
    // Current frame data (protected by frameDataLock)
    private final Object frameDataLock = new Object();
    private volatile ByteBuffer pendingY, pendingU, pendingV, pendingA;
    private volatile int pendingWidth, pendingHeight;
    private volatile boolean hasNewFrame = false;
    
    // Reusable buffers to avoid allocations during playback
    private ByteBuffer reuseY, reuseU, reuseV, reuseA;
    
    // Configuration
    private boolean autoPlay = true;
    // Kept so that a source loading after the paused prop was set doesn't start playing
    private boolean paused = false;
    private boolean loop = true;
    private double playbackSpeed = 1.0;
    // Reduced defaults for better performance with multiple simultaneous players
    // 2 decode threads is sufficient for most glow videos (60-360px, 60-120 frames)
    // 90 frame cache is enough for typical 60-90 frame glow loops
    private int decodeThreads = 2;
    private int cacheSize = 90;
    
    // Playback mode:
    // - "preload": decode all frames upfront, then play
    // - "stream" (default): decode on-demand with sliding window cache
    // - "streamAndPreload": start playing immediately, cache all frames for smooth looping
    public static final String MODE_PRELOAD = "preload";
    public static final String MODE_STREAM = "stream";
    public static final String MODE_STREAM_AND_PRELOAD = "streamAndPreload";
    
    private String mode = MODE_STREAM;
    
    // Decoder for files with colour and alpha as separate VP8/VP9/AV1 bitstreams; see setDecoder()
    public static final String DECODER_AUTO = "auto";
    public static final String DECODER_MEDIACODEC = "mediacodec";
    public static final String DECODER_MEDIACODEC_HW = "mediacodec-hw";
    public static final String DECODER_MEDIACODEC_SW = "mediacodec-sw";
    public static final String DECODER_SOFTWARE = "software";
    
    private String decoder = DECODER_AUTO;
    // Why "auto" left MediaCodec for the C++ core on this load, for a NO_DECODER error; else null
    private String softwareFallbackReason;
    
    /**
     * {@link PlayerCallback#onError}'s code when the file needs a software decoder that isn't
     * installed: add {@code dev.mediacache:alpha-video-vpx} (VP8, VP9) or {@code alpha-video-av1} (AV1),
     * as the message says. See {@link AlphaVideoCodecs}.
     */
    public static final String ERROR_NO_DECODER = "NO_DECODER";
    /**
     * {@link PlayerCallback#onError}'s code for a data source ({@link #setDataSource}) without the
     * range-source add-on ({@code dev.mediacache:range-source}), which reads them.
     */
    public static final String ERROR_NO_SOURCE_MODULE = "NO_SOURCE_MODULE";
    // A URL played without the add-on is warned of once per process
    private static final AtomicBoolean warnedWithoutAddOn = new AtomicBoolean(false);
    private int streamPrefetchAhead = 8;  // Frames to decode ahead of playback
    
    // Use global decode queue for preload/streamAndPreload modes to prevent CPU saturation
    // When true (default): decoding is serialized across all players
    // When false: each player decodes independently (can cause freezes with many players)
    private boolean useDecodeQueue = true;
    
    // Verbose per-frame logging — when false, only lifecycle/perf logs are emitted
    private boolean verbose = false;

    // Render thread - handles all GL operations
    private ExecutorService renderThread;
    private final AtomicBoolean renderPending = new AtomicBoolean(false);
    
    // Native loading executor
    private ExecutorService nativeLoadExecutor;
    private final Object nativeLoadLock = new Object();
    private Future<?> currentLoadTask;
    private volatile Future<?> softReleaseTask;  // Track soft release for synchronization
    
    // Opening a reader and probing through it wait on the network, so not on the main thread, and
    // never behind a decode.
    private static final ExecutorService probeExecutor = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "WebMProbe");
        t.setDaemon(true);
        return t;
    });
    
    // Where what a reader fetches is kept: cacheDir/alpha-video on disk, or the memory store's name
    private static final String STORE_NAME = "alpha-video";
    public static final long DEFAULT_MAX_STORAGE_BYTES = 512L * 1024 * 1024;
    public static final String STORAGE_AUTO = "auto";
    public static final String STORAGE_DISK = "disk";
    public static final String STORAGE_MEMORY = "memory";
    public static final String STORAGE_NONE = "none";
    
    // Source as given (may be remote), and the source to load once a surface exists and again
    // after reattach. Main thread only.
    private String sourceUri = null;
    private String localSource = null;
    // Set when localSource is read through a range reader (a URL or a data source)
    private SourceReader reader = null;
    private String cacheKey = null;
    private Map<String, String> headers = Collections.emptyMap();
    private String dataSource = null;
    private String storage = STORAGE_AUTO;
    private long maxStorageBytes = DEFAULT_MAX_STORAGE_BYTES;
    // Bumped on every setSource so a slow open can't load over a newer source.
    private int sourceGeneration = 0;
    
    // Callbacks
    private PlayerCallback callback;
    
    /** Called on the main thread. */
    public interface PlayerCallback {
        /**
         * @param decoder the engine and decoders playing the file, e.g. "software (libvpx)" or
         *     "mediacodec-hw (c2.qti.vp9.decoder)"
         */
        void onLoad(int totalFrames, int width, int height, String decoder);
        /** @param code {@link #ERROR_NO_DECODER}, or null */
        void onError(String error, String code);
        void onEnd();
        void onPreloadComplete(int frames, double timeMs);
    }
    
    public AlphaVideoView(Context context) {
        super(context);
        init();
    }
    
    public AlphaVideoView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }
    
    private void init() {
        setSurfaceTextureListener(this);
        setOpaque(false);  // Enable transparency
        
        // Create executors lazily - only render thread is created now
        // Native player creation is deferred to onSurfaceTextureAvailable
        // to prevent resource flooding when many views are created at once
        logV("TextureView initialized (player creation deferred)");
    }
    
    /**
     * Initialize the native player - called when surface is available
     * This prevents resource flooding when many views are created but not yet visible
     */
    private void initializePlayer() {
        if (playerId != 0) {
            logV("Player already initialized: " + playerId);
            return;
        }
        
        // Create executors if not already created
        if (renderThread == null || renderThread.isShutdown()) {
            renderThread = createRenderThread();
        }
        if (nativeLoadExecutor == null || nativeLoadExecutor.isShutdown()) {
            nativeLoadExecutor = createLoadExecutor();
        }
        
        // Create C++ player
        // preloadAllFrames should be false for stream mode, true for preload/streamAndPreload
        boolean preloadAllFrames = !MODE_STREAM.equals(mode);
        logV("Creating player with mode=" + mode + ", preloadAllFrames=" + preloadAllFrames + 
              ", streamPrefetchAhead=" + streamPrefetchAhead);
        playerId = WebMAlphaPlayerNative.nativeCreate(
            decodeThreads, cacheSize, preloadAllFrames, loop, playbackSpeed,
            mode, streamPrefetchAhead
        );
        WebMAlphaPlayerNative.nativeSetListener(playerId, this);
        WebMAlphaPlayerNative.nativeSetVerbose(playerId, verbose);
        isReleased = false;
        
        logV("Created TextureView player: " + playerId + 
              ", " + WebMAlphaPlayerNative.nativeGetLibraryVersions());
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
    
    /** Main thread only. */
    private void reportError(String error) {
        reportError(error, null);
    }
    
    private void reportError(String error, String code) {
        Log.e(TAG, error);
        if (callback != null && !isReleased) {
            callback.onError(error, code);
        }
    }
    
    /** Lifecycle chatter, only with the verbose prop. Warnings and errors always log. */
    private void logV(String message) {
        if (verbose) Log.i(TAG, message);
    }
    
    //------------------------------------------------------------------------------
    // Public API
    //------------------------------------------------------------------------------
    
    public void setCallback(PlayerCallback callback) {
        this.callback = callback;
    }
    
    public void setAutoPlay(boolean autoPlay) {
        this.autoPlay = autoPlay;
    }
    
    public void setLoop(boolean loop) {
        this.loop = loop;
    }
    
    /**
     * Sets the playback rate, live.
     *
     * Stored as well as applied, because this arrives as a React prop and props are set before
     * a surface exists — without keeping it, a rate given at mount would be lost and only a
     * later change would ever be seen.
     *
     * Both backends are covered: single-stream files play through the C++ player, dual-stream
     * ones through {@link BufferedDualStreamPlayer}, and which one is in use is decided at load
     * time from the file itself. Neither moves the playhead when the rate changes.
     */
    public void setPlaybackSpeed(double playbackSpeed) {
        this.playbackSpeed = playbackSpeed > 0.01 ? playbackSpeed : 0.01;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.setPlaybackSpeed(this.playbackSpeed);
        } else if (playerId != 0 && !isReleased) {
            WebMAlphaPlayerNative.nativeSetPlaybackSpeed(playerId, this.playbackSpeed);
        }
    }
    
    /**
     * Set the playback mode.
     * - "preload": decode all frames upfront, then play
     * - "stream" (default): decode on-demand with sliding window cache
     * - "streamAndPreload": start playing immediately, cache all frames for smooth looping
     * Must be called before player is initialized (before surface is available)
     */
    public void setMode(String mode) {
        String oldMode = this.mode;
        if (MODE_PRELOAD.equals(mode) || MODE_STREAM_AND_PRELOAD.equals(mode)) {
            this.mode = mode;
        } else {
            this.mode = MODE_STREAM; // Default
        }
        logV("setMode called: " + oldMode + " -> " + this.mode + " (playerId=" + playerId + ")");
    }
    
    /**
     * Sets the decoder for the next load:
     * - "auto" (default): MediaCodec hardware decoders that list every track's profile, else
     *   MediaCodec's software decoders, else the C++ core
     * - "mediacodec": per track, a hardware decoder if one lists the profile and has an instance
     *   free, else a software one
     * - "mediacodec-hw" / "mediacodec-sw": hardware / software decoders only, for both tracks
     * - "software": the C++ core
     * A forced engine never falls back: a file it can't play reports onError. MediaCodec plays
     * only files with colour and alpha as separate VP8/VP9/AV1 bitstreams. The core decodes with
     * the codec modules installed ({@link AlphaVideoCodecs}): VP8 and 8-bit VP9 with alpha-video-vpx, 8- to
     * 12-bit AV1 with alpha-video-av1. A file needing one that's missing reports
     * {@link #ERROR_NO_DECODER}.
     */
    public void setDecoder(String decoder) {
        switch (decoder == null ? DECODER_AUTO : decoder) {
            case DECODER_MEDIACODEC:
            case DECODER_MEDIACODEC_HW:
            case DECODER_MEDIACODEC_SW:
            case DECODER_SOFTWARE:
                this.decoder = decoder;
                break;
            default:
                this.decoder = DECODER_AUTO;
        }
    }
    
    /**
     * @deprecated Use setMode() instead
     */
    public void setStream(boolean stream) {
        this.mode = stream ? MODE_STREAM : MODE_PRELOAD;
    }
    
    /**
     * Set how many frames to decode ahead of the shown one when streaming. Stream mode keeps
     * only these, so this is also its memory: each is a decoded YUVA frame, about 2.5 bytes a pixel.
     * Default: 8 frames (about 0.27 seconds at 30fps)
     */
    public void setStreamPrefetchAhead(int frames) {
        this.streamPrefetchAhead = frames > 0 ? frames : 8;
    }
    
    /**
     * Enable/disable global decode queue for preload/streamAndPreload modes.
     * When true (default): decoding is serialized across all players, preventing CPU saturation
     * When false: each player decodes independently (can freeze with many)
     * Has no effect when mode='stream'.
     */
    public void setUseDecodeQueue(boolean use) {
        this.useDecodeQueue = use;
    }
    
    /**
     * Enable/disable verbose per-frame logging.
     * When true: logs lifecycle events, performance stats, and every frame queue/release/render.
     * When false (default): only warnings and errors.
     */
    public void setVerbose(boolean verbose) {
        this.verbose = verbose;
        // Propagate to active dual-stream player if it exists
        if (bufferedPlayer != null) {
            bufferedPlayer.setVerbose(verbose);
        }
        if (playerId != 0 && !isReleased) {
            WebMAlphaPlayerNative.nativeSetVerbose(playerId, verbose);
        }
    }
    
    public void setPaused(boolean paused) {
        this.paused = paused;
        if (paused) {
            pause();
        } else if (autoPlay) {
            play();
        }
    }
    
    /**
     * Names what the store keeps a URL's bytes under, instead of the URL (for presigned URLs that
     * change); set it before or together with the source.
     */
    public void setCacheKey(String cacheKey) {
        this.cacheKey = cacheKey;
    }
    
    /** Sent with every request for an http(s) source, and given to a data source; null for none. */
    public void setHeaders(Map<String, String> headers) {
        this.headers = headers == null || headers.isEmpty()
            ? Collections.emptyMap() : Collections.unmodifiableMap(new LinkedHashMap<>(headers));
    }
    
    /**
     * Reads the source through the data source registered under this name with the range-source
     * add-on ({@code dev.mediacache.rangesource.RangeDataSources.register}), whatever its scheme;
     * null for the built-in ones.
     */
    public void setDataSource(String name) {
        this.dataSource = name == null || name.isEmpty() ? null : name;
    }
    
    /**
     * Where a URL's or data source's bytes are kept between plays: {@code "disk"} (in the cache
     * folder), {@code "memory"} (for the process's lifetime) or {@code "none"}. {@code "auto"}, the
     * default, is disk for URLs and none for a data source, whose caching is its own. The store is
     * partial: it keeps what has been played, and a clip held whole plays again offline.
     */
    public void setStorage(String storage) {
        switch (storage == null ? STORAGE_AUTO : storage) {
            case STORAGE_DISK:
            case STORAGE_MEMORY:
            case STORAGE_NONE:
                this.storage = storage;
                break;
            default:
                this.storage = STORAGE_AUTO;
        }
    }
    
    /** The cap of this player's store, over every clip in it, least recently played out first. */
    public void setMaxStorageBytes(long maxStorageBytes) {
        this.maxStorageBytes = maxStorageBytes > 0 ? maxStorageBytes : DEFAULT_MAX_STORAGE_BYTES;
    }
    
    /**
     * Sets the video: {@code https://} (read by range as it plays), {@code file://}, an absolute
     * path, {@code asset://} (read from the APK in place), {@code content://}, or with
     * {@link #setDataSource} any URI that data source reads.
     */
    public void setSource(String uri) {
        final int generation = ++sourceGeneration;
        sourceUri = uri;
        localSource = null;
        closeReader();
        // Props arriving in the same update (cacheKey, mode, ...) are applied after this one.
        post(() -> {
            if (generation == sourceGeneration) resolveSource(uri, generation);
        });
    }
    
    private static boolean isRemote(String uri) {
        return uri.startsWith("https://") || uri.startsWith("http://");
    }
    
    private void resolveSource(String uri, int generation) {
        final boolean appSource = dataSource != null;
        if (!appSource && !isRemote(uri)) {
            localSource = uri;
            loadLocalSource();
            return;
        }
        
        final boolean addOn = RangeSourceLink.available();
        if (!addOn) {
            if (appSource) {
                reportError("The data source \"" + dataSource + "\" needs the range-source add-on "
                    + "(dev.mediacache:range-source; for React Native, react-native-range-source), which the app "
                    + "doesn't include", ERROR_NO_SOURCE_MODULE);
                return;
            }
            warnWithoutAddOn();
        }

        final Fetcher source;
        if (appSource) {
            source = AppDataSource.create(dataSource);
            if (source == null) {
                reportError("No data source is registered as \"" + dataSource + "\" (RangeDataSources.register)");
                return;
            }
        } else {
            source = new HttpFetcher();
        }
        final SourceReader.Storage readerStorage = storageFor(appSource);
        final String storeDir = readerStorage == SourceReader.Storage.DISK
            ? new File(getContext().getCacheDir(), STORE_NAME).getPath() : STORE_NAME;
        final String key = cacheKey != null && !cacheKey.isEmpty() ? cacheKey : uri;
        final Map<String, String> requestHeaders = headers;
        final long maxBytes = maxStorageBytes;
        final String downloadDir = getContext().getCacheDir().getPath();
        // The stream modes start on the first bytes. Preload decodes the whole clip before
        // showing anything, and would hold the global decode queue for the whole transfer.
        final boolean fetchFirst = MODE_PRELOAD.equals(mode);
        probeExecutor.execute(() -> {
            SourceReader opened = null;
            String error = null;
            try {
                opened = SourceReader.open(source, Uri.parse(uri), requestHeaders, readerStorage, storeDir, key, maxBytes,
                    downloadDir);
                if (fetchFirst) opened.fetchAll();
            } catch (IOException e) {
                error = e.getMessage();
                if (opened != null) opened.close();
                opened = null;
            }
            final SourceReader result = opened;
            final String failure = error;
            post(() -> {
                if (generation != sourceGeneration) {
                    if (result != null) result.close();
                    return;
                }
                if (result == null) {
                    reportError("Loading " + uri + " failed: " + failure);
                    return;
                }
                reader = result;
                localSource = uri;
                loadLocalSource();
            });
        });
    }
    
    private SourceReader.Storage storageFor(boolean appSource) {
        switch (storage) {
            case STORAGE_DISK: return SourceReader.Storage.DISK;
            case STORAGE_MEMORY: return SourceReader.Storage.MEMORY;
            case STORAGE_NONE: return SourceReader.Storage.NONE;
            default: return appSource ? SourceReader.Storage.NONE : SourceReader.Storage.DISK;
        }
    }
    
    private void warnWithoutAddOn() {
        if (warnedWithoutAddOn.compareAndSet(false, true)) {
            Log.w(TAG, "range-source isn't installed (dev.mediacache:range-source; for React Native, "
                + "npm i react-native-range-source): URLs are downloaded whole for each play, without range "
                + "requests, storage, maxStorageBytes or cacheKey");
        }
    }

    /** Drops this view's reference; a player still reading keeps the reader until it unloads. */
    private void closeReader() {
        if (reader != null) {
            reader.close();
            reader = null;
        }
    }
    
    /** Loads {@link #localSource} if there is one and the surface is ready; otherwise waits for it. */
    private void loadLocalSource() {
        String uri = localSource;
        if (uri == null) return;
        if (!glReady) {
            logV("Deferring load until the surface is ready: " + uri);
            return;
        }
        if (reader != null) {
            probeReader(uri, reader);
            return;
        }
        
        final WebMSource source;
        final int[] colorAlphaTracks;
        try {
            source = WebMSource.parse(uri);
            try (AssetFileDescriptor afd = source.open(getContext())) {
                // Colour and alpha as separate VP8/VP9/AV1 bitstreams (2 tracks, or VP8/VP9 alpha
                // in BlockAdditional) decode on MediaCodec; anything else on the software decoder
                colorAlphaTracks = BufferedDualStreamPlayer.nativeColorAlphaTracks(
                    afd.getParcelFileDescriptor().getFd(), afd.getStartOffset(), afd.getLength());
            }
        } catch (IllegalArgumentException | IOException e) {
            reportError(e.getMessage());
            return;
        }
        loadProbedSource(source, colorAlphaTracks);
    }
    
    private void probeReader(String uri, SourceReader sourceReader) {
        final int generation = sourceGeneration;
        final long reference = sourceReader.retain();
        if (reference == 0) return;
        probeExecutor.execute(() -> {
            int[] colorAlphaTracks;
            try {
                colorAlphaTracks = BufferedDualStreamPlayer.nativeColorAlphaTracksReader(reference);
            } finally {
                SourceReader.release(reference);
            }
            post(() -> {
                if (generation != sourceGeneration || sourceReader != reader || isReleased || !glReady) return;
                loadProbedSource(WebMSource.reader(uri, sourceReader), colorAlphaTracks);
            });
        });
    }
    
    /** @param colorAlphaTracks from {@link BufferedDualStreamPlayer#nativeColorAlphaTracks}; null if none */
    private void loadProbedSource(WebMSource source, int[] colorAlphaTracks) {
        logV("Source " + source + " isDualStream=" + (colorAlphaTracks != null) + " decoder=" + decoder);
        boolean forcedMediaCodec = decoder.startsWith(DECODER_MEDIACODEC);
        softwareFallbackReason = null;
        
        if (colorAlphaTracks == null) {
            if (forcedMediaCodec) {
                reportError("decoder \"" + decoder + "\" needs colour and alpha as separate VP8, VP9 or AV1 "
                    + "bitstreams, and " + source + " has none");
            } else {
                loadSingleStream(source);
            }
            return;
        }
        if (DECODER_SOFTWARE.equals(decoder)) {
            loadSingleStream(source);
            return;
        }
        
        // This view's previous player gives back its decoders before the new file reserves some
        if (bufferedPlayer != null) {
            bufferedPlayer.unload();
            bufferedPlayer = null;
        }
        String[] names;
        switch (decoder) {
            case DECODER_MEDIACODEC_HW:
                names = MediaCodecDecoders.reserve(colorAlphaTracks, MediaCodecDecoders.Kind.HARDWARE);
                break;
            case DECODER_MEDIACODEC_SW:
                names = MediaCodecDecoders.reserve(colorAlphaTracks, MediaCodecDecoders.Kind.SOFTWARE);
                break;
            case DECODER_MEDIACODEC:
                names = MediaCodecDecoders.reserve(colorAlphaTracks, MediaCodecDecoders.Kind.ANY);
                break;
            default:
                names = MediaCodecDecoders.reserve(colorAlphaTracks, MediaCodecDecoders.Kind.HARDWARE);
                if (names == null) {
                    names = MediaCodecDecoders.reserve(colorAlphaTracks, MediaCodecDecoders.Kind.SOFTWARE);
                }
                if (names == null) {
                    logV("No MediaCodec decoders free that list the profiles; decoding in software");
                    softwareFallbackReason = "No MediaCodec decoders free that list the profiles of " + source;
                    loadSingleStream(source);
                    return;
                }
        }
        if (names == null) {
            reportError("No MediaCodec decoders free that list the profiles of " + source
                + " (decoder \"" + decoder + "\")");
            return;
        }
        loadDualStream(source, names);
    }
    
    /**
     * Load a dual-stream WebM using MediaCodec decoding.
     * Uses direct OES texture compositing — no FBO ring buffer.
     *
     * @param decoderNames reserved by {@link MediaCodecDecoders#reserve} for this load, which the
     *     new player then holds
     */
    private void loadDualStream(WebMSource source, String[] decoderNames) {
        logV("Loading dual-stream source with direct OES pipeline: " + source);
        
        isDualStreamMode = true;
        
        // Release any existing single-stream player
        if (playerId != 0) {
            WebMAlphaPlayerNative.nativeDestroy(playerId);
            playerId = 0;
        }
        
        // Release any existing player
        if (bufferedPlayer != null) {
            bufferedPlayer.unload();
            bufferedPlayer = null;
        }
        
        // Create player — must be done on GL thread for OES texture creation
        renderThread.execute(() -> {
            if (isReleased) {
                MediaCodecDecoders.release(decoderNames);
                return;
            }
            
            bufferedPlayer = new BufferedDualStreamPlayer();
            bufferedPlayer.useDecoders(decoderNames);
            bufferedPlayer.setLoop(loop);
            bufferedPlayer.setVerbose(verbose);
            // A rate set as a prop arrived long before this player existed; apply it here or
            // the first render would run at 1x regardless of what was asked for.
            bufferedPlayer.setPlaybackSpeed(playbackSpeed);
            
            // GL executor posts tasks to the render thread
            bufferedPlayer.setGLExecutor(task -> {
                if (renderThread != null && !renderThread.isShutdown()) {
                    renderThread.execute(task);
                }
            });
            
            bufferedPlayer.setEventCallback(new BufferedDualStreamPlayer.EventCallback() {
                @Override
                public void onLoaded(int totalFrames, int width, int height, double fps, double duration) {
                    logV("Dual-stream loaded (OES): " + totalFrames + " frames, " + 
                          width + "x" + height + " @ " + String.format("%.1f", fps) + " fps");
                    post(() -> {
                        if (callback != null && !isReleased) {
                            callback.onLoad(totalFrames, width, height, MediaCodecDecoders.describe(decoderNames));
                        }
                        if (autoPlay && !paused && bufferedPlayer != null) {
                            bufferedPlayer.play();
                        }
                    });
                }
                
                @Override
                public void onError(String error) {
                    post(() -> reportError(error));
                }
                
                @Override
                public void onLooped() {
                    logV("Dual-stream player looped");
                }
                
                @Override
                public void onEnded() {
                    post(() -> {
                        if (callback != null && !isReleased) {
                            callback.onEnd();
                        }
                    });
                }
            });
            
            // Render callback — playback loop signals when it's time to present a frame
            bufferedPlayer.setRenderCallback((frameIndex) -> {
                if (isReleased) return;
                requestRender();
            });
            
            // Load the file (creates OES textures in this GL context)
            boolean loaded;
            if (source.reader() != null) {
                loaded = bufferedPlayer.loadFromReader(source.reader());
            } else {
                try (AssetFileDescriptor afd = source.open(getContext())) {
                    loaded = bufferedPlayer.loadFromFd(
                        afd.getParcelFileDescriptor().getFd(), afd.getStartOffset(), afd.getLength());
                } catch (IOException e) {
                    loaded = false;
                }
            }
            if (!loaded) {
                post(() -> reportError("Failed to load dual-stream video " + source));
            }
        });
    }
    
    /**
     * Load a single-stream WebM (VP9 with alpha) using software decoding.
     */
    private void loadSingleStream(WebMSource source) {
        logV("Loading single-stream source with software decoder: " + source);
        
        isDualStreamMode = false;
        
        // Release any existing dual-stream player
        if (bufferedPlayer != null) {
            bufferedPlayer.unload();
            bufferedPlayer = null;
        }
        
        // Initialize software player if needed - by now all React props should be set
        if (playerId == 0 && !isReleased) {
            initializePlayer();
        }
        
        if (isReleased || playerId == 0) {
            Log.w(TAG, "Cannot load - player not initialized");
            return;
        }
        
        final Context context = getContext();
        enqueueNativeLoad(() -> {
            SourceReader sourceReader = source.reader();
            if (sourceReader != null) {
                long reference = sourceReader.retain();
                if (reference == 0) return false;
                try {
                    return WebMAlphaPlayerNative.nativeLoadFromReader(playerId, reference);
                } finally {
                    SourceReader.release(reference);
                }
            }
            try (AssetFileDescriptor afd = source.open(context)) {
                return WebMAlphaPlayerNative.nativeLoadFromFd(playerId,
                    afd.getParcelFileDescriptor().getFd(), afd.getStartOffset(), afd.getLength());
            }
        });
    }
    
    public void loadFromBytes(byte[] data) {
        ++sourceGeneration;
        sourceUri = null;
        localSource = null;
        closeReader();
        
        if (isReleased || playerId == 0) {
            Log.w(TAG, "Cannot load from bytes before player is initialized");
            return;
        }
        
        enqueueNativeLoad(() -> WebMAlphaPlayerNative.nativeLoadFromBytes(playerId, data));
    }
    
    public void play() {
        if (isReleased) return;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.play();
        } else if (playerId != 0) {
            WebMAlphaPlayerNative.nativePlay(playerId);
        }
    }
    
    public void pause() {
        if (isReleased) return;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.pause();
        } else if (playerId != 0) {
            WebMAlphaPlayerNative.nativePause(playerId);
        }
    }
    
    public void stop() {
        if (isReleased) return;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.stop();
        } else if (playerId != 0) {
            WebMAlphaPlayerNative.nativeStop(playerId);
        }
    }
    
    /**
     * Moves to a frame (clamped to the clip), staying playing or paused; a paused view shows it.
     * Ignored until the clip has loaded.
     */
    public void seekToFrame(int frame) {
        if (isReleased) return;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.seekToFrame(frame);
        } else if (playerId != 0) {
            WebMAlphaPlayerNative.nativeSeekToFrame(playerId, frame);
        }
    }
    
    /** Moves to the frame showing at {@code seconds}; see {@link #seekToFrame}. */
    public void seekToTime(double seconds) {
        if (isReleased) return;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.seekToTime(seconds);
        } else if (playerId != 0) {
            WebMAlphaPlayerNative.nativeSeekToTime(playerId, seconds);
        }
    }
    
    public boolean isLoaded() {
        if (isReleased) return false;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            return bufferedPlayer.isLoaded();
        } else if (playerId != 0) {
            return WebMAlphaPlayerNative.nativeIsLoaded(playerId);
        }
        return false;
    }
    
    public boolean isPlaying() {
        if (isReleased) return false;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            return bufferedPlayer.isPlaying();
        } else if (playerId != 0) {
            return WebMAlphaPlayerNative.nativeIsPlaying(playerId);
        }
        return false;
    }
    
    public int getTotalFrames() {
        if (isReleased) return 0;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            return bufferedPlayer.getTotalFrames();
        } else if (playerId != 0) {
            return WebMAlphaPlayerNative.nativeGetTotalFrames(playerId);
        }
        return 0;
    }
    
    public int getVideoWidth() {
        if (isReleased) return 0;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            return bufferedPlayer.getWidth();
        } else if (playerId != 0) {
            return WebMAlphaPlayerNative.nativeGetWidth(playerId);
        }
        return 0;
    }
    
    public int getVideoHeight() {
        if (isReleased) return 0;
        
        if (isDualStreamMode && bufferedPlayer != null) {
            return bufferedPlayer.getHeight();
        } else if (playerId != 0) {
            return WebMAlphaPlayerNative.nativeGetHeight(playerId);
        }
        return 0;
    }
    
    //------------------------------------------------------------------------------
    // SurfaceTextureListener
    //------------------------------------------------------------------------------
    
    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
        logV("Surface available: " + width + "x" + height);
        this.surfaceTexture = surface;
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        surfaceAvailable.set(true);
        
        // DON'T initialize player here - wait until we load a source
        // This ensures all React props (including mode) are set before player creation
        
        // Create render thread if needed (for EGL init)
        if (renderThread == null || renderThread.isShutdown()) {
            renderThread = Executors.newSingleThreadExecutor(r -> {
                Thread t = new Thread(r, "WebMGLRender");
                t.setPriority(Thread.MAX_PRIORITY);
                return t;
            });
        }
        
        // Initialize EGL on render thread, then reload source
        renderThread.execute(() -> {
            initEGL();
            
            // Loading dispatches to renderThread internally, so post to the main thread
            // to avoid deadlocking the single-threaded executor
            post(() -> {
                glReady = true;
                if (isReleased || localSource == null) return;
                waitForSoftRelease();
                loadLocalSource();
            });
        });
    }
    
    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {
        logV("Surface size changed: " + width + "x" + height);
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
        logV("Surface destroyed");
        surfaceAvailable.set(false);
        glReady = false;
        
        // Store reference before nulling so we can release it async
        final SurfaceTexture surfaceToRelease = this.surfaceTexture;
        this.surfaceTexture = null;
        
        // Clean up EGL on render thread WITHOUT blocking main thread
        // This prevents ANRs when multiple players are destroyed simultaneously
        renderThread.execute(() -> {
            try {
                destroyEGL();
            } catch (Exception e) {
                Log.e(TAG, "Error during EGL cleanup: " + e.getMessage());
            }
        });
        
        // Return false - we'll manage SurfaceTexture lifecycle ourselves to avoid blocking
        // The render thread will clean up EGL and we don't need the SurfaceTexture after that
        return true;
    }
    
    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surface) {
        // Called when updateTexImage() is called, usually nothing to do here
    }
    
    //------------------------------------------------------------------------------
    // EGL Setup (runs on render thread)
    //------------------------------------------------------------------------------
    
    /** Picks an RGBA8 window config of the renderable type and creates a context of that version. */
    private boolean createContext(int renderableType, int clientVersion) {
        int[] configAttribs = {
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, renderableType,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_NONE
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] numConfigs = new int[1];
        if (!EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfigs, 0) || numConfigs[0] == 0) {
            return false;
        }
        int[] contextAttribs = { EGL14.EGL_CONTEXT_CLIENT_VERSION, clientVersion, EGL14.EGL_NONE };
        EGLContext context = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, contextAttribs, 0);
        if (context == EGL14.EGL_NO_CONTEXT) return false;
        eglConfig = configs[0];
        eglContext = context;
        return true;
    }

    private void initEGL() {
        logV("Initializing EGL");
        
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
        
        // ES 3 lets the dual-stream shader read raw YUV (GL_EXT_YUV_target); ES 2 otherwise
        glesVersion = 3;
        if (!createContext(EGLExt.EGL_OPENGL_ES3_BIT_KHR, 3)) {
            glesVersion = 2;
            if (!createContext(EGL14.EGL_OPENGL_ES2_BIT, 2)) {
                Log.e(TAG, "Unable to create EGL context");
                return;
            }
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
        
        logV("EGL initialized successfully");
    }
    
    private void destroyEGL() {
        logV("Destroying EGL");
        
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
        yuvOesProgramId = 0;
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
        
        // Create YUVA shader program (for ByteBuffer mode - software decoded)
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
            "uniform mat4 uYuvToRgb;\n" +
            "void main() {\n" +
            "    vec4 yuv = vec4(texture2D(yTexture, vTexCoord).r, texture2D(uTexture, vTexCoord).r,\n" +
            "                    texture2D(vTexture, vTexCoord).r, 1.0);\n" +
            "    float a = texture2D(aTexture, vTexCoord).r;\n" +
            "    vec3 rgb = clamp((uYuvToRgb * yuv).rgb, 0.0, 1.0);\n" +
            "    gl_FragColor = vec4(rgb * a, a);\n" +
            "}\n";
        
        programId = createProgram(vertexShader, fragmentShader);
        
        // Create OES shader program (for Surface mode - GPU accelerated, zero-copy)
        // Uses external OES textures which stay on GPU
        String oesVertexShader = 
            "attribute vec4 aPosition;\n" +
            "attribute vec2 aTexCoord;\n" +
            "uniform mat4 uColorSTMatrix;\n" +
            "uniform mat4 uAlphaSTMatrix;\n" +
            "varying vec2 vColorTexCoord;\n" +
            "varying vec2 vAlphaTexCoord;\n" +
            "void main() {\n" +
            "    gl_Position = aPosition;\n" +
            "    vColorTexCoord = (uColorSTMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;\n" +
            "    vAlphaTexCoord = (uAlphaSTMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;\n" +
            "}\n";
        
        // OES fragment shader - samples from external textures (MediaCodec output)
        // Color texture is YUV, alpha texture Y channel contains alpha
        String oesFragmentShader = 
            "#extension GL_OES_EGL_image_external : require\n" +
            "precision mediump float;\n" +
            "varying vec2 vColorTexCoord;\n" +
            "varying vec2 vAlphaTexCoord;\n" +
            "uniform samplerExternalOES uColorTexture;\n" +
            "uniform samplerExternalOES uAlphaTexture;\n" +
            "void main() {\n" +
            "    vec4 color = texture2D(uColorTexture, vColorTexCoord);\n" +
            "    float alpha = texture2D(uAlphaTexture, vAlphaTexCoord).r;\n" +
            "    gl_FragColor = vec4(color.rgb * alpha, alpha);\n" +
            "}\n";
        
        oesProgramId = createProgram(oesVertexShader, oesFragmentShader);
        if (oesProgramId == 0) {
            Log.w(TAG, "OES shader creation failed - Surface mode will not work");
        } else {
            logV("OES shader created (GPU-accelerated compositing)");
        }

        String extensions = GLES20.glGetString(GLES20.GL_EXTENSIONS);
        if (glesVersion >= 3 && extensions != null && extensions.contains("GL_EXT_YUV_target")) {
            String yuvOesVertexShader =
                "#version 300 es\n" +
                "in vec4 aPosition;\n" +
                "in vec2 aTexCoord;\n" +
                "uniform mat4 uColorSTMatrix;\n" +
                "uniform mat4 uAlphaSTMatrix;\n" +
                "out vec2 vColorTexCoord;\n" +
                "out vec2 vAlphaTexCoord;\n" +
                "void main() {\n" +
                "    gl_Position = aPosition;\n" +
                "    vColorTexCoord = (uColorSTMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;\n" +
                "    vAlphaTexCoord = (uAlphaSTMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;\n" +
                "}\n";
            String yuvOesFragmentShader =
                "#version 300 es\n" +
                "#extension GL_EXT_YUV_target : require\n" +
                "precision highp float;\n" +
                "in vec2 vColorTexCoord;\n" +
                "in vec2 vAlphaTexCoord;\n" +
                "uniform __samplerExternal2DY2YEXT uColorTexture;\n" +
                "uniform __samplerExternal2DY2YEXT uAlphaTexture;\n" +
                "uniform mat4 uYuvToRgb;\n" +
                "uniform vec2 uAlphaRange;\n" +
                "out vec4 fragColor;\n" +
                "void main() {\n" +
                "    vec3 yuv = texture(uColorTexture, vColorTexCoord).xyz;\n" +
                "    vec3 rgb = clamp((uYuvToRgb * vec4(yuv, 1.0)).rgb, 0.0, 1.0);\n" +
                "    float y = texture(uAlphaTexture, vAlphaTexCoord).x;\n" +
                "    float alpha = clamp(uAlphaRange.x * (y - uAlphaRange.y), 0.0, 1.0);\n" +
                "    fragColor = vec4(rgb * alpha, alpha);\n" +
                "}\n";
            yuvOesProgramId = createLinkedProgram(yuvOesVertexShader, yuvOesFragmentShader);
            logV("Raw YUV OES shader " + (yuvOesProgramId != 0 ? "created" : "failed; the driver converts"));
        }
        
        // Create textures for YUVA mode
        int[] textures = new int[4];
        GLES20.glGenTextures(4, textures, 0);
        yTextureId = textures[0];
        uTextureId = textures[1];
        vTextureId = textures[2];
        aTextureId = textures[3];
        
        // Set up vertex buffer (fullscreen quad with flipped Y for TextureView/YUVA mode)
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
        
        // Set up OES vertex buffer with UN-FLIPPED texture coordinates
        // SurfaceTexture's transformation matrix will handle the flip
        float[] oesVertices = {
            -1f, -1f, 0f, 0f,  // bottom-left  (tex coord: 0,0)
             1f, -1f, 1f, 0f,  // bottom-right (tex coord: 1,0)
            -1f,  1f, 0f, 1f,  // top-left     (tex coord: 0,1)
             1f,  1f, 1f, 1f   // top-right    (tex coord: 1,1)
        };
        oesVertexBuffer = ByteBuffer.allocateDirect(oesVertices.length * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .put(oesVertices);
        oesVertexBuffer.position(0);
        
        // Initialize identity matrices for OES
        android.opengl.Matrix.setIdentityM(colorTransformMatrix, 0);
        android.opengl.Matrix.setIdentityM(alphaTransformMatrix, 0);
        
        // Enable blending for transparency
        GLES20.glEnable(GLES20.GL_BLEND);
        GLES20.glBlendFunc(GLES20.GL_ONE, GLES20.GL_ONE_MINUS_SRC_ALPHA);
        
        // Set viewport
        GLES20.glViewport(0, 0, surfaceWidth, surfaceHeight);
        
        logV("OpenGL initialized");
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
    
    /** createProgram, but 0 (logged) when a shader doesn't compile or the program doesn't link. */
    private int createLinkedProgram(String vertexSource, String fragmentSource) {
        int program = createProgram(vertexSource, fragmentSource);
        int[] status = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0);
        if (status[0] != 0) return program;
        Log.w(TAG, "Shader program failed to link: " + GLES20.glGetProgramInfoLog(program));
        GLES20.glDeleteProgram(program);
        return 0;
    }

    private int loadShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        int[] status = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0);
        if (status[0] == 0) Log.w(TAG, "Shader failed to compile: " + GLES20.glGetShaderInfoLog(shader));
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
            
            // Dual-stream mode — composite directly from OES textures
            if (isDualStreamMode && bufferedPlayer != null) {
                doRenderDirectOes();
                return;
            }
            
            // ByteBuffer mode (YUVA textures) - for single-stream files
            doRenderYuva();
            
        } catch (Exception e) {
            Log.w(TAG, "Render exception: " + e.getMessage());
        }
    }
    
    /**
     * Render by compositing directly from OES textures (SurfaceTexture output).
     * Single GL draw call: fragment shader samples both color and alpha OES textures,
     * multiplies color by alpha, and outputs premultiplied RGBA.
     * No intermediate FBO — the SurfaceTexture IS the buffer.
     */
    private void doRenderDirectOes() {
        if (bufferedPlayer == null || oesProgramId == 0) {
            EGL14.eglSwapBuffers(eglDisplay, eglSurface);
            return;
        }
        
        // Consume frames only when BOTH decoders have released a new frame.
        // This ensures color and alpha are always from the same temporal position.
        // If consumeFrames() returns false, we re-render the previous frame (OES
        // textures retain their content), which is visually smooth.
        bufferedPlayer.consumeFrames();
        
        int colorTexId = bufferedPlayer.getColorOesTextureId();
        int alphaTexId = bufferedPlayer.getAlphaOesTextureId();
        
        if (colorTexId <= 0 || alphaTexId <= 0) {
            EGL14.eglSwapBuffers(eglDisplay, eglSurface);
            return;
        }
        
        float[] colorYuvToRgb = bufferedPlayer.getColorYuvToRgb();
        float[] alphaRange = bufferedPlayer.getAlphaRange();
        boolean rawYuv = yuvOesProgramId != 0 && colorYuvToRgb != null && alphaRange != null;
        int program = rawYuv ? yuvOesProgramId : oesProgramId;
        GLES20.glUseProgram(program);
        
        // Get attribute/uniform locations
        int positionHandle = GLES20.glGetAttribLocation(program, "aPosition");
        int texCoordHandle = GLES20.glGetAttribLocation(program, "aTexCoord");
        int colorSTMatrixHandle = GLES20.glGetUniformLocation(program, "uColorSTMatrix");
        int alphaSTMatrixHandle = GLES20.glGetUniformLocation(program, "uAlphaSTMatrix");
        int colorTextureHandle = GLES20.glGetUniformLocation(program, "uColorTexture");
        int alphaTextureHandle = GLES20.glGetUniformLocation(program, "uAlphaTexture");
        if (rawYuv) {
            GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "uYuvToRgb"), 1, false, colorYuvToRgb, 0);
            GLES20.glUniform2fv(GLES20.glGetUniformLocation(program, "uAlphaRange"), 1, alphaRange, 0);
        }
        
        // Set vertex attributes (un-flipped — SurfaceTexture transform handles orientation)
        oesVertexBuffer.position(0);
        GLES20.glVertexAttribPointer(positionHandle, 2, GLES20.GL_FLOAT, false, 16, oesVertexBuffer);
        GLES20.glEnableVertexAttribArray(positionHandle);
        
        oesVertexBuffer.position(2);
        GLES20.glVertexAttribPointer(texCoordHandle, 2, GLES20.GL_FLOAT, false, 16, oesVertexBuffer);
        GLES20.glEnableVertexAttribArray(texCoordHandle);
        
        // Set SurfaceTexture transform matrices
        GLES20.glUniformMatrix4fv(colorSTMatrixHandle, 1, false, bufferedPlayer.getColorTransformMatrix(), 0);
        GLES20.glUniformMatrix4fv(alphaSTMatrixHandle, 1, false, bufferedPlayer.getAlphaTransformMatrix(), 0);
        
        // Bind OES textures
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, colorTexId);
        GLES20.glUniform1i(colorTextureHandle, 0);
        
        GLES20.glActiveTexture(GLES20.GL_TEXTURE1);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, alphaTexId);
        GLES20.glUniform1i(alphaTextureHandle, 1);
        
        // Draw fullscreen quad
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        
        // Cleanup
        GLES20.glDisableVertexAttribArray(positionHandle);
        GLES20.glDisableVertexAttribArray(texCoordHandle);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE1);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0);
        
        // Swap buffers
        EGL14.eglSwapBuffers(eglDisplay, eglSurface);
    }
    
    /**
     * Render using YUVA textures (ByteBuffer mode - slower, CPU upload).
     * This is the fallback path for single-stream or legacy dual-stream.
     */
    private void doRenderYuva() {
        // Grab frame data under lock
        ByteBuffer y, u, v, a;
        int width, height;
        synchronized (frameDataLock) {
            // Two requests can arrive for one frame (a seek as the preload finishes); swapping
            // the cleared buffer for the second would blank a paused view.
            if (isReleased || !hasNewFrame || pendingY == null) {
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
        
        // U texture (4:2:0, rounded up for odd sizes)
        int uvWidth = (width + 1) / 2;
        int uvHeight = (height + 1) / 2;
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
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(programId, "uYuvToRgb"), 1, false, yuvToRgb, 0);
        
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        
        // Swap buffers
        EGL14.eglSwapBuffers(eglDisplay, eglSurface);
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
        
        logV("Loaded: " + totalFrames + " frames, " + width + "x" + height);
        float[] clipYuvToRgb = WebMAlphaPlayerNative.nativeGetYuvToRgb(playerId);
        if (clipYuvToRgb != null) yuvToRgb = clipYuvToRgb;
        String decoderName = WebMAlphaPlayerNative.nativeGetDecoderName(playerId);
        
        post(() -> {
            if (isReleased) return;
            
            if (callback != null) {
                callback.onLoad(totalFrames, width, height, "software (" + decoderName + ")");
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
                        logV("First frame rendered via TextureView pipeline");
                    }
                }
            }
            
            if (autoPlay && !paused && playerId != 0 && !isReleased) {
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
        logV("Playing");
    }
    
    @Override
    public void onPause() {
        if (isReleased) return;
        logV("Paused");
    }
    
    @Override
    public void onStop() {
        if (isReleased) return;
        logV("Stopped");
    }
    
    @Override
    public void onLoop() {
        if (isReleased) return;
        logV("Looped");
    }
    
    @Override
    public void onEnd() {
        if (isReleased) return;
        post(() -> {
            if (callback != null && !isReleased) {
                callback.onEnd();
            }
        });
    }
    
    @Override
    public void onError(String error, String code) {
        if (isReleased) return;
        post(() -> {
            String reason = softwareFallbackReason;
            reportError(ERROR_NO_DECODER.equals(code) && reason != null ? reason + "; " + error : error, code);
        });
    }
    
    @Override
    public void onPreloadComplete(int totalFrames, double timeMs) {
        if (isReleased) return;
        logV("Preload complete: " + totalFrames + " frames in " + timeMs + "ms");
        post(() -> {
            if (callback != null && !isReleased) {
                callback.onPreloadComplete(totalFrames, timeMs);
            }
        });
    }
    
    //------------------------------------------------------------------------------
    // Lifecycle
    //------------------------------------------------------------------------------
    
    public void release() {
        logV("Full release of player " + playerId + " (dualStream=" + isDualStreamMode + ")");
        
        final long pid = playerId;
        playerId = 0;
        isReleased = true;
        // A reader still opening is closed when it arrives
        ++sourceGeneration;
        closeReader();
        
        cancelNativeLoad();
        
        // Release buffered dual-stream player if in use
        if (bufferedPlayer != null) {
            bufferedPlayer.unload();
            bufferedPlayer = null;
        }
        
        isDualStreamMode = false;
        
        // Clean up native player on executor
        if (pid != 0) {
            synchronized (nativeLoadLock) {
                if (nativeLoadExecutor != null && !nativeLoadExecutor.isShutdown()) {
                    nativeLoadExecutor.submit(() -> {
                        WebMAlphaPlayerNative.nativeDestroy(pid);
                        logV("Native player destroyed: " + pid);
                    });
                    nativeLoadExecutor.shutdown();
                } else {
                    new Thread(() -> {
                        WebMAlphaPlayerNative.nativeDestroy(pid);
                        logV("Native player destroyed (bg): " + pid);
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
        logV("Soft release of player " + playerId + " (dualStream=" + isDualStreamMode + ")");
        
        cancelNativeLoad();
        
        synchronized (frameDataLock) {
            isReleased = true;
            hasNewFrame = false;
            pendingY = pendingU = pendingV = pendingA = null;
            currentWidth = 0;
            currentHeight = 0;
        }
        
        // Soft release buffered dual-stream player
        if (isDualStreamMode && bufferedPlayer != null) {
            bufferedPlayer.stop();
            bufferedPlayer.unload();
            bufferedPlayer = null;
            logV("Soft release cleanup complete for buffered dual-stream player");
        }
        
        final long pid = playerId;
        if (pid != 0) {
            synchronized (nativeLoadLock) {
                if (nativeLoadExecutor == null || nativeLoadExecutor.isShutdown()) {
                    nativeLoadExecutor = createLoadExecutor();
                }
                // Track this task so we can wait for it before reloading
                softReleaseTask = nativeLoadExecutor.submit(() -> {
                    WebMAlphaPlayerNative.nativeStop(pid);
                    WebMAlphaPlayerNative.nativeUnload(pid);
                    logV("Soft release cleanup complete for player " + pid);
                });
            }
        }
    }
    
    /**
     * Wait for any pending soft release to complete.
     * Must be called before reloading after a detach/reattach cycle.
     */
    private void waitForSoftRelease() {
        Future<?> task = softReleaseTask;
        if (task != null && !task.isDone()) {
            try {
                logV("Waiting for soft release to complete...");
                task.get(1000, java.util.concurrent.TimeUnit.MILLISECONDS);
                logV("Soft release completed");
            } catch (Exception e) {
                Log.w(TAG, "Soft release wait interrupted or timed out: " + e.getMessage());
            }
        }
        softReleaseTask = null;
    }
    
    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        logV("View attached (isReleased=" + isReleased + ", playerId=" + playerId +
              ", dualStream=" + isDualStreamMode + ")");
        
        // If we were previously released, reset the flag so the surface callback can re-load.
        // This covers both software decode (playerId != 0) and dual-stream (isDualStreamMode)
        // re-attach after z-index reordering.
        if (isReleased && (playerId != 0 || isDualStreamMode || sourceUri != null)) {
            isReleased = false;
            
            if (renderThread == null || renderThread.isShutdown()) {
                renderThread = createRenderThread();
            }
        }
    }
    
    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        logV("View detached");
        softRelease();
    }
    
    //------------------------------------------------------------------------------
    // Helpers
    //------------------------------------------------------------------------------
    
    /** Native load failures report themselves through {@link #onError}; only I/O errors are thrown. */
    private interface NativeLoadCommand {
        boolean run() throws IOException;
    }
    
    /**
     * Get or create the global decode queue.
     * This queue serializes non-stream decoding across all player instances.
     */
    private static ExecutorService getGlobalDecodeQueue() {
        synchronized (globalDecodeQueueLock) {
            if (globalDecodeQueue == null || globalDecodeQueue.isShutdown()) {
                globalDecodeQueue = Executors.newSingleThreadExecutor(r -> {
                    Thread t = new Thread(r, "WebMGlobalDecode");
                    t.setPriority(Thread.NORM_PRIORITY);
                    return t;
                });
            }
            globalDecodeQueueRefCount++;
            return globalDecodeQueue;
        }
    }
    
    /**
     * Release reference to global decode queue.
     * Queue is shut down when all references are released.
     */
    private static void releaseGlobalDecodeQueue() {
        synchronized (globalDecodeQueueLock) {
            globalDecodeQueueRefCount--;
            if (globalDecodeQueueRefCount <= 0 && globalDecodeQueue != null) {
                globalDecodeQueue.shutdown();
                globalDecodeQueue = null;
                globalDecodeQueueRefCount = 0;
            }
        }
    }
    
    private void enqueueNativeLoad(NativeLoadCommand command) {
        synchronized (nativeLoadLock) {
            if (isReleased || playerId == 0) {
                Log.w(TAG, "Ignoring load - player released");
                return;
            }
            
            // Decide which executor to use:
            // - Stream mode: use per-instance executor (streaming is already lightweight)
            // - Preload/StreamAndPreload + useDecodeQueue: use global queue to serialize decoding
            // - Preload/StreamAndPreload + !useDecodeQueue: use per-instance executor
            ExecutorService executor;
            boolean usingGlobalQueue = !MODE_STREAM.equals(mode) && useDecodeQueue;
            
            if (usingGlobalQueue) {
                executor = getGlobalDecodeQueue();
            } else {
                if (nativeLoadExecutor == null || nativeLoadExecutor.isShutdown()) {
                    nativeLoadExecutor = createLoadExecutor();
                }
                executor = nativeLoadExecutor;
            }
            
            cancelNativeLoadLocked();
            final long pid = playerId;
            final boolean usedGlobalQueue = usingGlobalQueue;
            currentLoadTask = executor.submit(() -> {
                try {
                    if (isReleased || playerId != pid) {
                        Log.w(TAG, "Load aborted - player released or changed");
                        return;
                    }
                    command.run();
                } catch (IOException e) {
                    post(() -> reportError(e.getMessage()));
                } finally {
                    // Release global queue reference when done
                    if (usedGlobalQueue) {
                        releaseGlobalDecodeQueue();
                    }
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
