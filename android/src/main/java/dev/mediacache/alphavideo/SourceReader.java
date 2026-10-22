package dev.mediacache.alphavideo;

import android.net.Uri;

import java.io.IOException;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * A native reader over an {@link AlphaVideoDataSource}, fetching on its own thread. With the
 * range-source add-on ({@link RangeSourceLink}), it reads by range, through blocks in memory and an
 * optional store. Without it, the basic input: the file is downloaded whole into a temporary file,
 * and read as it arrives. The probe and the player of one source share it.
 *
 * Native calls take their own reference with {@link #retain} and drop it with {@link #release},
 * so that {@link #close} on the main thread never frees a reader under a read.
 */
final class SourceReader {

    static {
        System.loadLibrary("alphavideo");
    }

    /** Where a reader keeps what it fetches between plays. */
    enum Storage { DISK, MEMORY, NONE }

    // Guarded by this; 0 once closed
    private long handle;

    private SourceReader(long handle) {
        this.handle = handle;
    }

    /**
     * Opens the source at 0 to learn its size and version, unless the store holds it whole. Blocks
     * on the network.
     *
     * @param storeDir the folder of the disk store, or the name of the memory one; with the add-on
     * @param key what the store calls this file; with the add-on
     * @param downloadDir where the basic input downloads to, without the add-on
     */
    static SourceReader open(AlphaVideoDataSource source, Uri uri, Map<String, String> headers,
                             Storage storage, String storeDir, String key, long maxStorageBytes,
                             String downloadDir) throws IOException {
        String[] error = new String[1];
        long handle = nativeOpen(RangeSourceLink.plugin(), new Upstream(source, uri, headers), storage.ordinal(),
            storeDir, key, maxStorageBytes, downloadDir, error);
        if (handle == 0) throw new IOException(error[0] != null ? error[0] : "Cannot open " + uri);
        return new SourceReader(handle);
    }

    /** A new reference to the native reader, for one native call; 0 once closed. */
    synchronized long retain() {
        return handle != 0 ? nativeRetain(handle) : 0;
    }

    static void release(long reference) {
        if (reference != 0) nativeRelease(reference);
    }

    /** Reads the whole file into the store. Blocks; throws if it fails or the reader closes. */
    void fetchAll() throws IOException {
        long reference = retain();
        if (reference == 0) throw new IOException("The reader is closed");
        try {
            String error = nativeFetchAll(reference);
            if (error != null) throw new IOException(error);
        } finally {
            release(reference);
        }
    }

    /**
     * Drops this reference. A player still reading the file keeps the reader open until it
     * unloads; then it closes and its fetch stops.
     */
    void close() {
        long h;
        synchronized (this) {
            h = handle;
            handle = 0;
        }
        release(h);
    }

    /**
     * The data source as the native reader calls it, on the reader's fetch thread, with failures
     * as return values.
     */
    static final class Upstream {
        // cancel() comes from whichever thread closes the reader, the main thread too, and an
        // HTTPS disconnect can write to the socket.
        private static final ExecutorService cancelExecutor = Executors.newCachedThreadPool(r -> {
            Thread t = new Thread(r, "AlphaVideoCancel");
            t.setDaemon(true);
            return t;
        });

        private final AlphaVideoDataSource source;
        private final Uri uri;
        private final Map<String, String> headers;
        // Read by native code after open() and failures
        String etag;
        String lastModified;
        String error;

        Upstream(AlphaVideoDataSource source, Uri uri, Map<String, String> headers) {
            this.source = source;
            this.uri = uri;
            this.headers = headers;
        }

        /** The file's size, -1 if not known, or -2 with {@link #error} set. */
        long open(long position) {
            try {
                AlphaVideoDataSource.Info info = source.open(uri, position, headers);
                etag = info.etag;
                lastModified = info.lastModified;
                return info.size;
            } catch (Exception e) {
                error = describe(e);
                return -2;
            }
        }

        /** How many bytes were read into {@code buffer}, 0 at the end, or -1 with {@link #error} set. */
        int read(byte[] buffer, int length) {
            try {
                int n;
                do {
                    n = source.read(buffer, 0, length);
                } while (n == 0);
                return n < 0 ? 0 : n;
            } catch (Exception e) {
                error = describe(e);
                return -1;
            }
        }

        void close() {
            try {
                source.close();
            } catch (Exception ignored) {
            }
        }

        void cancel() {
            cancelExecutor.execute(() -> {
                try {
                    source.cancel();
                } catch (Exception ignored) {
                }
            });
        }

        private static String describe(Exception e) {
            return e.getMessage() != null ? e.getMessage() : e.toString();
        }
    }

    /**
     * @param plugin the add-on's table, or 0 for the basic input
     * @param storage a {@link Storage} ordinal
     * @param error set to why, when it returns 0
     * @return a reference to the reader, or 0
     */
    private static native long nativeOpen(long plugin, Upstream upstream, int storage, String storeDir, String key,
                                          long maxStorageBytes, String downloadDir, String[] error);
    private static native long nativeRetain(long reference);
    private static native void nativeRelease(long reference);
    /** Null once the whole file is held, else why not. */
    private static native String nativeFetchAll(long reference);
}
