package com.danielkpl2.webmalphaplayer;

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
 * Provides access to VP9 decoder information and diagnostics.
 */
public class RNWebMAlphaPlayerModule extends ReactContextBaseJavaModule {
    
    private static final String TAG = "RNWebMAlphaPlayer";
    public static final String NAME = "RNWebMAlphaPlayer";
    
    public RNWebMAlphaPlayerModule(ReactApplicationContext reactContext) {
        super(reactContext);
    }
    
    @NonNull
    @Override
    public String getName() {
        return NAME;
    }
    
    /**
     * Get detailed VP9 decoder information.
     * Returns a string with all decoder capabilities.
     */
    @ReactMethod
    public void getVp9DecoderInfo(Promise promise) {
        try {
            String info = MediaCodecVp9Decoder.getVp9DecoderInfo();
            Log.i(TAG, info);
            promise.resolve(info);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /**
     * Get the maximum number of concurrent VP9 decoder instances.
     */
    @ReactMethod
    public void getMaxVp9Instances(Promise promise) {
        try {
            int max = MediaCodecVp9Decoder.getMaxVp9Instances();
            promise.resolve(max);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
    
    /**
     * Get the current number of active VP9 decoder instances.
     */
    @ReactMethod
    public void getActiveDecoderCount(Promise promise) {
        try {
            int count = MediaCodecVp9Decoder.getActiveDecoderCount();
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
            int maxInstances = MediaCodecVp9Decoder.getMaxVp9Instances();
            int activeCount = MediaCodecVp9Decoder.getActiveDecoderCount();
            
            WritableMap result = new WritableNativeMap();
            result.putInt("maxVp9Instances", maxInstances);
            result.putInt("activeDecoders", activeCount);
            result.putInt("availableSlots", Math.max(0, maxInstances - activeCount));
            result.putInt("maxDualStreamPlayers", maxInstances / 2);
            result.putInt("activeDualStreamPlayers", activeCount / 2);
            result.putBoolean("canCreateMore", activeCount < maxInstances - 1); // Need 2 for dual-stream
            
            promise.resolve(result);
        } catch (Exception e) {
            promise.reject("DECODER_INFO_ERROR", e.getMessage());
        }
    }
}
