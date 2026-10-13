package dev.mediacache.alphavideo.reactnative;

import android.view.View;

import androidx.annotation.Nullable;

import com.facebook.react.uimanager.SimpleViewManager;
import com.facebook.react.uimanager.ViewManagerDelegate;
import com.facebook.react.viewmanagers.RNAlphaVideoViewManagerDelegate;
import com.facebook.react.viewmanagers.RNAlphaVideoViewManagerInterface;

/** New architecture: props and commands arrive through the delegate generated from the codegen spec. */
public abstract class AlphaVideoViewManagerSpec<T extends View> extends SimpleViewManager<T>
        implements RNAlphaVideoViewManagerInterface<T> {

    private final ViewManagerDelegate<T> delegate = new RNAlphaVideoViewManagerDelegate<>(this);

    @Nullable
    @Override
    protected ViewManagerDelegate<T> getDelegate() {
        return delegate;
    }
}
