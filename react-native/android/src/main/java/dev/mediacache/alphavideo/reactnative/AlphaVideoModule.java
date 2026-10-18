package dev.mediacache.alphavideo.reactnative;

import dev.mediacache.alphavideo.AlphaVideoCodecs;
import dev.mediacache.alphavideo.SurfaceTextureVideoDecoder;

import android.util.Log;

import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactMethod;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.bridge.WritableNativeArray;
import com.facebook.react.bridge.WritableNativeMap;

/**
 * React Native Module for WebM Alpha Player utility functions.
 * Provides access to video decoder information and diagnostics.
 */
public class AlphaVideoModule extends AlphaVideoModuleSpec {
    
    private static final String TAG = "RNAlphaVideo";
    
    public AlphaVideoModule(ReactApplicationContext reactContext) {
        super(reactContext);
    }
    
    /**
     * Get detailed decoder information for all supported codecs (VP9, AV1).
     */
    @Override
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
    
    /**
     * Get the maximum number of concurrent decoder instances for a given codec.
     */
    @Override
    @ReactMethod
    public void getMaxDecoderInstances(String mimeType, Promise promise) {
        try {
            int max = SurfaceTextureVideoDecoder.getMaxInstances(mimeType);
            promise.resolve(max);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /**
     * Get the current number of active decoder instances.
     */
    @Override
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
    @Override
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
            WritableArray softwareDecoders = new WritableNativeArray();
            for (String decoder : AlphaVideoCodecs.softwareDecoders()) softwareDecoders.pushString(decoder);
            result.putArray("softwareDecoders", softwareDecoders);
            
            promise.resolve(result);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
}
