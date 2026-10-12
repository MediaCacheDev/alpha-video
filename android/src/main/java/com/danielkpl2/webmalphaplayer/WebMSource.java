package com.danielkpl2.webmalphaplayer;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.content.res.Resources;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.regex.Pattern;

/**
 * A local video the player can read in place: a file, an APK asset, a raw resource, or a
 * content:// URI.
 *
 * Every kind but a download opens as an {@link AssetFileDescriptor} (descriptor + byte range),
 * which native code reads directly, so nothing is copied out first. Remote URLs are not sources;
 * {@link WebMDownloader} fetches them into the cache, and the file is played as a download (read
 * by native code as it arrives) or, once complete, as a plain file.
 */
final class WebMSource {

    private enum Kind { FILE, ASSET, RAW, CONTENT, DOWNLOAD }

    private static final String ASSET_SCHEME = "asset://";
    // What Image.resolveAssetSource(require(...)) gives in a release build: a res/raw name
    private static final Pattern RAW_RESOURCE_NAME = Pattern.compile("[A-Za-z_][A-Za-z0-9_]*");

    private final Kind kind;
    private final String location;

    private WebMSource(Kind kind, String location) {
        this.kind = kind;
        this.location = location;
    }

    static WebMSource file(String path) {
        return new WebMSource(Kind.FILE, path);
    }

    /** A {@link WebMDownloader} cache file that may still be downloading. */
    static WebMSource download(String path) {
        return new WebMSource(Kind.DOWNLOAD, path);
    }

    /** Read it through the native {@code ...Download} loaders by {@link #path()}, not {@link #open}. */
    boolean isDownload() {
        return kind == Kind.DOWNLOAD;
    }

    String path() {
        return location;
    }

    /**
     * Parses {@code file:///abs/path}, {@code /abs/path}, {@code asset://relative/path},
     * {@code content://...}, or a bare res/raw resource name.
     *
     * @throws IllegalArgumentException for anything else
     */
    static WebMSource parse(String uri) {
        if (uri.startsWith(ASSET_SCHEME)) {
            String path = Uri.decode(uri.substring(ASSET_SCHEME.length()));
            while (path.startsWith("/")) path = path.substring(1);
            if (path.isEmpty()) throw new IllegalArgumentException("Empty asset path: " + uri);
            return new WebMSource(Kind.ASSET, path);
        }
        if (uri.startsWith("content://")) {
            return new WebMSource(Kind.CONTENT, uri);
        }
        if (uri.startsWith("file://")) {
            String path = Uri.parse(uri).getPath();
            if (path == null || path.isEmpty()) throw new IllegalArgumentException("Bad file URI: " + uri);
            return file(path);
        }
        if (uri.startsWith("/")) {
            return file(uri);
        }
        if (RAW_RESOURCE_NAME.matcher(uri).matches()) {
            return new WebMSource(Kind.RAW, uri);
        }
        throw new IllegalArgumentException(
            "Unsupported source '" + uri + "'. Use https://, file://, an absolute path, asset:// or content://");
    }

    /** The caller closes the descriptor; native code duplicates it if it needs to keep reading. */
    AssetFileDescriptor open(Context context) throws IOException {
        switch (kind) {
            case ASSET:
                return openAsset(context);
            case RAW:
                return openRawResource(context);
            case DOWNLOAD:
                throw new IllegalStateException("A download is read through the native download loaders: " + location);
            case CONTENT: {
                AssetFileDescriptor afd = context.getContentResolver()
                    .openAssetFileDescriptor(Uri.parse(location), "r");
                if (afd == null) throw new FileNotFoundException("Cannot open " + location);
                return afd;
            }
            default: {
                ParcelFileDescriptor pfd = ParcelFileDescriptor.open(
                    new File(location), ParcelFileDescriptor.MODE_READ_ONLY);
                return new AssetFileDescriptor(pfd, 0, AssetFileDescriptor.UNKNOWN_LENGTH);
            }
        }
    }

    private AssetFileDescriptor openAsset(Context context) throws IOException {
        try {
            return context.getAssets().openFd(location);
        } catch (FileNotFoundException e) {
            if (!assetExists(context)) throw new FileNotFoundException("Asset not found: " + location);
            // openFd only works on assets stored uncompressed in the APK.
            throw new IOException("Asset " + location + " is compressed in the APK; add its extension to "
                + "android.androidResources.noCompress in the app's build.gradle");
        }
    }

    private AssetFileDescriptor openRawResource(Context context) throws IOException {
        Resources resources = context.getResources();
        int id = resources.getIdentifier(location, "raw", context.getPackageName());
        if (id == 0) throw new FileNotFoundException("No res/raw resource named " + location);
        try {
            return resources.openRawResourceFd(id);
        } catch (Resources.NotFoundException e) {
            // openRawResourceFd only works on resources stored uncompressed in the APK.
            throw new IOException("Resource res/raw/" + location + " is compressed in the APK; add its "
                + "extension to android.androidResources.noCompress in the app's build.gradle");
        }
    }

    private boolean assetExists(Context context) {
        int slash = location.lastIndexOf('/');
        String dir = slash < 0 ? "" : location.substring(0, slash);
        String name = location.substring(slash + 1);
        try {
            String[] names = context.getAssets().list(dir);
            if (names == null) return false;
            for (String n : names) {
                if (n.equals(name)) return true;
            }
        } catch (IOException ignored) {
        }
        return false;
    }

    @Override
    public String toString() {
        switch (kind) {
            case ASSET: return ASSET_SCHEME + location;
            case RAW: return "res/raw/" + location;
            default: return location;
        }
    }
}
