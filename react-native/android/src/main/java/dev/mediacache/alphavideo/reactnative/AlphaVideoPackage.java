package dev.mediacache.alphavideo.reactnative;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.facebook.react.TurboReactPackage;
import com.facebook.react.bridge.NativeModule;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.model.ReactModuleInfo;
import com.facebook.react.module.model.ReactModuleInfoProvider;
import com.facebook.react.uimanager.ViewManager;

import java.util.Collections;
import java.util.List;

/**
 * React Native Package for WebM Alpha Player
 */
// TurboReactPackage is deprecated in favour of BaseReactPackage, which React Native before 0.74 lacks.
@SuppressWarnings("deprecation")
public class AlphaVideoPackage extends TurboReactPackage {

    @Nullable
    @Override
    public NativeModule getModule(@NonNull String name, @NonNull ReactApplicationContext reactContext) {
        return AlphaVideoModule.NAME.equals(name) ? new AlphaVideoModule(reactContext) : null;
    }

    @NonNull
    @Override
    public ReactModuleInfoProvider getReactModuleInfoProvider() {
        return () -> Collections.singletonMap(
            AlphaVideoModule.NAME,
            new ReactModuleInfo(
                AlphaVideoModule.NAME,
                AlphaVideoModule.class.getName(),
                false, // canOverrideExistingModule
                false, // needsEagerInit
                false, // isCxxModule
                BuildConfig.IS_NEW_ARCHITECTURE_ENABLED // isTurboModule
            )
        );
    }

    @NonNull
    @Override
    public List<ViewManager> createViewManagers(@NonNull ReactApplicationContext reactContext) {
        return Collections.singletonList(new AlphaVideoViewManager(reactContext));
    }
}
