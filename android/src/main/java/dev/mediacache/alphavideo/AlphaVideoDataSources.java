package dev.mediacache.alphavideo;

import java.util.HashMap;
import java.util.Map;

/**
 * Data sources by name, for React Native: register a factory here, then pass
 * {@code source={{ uri, dataSource: 'name' }}} to {@code AlphaVideoView}. Thread-safe.
 */
public final class AlphaVideoDataSources {

    private static final Map<String, AlphaVideoDataSource.Factory> factories = new HashMap<>();

    private AlphaVideoDataSources() {}

    /** Registers {@code factory} under {@code name}, replacing any registered before. */
    public static synchronized void register(String name, AlphaVideoDataSource.Factory factory) {
        factories.put(name, factory);
    }

    public static synchronized void unregister(String name) {
        factories.remove(name);
    }

    /** The factory registered under {@code name}, or null. */
    public static synchronized AlphaVideoDataSource.Factory get(String name) {
        return factories.get(name);
    }
}
