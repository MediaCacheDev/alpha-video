package dev.mediacache.alphavideo.reactnative;

import dev.mediacache.alphavideo.SurfaceTextureVideoDecoder;

import android.util.Log;

import androidx.annotation.NonNull;

import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.bridge.WritableNativeMap;

/**
 * React Native Module for WebM Alpha Player utility functions.
 * Provides access to video decoder information and diagnostics.
 */
public class AlphaVideoModule extends ReactContextBaseJavaModule {
    
    private static final String TAG = "RNAlphaVideo";
    public static final String NAME = "RNAlphaVideo";
    
    public AlphaVideoModule(ReactApplicationContext reactContext) {
        super(reactContext);
    }
    
    @NonNull
    @Override
    public String getName() {
        return NAME;
    }
    
    /**
     * Get detailed decoder information for all supported codecs (VP9, AV1).
     */
    @ReactMethod
    public void getDecoderInfo(Promise promise) {
        try {
            String info = SurfaceTextureVideoDecoder.getDecoderInfo();
            Log.i(TAG, info);
            promise.resolve(info);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /** @deprecated Use getDecoderInfo() */
    @ReactMethod
    public void getVp9DecoderInfo(Promise promise) {
        getDecoderInfo(promise);
    }
    
    /**
     * Get the maximum number of concurrent decoder instances for a given codec.
     */
    @ReactMethod
    public void getMaxDecoderInstances(String mimeType, Promise promise) {
        try {
            int max = SurfaceTextureVideoDecoder.getMaxInstances(mimeType);
            promise.resolve(max);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /** @deprecated Use getMaxDecoderInstances() */
    @ReactMethod
    public void getMaxVp9Instances(Promise promise) {
        try {
            int max = SurfaceTextureVideoDecoder.getMaxVp9Instances();
            promise.resolve(max);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /**
     * Get the current number of active decoder instances.
     */
    @ReactMethod
    public void getActiveDecoderCount(Promise promise) {
        try {
            int count = SurfaceTextureVideoDecoder.getActiveDecoderCount();
            promise.resolve(count);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /**
     * Get comprehensive decoder status as a structured object.
     */
    @ReactMethod
    public void getDecoderStatus(Promise promise) {
        try {
            int maxVp9 = SurfaceTextureVideoDecoder.getMaxInstances(SurfaceTextureVideoDecoder.MIME_VP9);
            int maxAv1 = SurfaceTextureVideoDecoder.getMaxInstances(SurfaceTextureVideoDecoder.MIME_AV1);
            int activeCount = SurfaceTextureVideoDecoder.getActiveDecoderCount();
            boolean hasVp9Hw = SurfaceTextureVideoDecoder.isVp9HardwareDecoderAvailable();
            boolean hasAv1Hw = SurfaceTextureVideoDecoder.isAv1HardwareDecoderAvailable();
            
            WritableMap result = new WritableNativeMap();
            result.putInt("maxVp9Instances", maxVp9);
            result.putInt("maxAv1Instances", maxAv1);
            result.putBoolean("vp9HardwareAvailable", hasVp9Hw);
            result.putBoolean("av1HardwareAvailable", hasAv1Hw);
            result.putInt("activeDecoders", activeCount);
            result.putInt("availableVp9Slots", Math.max(0, maxVp9 - activeCount));
            result.putInt("maxDualStreamPlayers", Math.max(maxVp9, maxAv1) / 2);
            result.putBoolean("canCreateMore", activeCount < Math.max(maxVp9, maxAv1) - 1);
            
            promise.resolve(result);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
}
