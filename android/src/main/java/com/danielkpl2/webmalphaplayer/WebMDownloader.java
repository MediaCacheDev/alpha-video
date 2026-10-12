package com.danielkpl2.webmalphaplayer;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;

/**
 * Downloads remote videos into the app's cache directory. FFmpeg here is built without network
 * protocols, so http(s) sources are always played from a cached file.
 *
 * A download can be read while it runs: native code registers it (GrowingFile) under the cache
 * file's path, and the {@code ...Download} native loaders wait for bytes that haven't arrived yet.
 * Every view asking for the same file while it downloads shares one download.
 */
final class WebMDownloader {

    static {
        System.loadLibrary("webmalphaplayer");
    }

    /** Called on a download thread, or on the calling thread if the file is already cached. */
    interface Listener {
        /**
         * The file can be opened with the native {@code ...Download} loaders, which read it as it
         * arrives. Called once, before {@link #onComplete} or {@link #onError}.
         */
        void onReadable(File cacheFile);

        /** The whole file is in the cache. */
        void onComplete(File cacheFile);

        /** The download failed, before or after {@link #onReadable}. */
        void onError(String message);
    }

    private static final Map<String, Download> downloads = new HashMap<>();

    private WebMDownloader() {}

    static boolean isRemote(String uri) {
        return uri.startsWith("https://") || uri.startsWith("http://");
    }

    /**
     * Reports the cached file for {@code url} to {@code listener}, downloading it first if needed.
     *
     * @param cacheKey optional file name (may include subfolders) under the cache directory; when
     *                 empty, the name is derived from the URL
     * @param executor runs a new download; it blocks for as long as the download takes
     */
    static void fetch(Context context, String url, String cacheKey, Executor executor, Listener listener) {
        File cacheFile;
        try {
            cacheFile = cacheFileFor(context.getCacheDir(), url, cacheKey);
        } catch (IOException e) {
            listener.onError(e.getMessage());
            return;
        }

        Download download;
        boolean start = false;
        synchronized (downloads) {
            download = downloads.get(cacheFile.getPath());
            if (download == null && cacheFile.length() == 0) {
                download = new Download(url, cacheFile);
                downloads.put(cacheFile.getPath(), download);
                start = true;
            }
        }
        if (download == null) {
            // Already in the cache
            listener.onReadable(cacheFile);
            listener.onComplete(cacheFile);
            return;
        }
        download.addListener(listener);
        if (start) executor.execute(download::run);
    }

    private static final class Download {
        private final String url;
        private final File cacheFile;

        // Guarded by this
        private final List<Listener> listeners = new ArrayList<>();
        private boolean readable = false;
        private boolean ended = false;
        private String error = null;

        Download(String url, File cacheFile) {
            this.url = url;
            this.cacheFile = cacheFile;
        }

        /** Replays whatever has already happened, so a late listener sees the same sequence. */
        void addListener(Listener listener) {
            boolean wasReadable;
            boolean wasEnded;
            String endError;
            synchronized (this) {
                if (!ended) listeners.add(listener);
                wasReadable = readable;
                wasEnded = ended;
                endError = error;
            }
            if (wasReadable) listener.onReadable(cacheFile);
            if (wasEnded) {
                if (endError == null) listener.onComplete(cacheFile);
                else listener.onError(endError);
            }
        }

        void run() {
            long handle = 0;
            File partFile = null;
            HttpURLConnection connection = null;
            try {
                File dir = cacheFile.getParentFile();
                if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
                    throw new IOException("Cannot create cache directory " + dir);
                }

                connection = (HttpURLConnection) new URL(url).openConnection();
                connection.setConnectTimeout(15000);
                connection.setReadTimeout(30000);
                connection.setRequestProperty("User-Agent", "WebMAlphaPlayer/1.0");

                int status = connection.getResponseCode();
                if (status != HttpURLConnection.HTTP_OK) {
                    throw new IOException("HTTP " + status + " for " + url);
                }
                long expectedSize = connection.getContentLengthLong();

                // Readers open the cache path; while this runs, native code points them at the
                // part file. The rename at the end makes the finished file visible as complete.
                partFile = File.createTempFile("download", ".part", dir);
                try (InputStream in = connection.getInputStream();
                     OutputStream out = new FileOutputStream(partFile)) {
                    handle = nativeBegin(cacheFile.getPath(), partFile.getPath(), expectedSize);
                    if (handle == 0) throw new IOException("Cannot read " + partFile);
                    markReadable();

                    byte[] buffer = new byte[64 * 1024];
                    long written = 0;
                    int n;
                    while ((n = in.read(buffer)) != -1) {
                        out.write(buffer, 0, n);
                        written += n;
                        nativeSetWritten(handle, written);
                    }
                    if (expectedSize >= 0 && written != expectedSize) {
                        throw new IOException("Connection closed after " + written + " of " + expectedSize + " bytes");
                    }
                }
                if (!partFile.renameTo(cacheFile) && cacheFile.length() == 0) {
                    throw new IOException("Cannot write " + cacheFile);
                }
                end(handle, null);
            } catch (IOException e) {
                end(handle, e.getMessage() != null ? e.getMessage() : e.toString());
            } finally {
                if (connection != null) connection.disconnect();
                if (partFile != null) partFile.delete();
            }
        }

        private void markReadable() {
            List<Listener> toNotify;
            synchronized (this) {
                readable = true;
                toNotify = new ArrayList<>(listeners);
            }
            for (Listener listener : toNotify) listener.onReadable(cacheFile);
        }

        private void end(long handle, String endError) {
            // Unregister first, so a view arriving from now on finds the finished file (or
            // starts over after a failure) instead of joining a download that is over.
            synchronized (downloads) {
                downloads.remove(cacheFile.getPath());
            }
            if (handle != 0) nativeEnd(handle, endError == null);

            List<Listener> toNotify;
            synchronized (this) {
                ended = true;
                error = endError;
                toNotify = new ArrayList<>(listeners);
                listeners.clear();
            }
            for (Listener listener : toNotify) {
                if (endError == null) listener.onComplete(cacheFile);
                else listener.onError(endError);
            }
        }
    }

    private static File cacheFileFor(File cacheDir, String url, String cacheKey) throws IOException {
        if (cacheKey == null || cacheKey.isEmpty()) {
            return new File(cacheDir, "webm_" + sha256(url) + ".webm");
        }
        File file = new File(cacheDir, cacheKey).getCanonicalFile();
        if (!file.getPath().startsWith(cacheDir.getCanonicalPath() + File.separator)) {
            throw new IOException("cacheKey must stay inside the cache directory: " + cacheKey);
        }
        return file;
    }

    private static String sha256(String text) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(text.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                hex.append(String.format("%02x", b));
            }
            return hex.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    /** Registers the download with native readers; 0 if the part file can't be opened. */
    private static native long nativeBegin(String path, String partPath, long expectedSize);
    private static native void nativeSetWritten(long handle, long bytes);
    /** Unregisters and frees the handle; readers then see end of file, or an error if not finished. */
    private static native void nativeEnd(long handle, boolean finished);
}
