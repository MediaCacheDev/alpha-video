package com.danielkpl2.webmalphaplayer;

import android.util.Log;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReadableMap;
import com.facebook.react.common.MapBuilder;
import com.facebook.react.uimanager.SimpleViewManager;
import com.facebook.react.uimanager.ThemedReactContext;
import com.facebook.react.uimanager.annotations.ReactProp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * React Native ViewManager for WebMAlphaTextureView
 * Uses TextureView for proper z-ordering and ScrollView compatibility
 * 
 * Props match WebMAlphaView interface:
 * - source: { uri: string } or { path: string }
 * - autoPlay: boolean (default true)
 * - loop: boolean (default true)
 * - paused: boolean (default false)
 * - cacheKey: string (optional custom cache path)
 */
public class RNWebMAlphaViewManager extends SimpleViewManager<WebMAlphaTextureView> {
    
    private static final String TAG = "RNWebMAlphaViewManager";
    public static final String REACT_CLASS = "RNWebMAlphaView";
    
    private final ReactApplicationContext reactContext;
    private final ExecutorService downloadExecutor = Executors.newFixedThreadPool(4);
    
    // Store custom cache keys per view instance
    private final WeakHashMap<WebMAlphaTextureView, String> viewCacheKeys = new WeakHashMap<>();
    
    public RNWebMAlphaViewManager(ReactApplicationContext context) {
        this.reactContext = context;
    }
    
    @NonNull
    @Override
    public String getName() {
        return REACT_CLASS;
    }
    
    @NonNull
    @Override
    protected WebMAlphaTextureView createViewInstance(@NonNull ThemedReactContext context) {
        Log.i(TAG, "Creating WebMAlphaTextureView with TextureView backend");
        return new WebMAlphaTextureView(context);
    }
    
    @Override
    public void onDropViewInstance(@NonNull WebMAlphaTextureView view) {
        Log.i(TAG, "Dropping view instance");
        view.release();
        super.onDropViewInstance(view);
    }
    
    @ReactProp(name = "source")
    public void setSource(WebMAlphaTextureView view, @Nullable ReadableMap source) {
        String sourceUri = null;
        if (source != null) {
            if (source.hasKey("uri") && !source.isNull("uri")) {
                sourceUri = source.getString("uri");
            } else if (source.hasKey("path") && !source.isNull("path")) {
                sourceUri = source.getString("path");
            }
        }
        
        if (sourceUri == null || sourceUri.isEmpty()) return;
        
        Log.d(TAG, "setSource: " + sourceUri);
        
        if (sourceUri.startsWith("http://") || sourceUri.startsWith("https://")) {
            downloadAndLoad(view, sourceUri);
        } else if (sourceUri.startsWith("file://")) {
            view.loadFromFile(sourceUri.substring(7));
        } else if (sourceUri.startsWith("asset://") || sourceUri.startsWith("asset:/")) {
            loadFromAsset(view, sourceUri);
        } else if (isBundledResourceName(sourceUri)) {
            // Load from bundled assets (release build)
            loadFromBundledAsset(view, sourceUri);
        } else {
            view.loadFromFile(sourceUri);
        }
    }
    
    /**
     * Check if the URI looks like a bundled asset resource name (from release build)
     * These look like: "src_assets_test_webm" (no slashes, no scheme, no dots except extension)
     */
    private boolean isBundledResourceName(String uri) {
        if (uri == null || uri.isEmpty()) return false;
        if (uri.contains("/") || uri.contains(":")) return false;
        // Check if it looks like a resource name (alphanumeric with underscores, starting with letter)
        return uri.matches("^[a-zA-Z][a-zA-Z0-9_]*$");
    }
    
    /**
     * Load WebM from bundled assets (for release build)
     * React Native bundles custom assets into the APK's res/raw folder or assets folder.
     */
    private void loadFromBundledAsset(WebMAlphaTextureView view, String resourceName) {
        downloadExecutor.execute(() -> {
            try {
                // First, try to find in assets folder
                String foundPath = findWebMAsset(resourceName, "");
                if (foundPath != null) {
                    // Copy from assets to cache (FFmpeg needs a real file path)
                    File cacheFile = new File(reactContext.getCacheDir(), 
                        "webm_bundled_" + resourceName.hashCode() + ".webm");
                    
                    if (!cacheFile.exists()) {
                        InputStream is = reactContext.getAssets().open(foundPath);
                        FileOutputStream fos = new FileOutputStream(cacheFile);
                        byte[] buffer = new byte[8192];
                        int read;
                        while ((read = is.read(buffer)) != -1) {
                            fos.write(buffer, 0, read);
                        }
                        fos.close();
                        is.close();
                        Log.d(TAG, "Copied bundled asset to cache: " + cacheFile.getAbsolutePath());
                    }
                    
                    view.post(() -> view.loadFromFile(cacheFile.getAbsolutePath()));
                    return;
                }
                
                // Fallback: try raw resources
                int resourceId = reactContext.getResources().getIdentifier(
                    resourceName, "raw", reactContext.getPackageName());
                if (resourceId != 0) {
                    File cacheFile = new File(reactContext.getCacheDir(), 
                        "webm_raw_" + resourceName.hashCode() + ".webm");
                    
                    if (!cacheFile.exists()) {
                        InputStream is = reactContext.getResources().openRawResource(resourceId);
                        FileOutputStream fos = new FileOutputStream(cacheFile);
                        byte[] buffer = new byte[8192];
                        int read;
                        while ((read = is.read(buffer)) != -1) {
                            fos.write(buffer, 0, read);
                        }
                        fos.close();
                        is.close();
                        Log.d(TAG, "Copied raw resource to cache: " + cacheFile.getAbsolutePath());
                    }
                    
                    view.post(() -> view.loadFromFile(cacheFile.getAbsolutePath()));
                    return;
                }
                
                Log.e(TAG, "Bundled asset not found: " + resourceName);
            } catch (Exception e) {
                Log.e(TAG, "Failed to load bundled asset: " + resourceName, e);
            }
        });
    }
    
    /**
     * Search assets folder for a .webm file matching the resource name
     */
    private String findWebMAsset(String resourceName, String path) {
        try {
            String[] files = reactContext.getAssets().list(path);
            if (files == null) return null;
            
            // Extract a search key from the resource name
            String searchKey = resourceName.toLowerCase().replace("_", "");
            
            for (String file : files) {
                String fullPath = path.isEmpty() ? file : path + "/" + file;
                
                if (file.endsWith(".webm")) {
                    // Normalize filename for comparison
                    String normalized = file.replace(".webm", "").replace("-", "").replace("_", "").toLowerCase();
                    if (normalized.contains(searchKey) || searchKey.contains(normalized) ||
                        resourceName.toLowerCase().contains(file.replace(".webm", "").toLowerCase())) {
                        Log.d(TAG, "Found WebM asset: " + fullPath + " for resource: " + resourceName);
                        return fullPath;
                    }
                }
                
                // Recurse into directories
                String[] subFiles = reactContext.getAssets().list(fullPath);
                if (subFiles != null && subFiles.length > 0) {
                    String found = findWebMAsset(resourceName, fullPath);
                    if (found != null) return found;
                }
            }
        } catch (Exception ignored) {}
        return null;
    }
    
    private void loadFromAsset(WebMAlphaTextureView view, String assetUri) {
        final String assetPath = assetUri.replace("asset://", "").replace("asset:/", "")
            .replaceFirst("^/", "");
        
        downloadExecutor.execute(() -> {
            try {
                File tempFile = new File(reactContext.getCacheDir(), "webm_asset_" + assetPath.hashCode() + ".webm");
                
                if (!tempFile.exists()) {
                    InputStream is = reactContext.getAssets().open(assetPath);
                    FileOutputStream fos = new FileOutputStream(tempFile);
                    byte[] buffer = new byte[8192];
                    int read;
                    while ((read = is.read(buffer)) != -1) {
                        fos.write(buffer, 0, read);
                    }
                    fos.close();
                    is.close();
                }
                
                view.post(() -> view.loadFromFile(tempFile.getAbsolutePath()));
            } catch (Exception e) {
                Log.e(TAG, "Failed to load asset: " + e.getMessage());
            }
        });
    }
    
    private void downloadAndLoad(WebMAlphaTextureView view, String urlString) {
        // Use custom cacheKey if provided, otherwise fall back to URL hash
        String customCacheKey = viewCacheKeys.get(view);
        final File cacheFile;
        final String tempSuffix;  // Unique suffix for temp file
        
        if (customCacheKey != null && !customCacheKey.isEmpty()) {
            // Use custom cache path - create parent directories as needed
            cacheFile = new File(reactContext.getCacheDir(), customCacheKey);
            File parentDir = cacheFile.getParentFile();
            if (parentDir != null && !parentDir.exists()) {
                parentDir.mkdirs();
            }
            tempSuffix = String.valueOf(customCacheKey.hashCode());
            Log.d(TAG, "📁 Using custom cacheKey: " + customCacheKey);
        } else {
            // Fall back to URL hash-based cache key
            String urlHash = String.valueOf(urlString.hashCode());
            cacheFile = new File(reactContext.getCacheDir(), "webm_" + urlHash + ".webm");
            tempSuffix = urlHash;
        }
        
        downloadExecutor.execute(() -> {
            if (cacheFile.exists()) {
                Log.d(TAG, "📦 Using cached file: " + cacheFile.getAbsolutePath());
                view.post(() -> view.loadFromFile(cacheFile.getAbsolutePath()));
                return;
            }
            
            HttpURLConnection connection = null;
            try {
                URL url = new URL(urlString);
                connection = (HttpURLConnection) url.openConnection();
                connection.setConnectTimeout(15000);
                connection.setReadTimeout(30000);
                connection.setRequestProperty("User-Agent", "WebMAlphaPlayer/1.0");
                
                int responseCode = connection.getResponseCode();
                if (responseCode != HttpURLConnection.HTTP_OK) {
                    Log.e(TAG, "❌ Download failed: HTTP " + responseCode);
                    return;
                }
                
                File tempFile = new File(reactContext.getCacheDir(), "webm_tmp_" + tempSuffix + ".webm");
                InputStream is = connection.getInputStream();
                FileOutputStream fos = new FileOutputStream(tempFile);
                
                byte[] buffer = new byte[8192];
                int bytesRead;
                int totalBytes = 0;
                while ((bytesRead = is.read(buffer)) != -1) {
                    fos.write(buffer, 0, bytesRead);
                    totalBytes += bytesRead;
                }
                
                fos.close();
                is.close();
                
                if (tempFile.renameTo(cacheFile)) {
                    Log.d(TAG, "✅ Downloaded " + totalBytes + " bytes");
                    view.post(() -> view.loadFromFile(cacheFile.getAbsolutePath()));
                } else {
                    Log.e(TAG, "❌ Failed to rename temp file");
                }
                
            } catch (Exception e) {
                Log.e(TAG, "❌ Download error: " + e.getMessage());
            } finally {
                if (connection != null) {
                    connection.disconnect();
                }
            }
        });
    }
    
    @ReactProp(name = "autoPlay", defaultBoolean = true)
    public void setAutoPlay(WebMAlphaTextureView view, boolean autoPlay) {
        view.setAutoplay(autoPlay);
    }
    
    @ReactProp(name = "loop", defaultBoolean = true)
    public void setLoop(WebMAlphaTextureView view, boolean loop) {
        view.setLoop(loop);
    }
    
    @ReactProp(name = "paused", defaultBoolean = false)
    public void setPaused(WebMAlphaTextureView view, boolean paused) {
        view.setPaused(paused);
    }
    
    @ReactProp(name = "cacheKey")
    public void setCacheKey(WebMAlphaTextureView view, @Nullable String cacheKey) {
        if (cacheKey != null && !cacheKey.isEmpty()) {
            viewCacheKeys.put(view, cacheKey);
            Log.d(TAG, "📁 Set cacheKey: " + cacheKey);
        } else {
            viewCacheKeys.remove(view);
        }
    }
    
    @Nullable
    @Override
    public Map<String, Object> getExportedCustomDirectEventTypeConstants() {
        return MapBuilder.<String, Object>builder()
            .put("onLoad", MapBuilder.of("registrationName", "onLoad"))
            .put("onError", MapBuilder.of("registrationName", "onError"))
            .put("onPreloadComplete", MapBuilder.of("registrationName", "onPreloadComplete"))
            .build();
    }
}
