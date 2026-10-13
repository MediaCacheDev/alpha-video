package dev.mediacache.alphavideo.reactnative;

import androidx.annotation.NonNull;

import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;

/**
 * Old architecture: the same methods as the spec codegen generates for the new one
 * (NativeAlphaVideoModuleSpec), so the module compiles against either.
 */
public abstract class AlphaVideoModuleSpec extends ReactContextBaseJavaModule {
    public static final String NAME = "RNAlphaVideo";

    AlphaVideoModuleSpec(ReactApplicationContext context) {
        super(context);
    }

    @NonNull
    @Override
    public String getName() {
        return NAME;
    }

    public abstract void getDecoderInfo(Promise promise);
    public abstract void getMaxDecoderInstances(String mimeType, Promise promise);
    public abstract void getActiveDecoderCount(Promise promise);
    public abstract void getDecoderStatus(Promise promise);
}
