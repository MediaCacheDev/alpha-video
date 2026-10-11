package com.danielkpl2.webmalphaplayer;

import android.util.Log;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContext;
import com.facebook.react.bridge.ReadableArray;
import com.facebook.react.bridge.ReadableMap;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.common.MapBuilder;
import com.facebook.react.uimanager.SimpleViewManager;
import com.facebook.react.uimanager.ThemedReactContext;
import com.facebook.react.uimanager.UIManagerHelper;
import com.facebook.react.uimanager.annotations.ReactProp;
import com.facebook.react.uimanager.events.Event;
import com.facebook.react.uimanager.events.EventDispatcher;

import java.util.Map;

/**
 * React Native ViewManager for WebMAlphaTextureView
 * Uses TextureView for proper z-ordering and ScrollView compatibility
 * 
 * Props match WebMAlphaView interface:
 * - source: { uri: string } or { path: string }, where uri is https://, file://, an absolute
 *   path, asset:// or content://
 * - autoPlay: boolean (default true)
 * - loop: boolean (default true)
 * - paused: boolean (default false)
 * - playbackSpeed: double (default 1.0, changeable while playing)
 * - cacheKey: string (optional cache file name for https sources)
 * 
 * Commands: play, pause, stop, seekToFrame(frame). Events: onLoad, onError, onEnd, onPreloadComplete.
 */
public class RNWebMAlphaViewManager extends SimpleViewManager<WebMAlphaTextureView> {
    
    private static final String TAG = "RNWebMAlphaViewManager";
    public static final String REACT_CLASS = "RNWebMAlphaView";
    
    public RNWebMAlphaViewManager(ReactApplicationContext context) {
    }
    
    @NonNull
    @Override
    public String getName() {
        return REACT_CLASS;
    }
    
    @NonNull
    @Override
    protected WebMAlphaTextureView createViewInstance(@NonNull ThemedReactContext context) {
        WebMAlphaTextureView view = new WebMAlphaTextureView(context);
        view.setCallback(new EventForwarder(view));
        return view;
    }
    
    @Override
    public void onDropViewInstance(@NonNull WebMAlphaTextureView view) {
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
        view.setSource(sourceUri);
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
    
    /**
     * Playback rate. iOS and web have had this; Android had the plumbing but no prop, so the
     * value never left JS.
     */
    @ReactProp(name = "playbackSpeed", defaultDouble = 1.0)
    public void setPlaybackSpeed(WebMAlphaTextureView view, double playbackSpeed) {
        view.setPlaybackSpeed(playbackSpeed);
    }
    
    @ReactProp(name = "cacheKey")
    public void setCacheKey(WebMAlphaTextureView view, @Nullable String cacheKey) {
        view.setCacheKey(cacheKey);
    }
    
    /**
     * Set the playback mode.
     * - "preload" (default): decode all frames upfront, then play
     * - "stream": decode on-demand with sliding window cache
     * - "streamAndPreload": start playing immediately, cache all frames for smooth looping
     */
    @ReactProp(name = "mode")
    public void setMode(WebMAlphaTextureView view, @Nullable String mode) {
        view.setMode(mode != null ? mode : "preload");
    }
    
    /**
     * @deprecated Use mode prop instead
     */
    @ReactProp(name = "stream", defaultBoolean = false)
    public void setStream(WebMAlphaTextureView view, boolean stream) {
        view.setStream(stream);
    }
    
    /**
     * Set the cache size when streaming (number of frames to keep in memory)
     * Default: 30 frames (~1 second at 30fps)
     */
    @ReactProp(name = "streamCacheSize", defaultInt = 30)
    public void setStreamCacheSize(WebMAlphaTextureView view, int size) {
        view.setStreamCacheSize(size);
    }
    
    /**
     * Set how many frames to prefetch ahead of playback position when streaming
     * Default: 15 frames (~0.5 seconds at 30fps)
     */
    @ReactProp(name = "streamPrefetchAhead", defaultInt = 15)
    public void setStreamPrefetchAhead(WebMAlphaTextureView view, int frames) {
        view.setStreamPrefetchAhead(frames);
    }
    
    /**
     * Enable/disable global decode queue for preload/streamAndPreload modes.
     * When true (default): decoding is serialized across all players, preventing CPU saturation
     * When false: each player decodes independently (can freeze with many)
     * Has no effect when mode='stream'.
     */
    @ReactProp(name = "useDecodeQueue", defaultBoolean = true)
    public void setUseDecodeQueue(WebMAlphaTextureView view, boolean use) {
        view.setUseDecodeQueue(use);
    }
    
    /**
     * Enable verbose logging for debugging: lifecycle, performance, and per-frame events.
     * When false (default): only warnings and errors.
     */
    @ReactProp(name = "verbose", defaultBoolean = false)
    public void setVerbose(WebMAlphaTextureView view, boolean verbose) {
        view.setVerbose(verbose);
    }
    
    @Override
    public void receiveCommand(@NonNull WebMAlphaTextureView view, String commandId, @Nullable ReadableArray args) {
        switch (commandId) {
            case "play":
                view.play();
                break;
            case "pause":
                view.pause();
                break;
            case "stop":
                view.stop();
                break;
            case "seekToFrame":
                if (args != null && args.size() > 0) {
                    view.seekToFrame(args.getInt(0));
                }
                break;
            default:
                Log.w(TAG, "Unknown command: " + commandId);
        }
    }
    
    @Nullable
    @Override
    public Map<String, Object> getExportedCustomDirectEventTypeConstants() {
        return MapBuilder.<String, Object>builder()
            .put("onLoad", MapBuilder.of("registrationName", "onLoad"))
            .put("onError", MapBuilder.of("registrationName", "onError"))
            .put("onEnd", MapBuilder.of("registrationName", "onEnd"))
            .put("onPreloadComplete", MapBuilder.of("registrationName", "onPreloadComplete"))
            .build();
    }
    
    /** Turns player callbacks into React events on the view. */
    private static final class EventForwarder implements WebMAlphaTextureView.PlayerCallback {
        private final WebMAlphaTextureView view;
        
        EventForwarder(WebMAlphaTextureView view) {
            this.view = view;
        }
        
        @Override
        public void onLoad(int totalFrames, int width, int height) {
            WritableMap event = Arguments.createMap();
            event.putInt("totalFrames", totalFrames);
            event.putInt("width", width);
            event.putInt("height", height);
            send("onLoad", event);
        }
        
        @Override
        public void onError(String error) {
            WritableMap event = Arguments.createMap();
            event.putString("error", error);
            send("onError", event);
        }
        
        @Override
        public void onEnd() {
            send("onEnd", Arguments.createMap());
        }
        
        @Override
        public void onPreloadComplete(int frames, double timeMs) {
            WritableMap event = Arguments.createMap();
            event.putInt("totalFrames", frames);
            event.putDouble("timeMs", timeMs);
            send("onPreloadComplete", event);
        }
        
        private void send(String eventName, WritableMap params) {
            ReactContext context = (ReactContext) view.getContext();
            EventDispatcher dispatcher = UIManagerHelper.getEventDispatcherForReactTag(context, view.getId());
            if (dispatcher == null) return;
            dispatcher.dispatchEvent(
                new PlayerEvent(UIManagerHelper.getSurfaceId(view), view.getId(), eventName, params));
        }
    }
    
    private static final class PlayerEvent extends Event<PlayerEvent> {
        private final String eventName;
        private final WritableMap eventData;
        
        PlayerEvent(int surfaceId, int viewTag, String eventName, WritableMap eventData) {
            super(surfaceId, viewTag);
            this.eventName = eventName;
            this.eventData = eventData;
        }
        
        @NonNull
        @Override
        public String getEventName() {
            return eventName;
        }
        
        @Override
        public boolean canCoalesce() {
            return false;
        }
        
        @Nullable
        @Override
        protected WritableMap getEventData() {
            return eventData;
        }
    }
}
