package dev.mediacache.alphavideo.reactnative;

import dev.mediacache.alphavideo.AlphaVideoView;

import android.graphics.Color;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContext;
import com.facebook.react.bridge.ReadableArray;
import com.facebook.react.bridge.ReadableMap;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.common.MapBuilder;
import com.facebook.react.uimanager.ThemedReactContext;
import com.facebook.react.uimanager.UIManagerHelper;
import com.facebook.react.uimanager.ViewProps;
import com.facebook.react.uimanager.annotations.ReactProp;
import com.facebook.react.uimanager.events.Event;
import com.facebook.react.uimanager.events.EventDispatcher;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * React Native ViewManager for AlphaVideoView
 * Uses TextureView for proper z-ordering and ScrollView compatibility
 * 
 * Props match WebMAlphaView interface:
 * - source: { uri, headers?, dataSource? } or { path }, where uri is https://, file://, an
 *   absolute path, asset:// or content://, or anything the named data source reads; headers come
 *   as a flat [name, value, ...] array
 * - autoPlay: boolean (default true)
 * - loop: boolean (default true)
 * - paused: boolean (default false)
 * - playbackSpeed: double (default 1.0, changeable while playing)
 * - cacheKey: string (what the store keeps the source under, instead of its URL)
 * - storage: "auto" | "disk" | "memory" | "none"; maxStorageBytes: double (0 for 512 MB)
 * 
 * Commands: play, pause, stop, seekToFrame(frame), seekToTime(seconds). Events: onLoad, onError, onEnd, onPreloadComplete.
 * Every prop and event is declared in the codegen spec, react-native/src/AlphaVideoViewNativeComponent.ts.
 */
public class AlphaVideoViewManager extends AlphaVideoViewManagerSpec<AlphaVideoView> {
    
    public static final String REACT_CLASS = "RNAlphaVideoView";
    
    public AlphaVideoViewManager(ReactApplicationContext context) {
    }
    
    @NonNull
    @Override
    public String getName() {
        return REACT_CLASS;
    }
    
    @NonNull
    @Override
    protected AlphaVideoView createViewInstance(@NonNull ThemedReactContext context) {
        AlphaVideoView view = new AlphaVideoView(context);
        view.setCallback(new EventForwarder(view));
        return view;
    }
    
    @Override
    public void onDropViewInstance(@NonNull AlphaVideoView view) {
        view.release();
        super.onDropViewInstance(view);
    }
    
    /**
     * TextureView throws on a background drawable, so a backgroundColor style would crash the
     * app. The view stays transparent; put a background on a parent view instead.
     */
    @Override
    @ReactProp(name = ViewProps.BACKGROUND_COLOR, defaultInt = Color.TRANSPARENT, customType = "Color")
    public void setBackgroundColor(@NonNull AlphaVideoView view, int backgroundColor) {
    }
    
    @ReactProp(name = "source")
    @Override
    public void setSource(AlphaVideoView view, @Nullable ReadableMap source) {
        String sourceUri = null;
        if (source != null) {
            if (source.hasKey("uri") && !source.isNull("uri")) {
                sourceUri = source.getString("uri");
            } else if (source.hasKey("path") && !source.isNull("path")) {
                sourceUri = source.getString("path");
            }
        }
        
        if (sourceUri == null || sourceUri.isEmpty()) return;
        Map<String, String> headers = new LinkedHashMap<>();
        if (source.hasKey("headers") && !source.isNull("headers")) {
            ReadableArray flat = source.getArray("headers");
            for (int i = 0; flat != null && i + 1 < flat.size(); i += 2) {
                headers.put(flat.getString(i), flat.getString(i + 1));
            }
        }
        view.setHeaders(headers);
        view.setDataSource(source.hasKey("dataSource") && !source.isNull("dataSource")
            ? source.getString("dataSource") : null);
        view.setSource(sourceUri);
    }
    
    @ReactProp(name = "autoPlay", defaultBoolean = true)
    @Override
    public void setAutoPlay(AlphaVideoView view, boolean autoPlay) {
        view.setAutoPlay(autoPlay);
    }
    
    @ReactProp(name = "loop", defaultBoolean = true)
    @Override
    public void setLoop(AlphaVideoView view, boolean loop) {
        view.setLoop(loop);
    }
    
    @ReactProp(name = "paused", defaultBoolean = false)
    @Override
    public void setPaused(AlphaVideoView view, boolean paused) {
        view.setPaused(paused);
    }
    
    /**
     * Playback rate. iOS and web have had this; Android had the plumbing but no prop, so the
     * value never left JS.
     */
    @ReactProp(name = "playbackSpeed", defaultDouble = 1.0)
    @Override
    public void setPlaybackSpeed(AlphaVideoView view, double playbackSpeed) {
        view.setPlaybackSpeed(playbackSpeed);
    }
    
    @ReactProp(name = "cacheKey")
    @Override
    public void setCacheKey(AlphaVideoView view, @Nullable String cacheKey) {
        view.setCacheKey(cacheKey);
    }
    
    /** "auto" (default), "disk", "memory" or "none"; see AlphaVideoView.setStorage. */
    @ReactProp(name = "storage")
    @Override
    public void setStorage(AlphaVideoView view, @Nullable String storage) {
        view.setStorage(storage);
    }
    
    /** 0 for the default, 512 MB. */
    @ReactProp(name = "maxStorageBytes", defaultDouble = 0)
    @Override
    public void setMaxStorageBytes(AlphaVideoView view, double maxStorageBytes) {
        view.setMaxStorageBytes((long) maxStorageBytes);
    }
    
    /**
     * Set the playback mode.
     * - "preload": decode all frames upfront, then play
     * - "stream" (default): decode on-demand with sliding window cache
     * - "streamAndPreload": start playing immediately, cache all frames for smooth looping
     */
    @ReactProp(name = "mode")
    @Override
    public void setMode(AlphaVideoView view, @Nullable String mode) {
        view.setMode(mode != null ? mode : "stream");
    }
    
    /** "auto" (default), "mediacodec", "mediacodec-hw", "mediacodec-sw" or "software"; see AlphaVideoView.setDecoder. */
    @ReactProp(name = "decoder")
    @Override
    public void setDecoder(AlphaVideoView view, @Nullable String decoder) {
        view.setDecoder(decoder);
    }
    
    /**
     * Set the cache size when streaming (number of frames to keep in memory)
     * Default: 30 frames (~1 second at 30fps)
     */
    @ReactProp(name = "streamCacheSize", defaultInt = 30)
    @Override
    public void setStreamCacheSize(AlphaVideoView view, int size) {
        view.setStreamCacheSize(size);
    }
    
    /**
     * Set how many frames to prefetch ahead of playback position when streaming
     * Default: 15 frames (~0.5 seconds at 30fps)
     */
    @ReactProp(name = "streamPrefetchAhead", defaultInt = 15)
    @Override
    public void setStreamPrefetchAhead(AlphaVideoView view, int frames) {
        view.setStreamPrefetchAhead(frames);
    }
    
    /**
     * Enable/disable global decode queue for preload/streamAndPreload modes.
     * When true (default): decoding is serialized across all players, preventing CPU saturation
     * When false: each player decodes independently (can freeze with many)
     * Has no effect when mode='stream'.
     */
    @ReactProp(name = "useDecodeQueue", defaultBoolean = true)
    @Override
    public void setUseDecodeQueue(AlphaVideoView view, boolean use) {
        view.setUseDecodeQueue(use);
    }
    
    /**
     * Enable verbose logging for debugging: lifecycle, performance, and per-frame events.
     * When false (default): only warnings and errors.
     */
    @ReactProp(name = "verbose", defaultBoolean = false)
    @Override
    public void setVerbose(AlphaVideoView view, boolean verbose) {
        view.setVerbose(verbose);
    }
    
    @Override
    public void play(AlphaVideoView view) {
        view.play();
    }

    @Override
    public void pause(AlphaVideoView view) {
        view.pause();
    }

    @Override
    public void stop(AlphaVideoView view) {
        view.stop();
    }

    @Override
    public void seekToFrame(AlphaVideoView view, int frame) {
        view.seekToFrame(frame);
    }

    @Override
    public void seekToTime(AlphaVideoView view, double seconds) {
        view.seekToTime(seconds);
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
    private static final class EventForwarder implements AlphaVideoView.PlayerCallback {
        private final AlphaVideoView view;
        
        EventForwarder(AlphaVideoView view) {
            this.view = view;
        }
        
        @Override
        public void onLoad(int totalFrames, int width, int height, String decoder) {
            WritableMap event = Arguments.createMap();
            event.putInt("totalFrames", totalFrames);
            event.putInt("width", width);
            event.putInt("height", height);
            event.putString("decoder", decoder);
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
