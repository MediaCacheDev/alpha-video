package dev.mediacache.alphavideo;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;

import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;

/**
 * Picks the MediaCodec decoders for a file's colour and alpha tracks, and counts the instances
 * players hold, so a load only takes a decoder with one free. Decoders cap their concurrent
 * instances (a Snapdragon 778G's VP9 hardware decoder allows 3), and each player needs 2.
 */
final class MediaCodecDecoders {

    enum Kind {
        HARDWARE,
        SOFTWARE,
        /** Hardware where it has an instance free, else software; chosen per track. */
        ANY,
    }

    // Instances reserved per decoder name, by players that hold them or are about to create them
    private static final Map<String, Integer> reserved = new HashMap<>();

    private MediaCodecDecoders() {}

    /**
     * Reserves a decoder of {@code kind} for the colour and the alpha track, each one listing the
     * track's profile and having an instance free.
     *
     * @param tracks pairs of codec and FFmpeg profile, colour first, from
     *     {@link BufferedDualStreamPlayer#nativeColorAlphaTracks}
     * @return the decoder names, colour first; null if a track got none, and then none is reserved.
     *     Give them back with {@link #release}.
     */
    static synchronized String[] reserve(int[] tracks, Kind kind) {
        String[] names = new String[2];
        for (int track = 0; track < names.length; track++) {
            String name = pick(mimeFor(tracks[2 * track]), tracks[2 * track + 1], kind);
            if (name == null) {
                release(names);
                return null;
            }
            reserved.merge(name, 1, Integer::sum);
            names[track] = name;
        }
        return names;
    }

    static synchronized void release(String[] names) {
        if (names == null) return;
        for (String name : names) {
            if (name != null) reserved.computeIfPresent(name, (n, count) -> count > 1 ? count - 1 : null);
        }
    }

    /**
     * The engine and decoders, as onLoad reports them: "mediacodec-hw (c2.qti.vp9.decoder)",
     * "mediacodec-sw (...)", or "mediacodec (hardware, software)" for one of each.
     */
    static String describe(String[] names) {
        int hardware = 0;
        for (String name : names) {
            if (isHardware(infoFor(name))) hardware++;
        }
        String engine = hardware == names.length ? "mediacodec-hw" : hardware == 0 ? "mediacodec-sw" : "mediacodec";
        return engine + " (" + String.join(", ", new LinkedHashSet<>(Arrays.asList(names))) + ")";
    }

    static String mimeFor(int codec) {
        return codec == BufferedDualStreamPlayer.TRACK_AV1
            ? SurfaceTextureVideoDecoder.MIME_AV1 : SurfaceTextureVideoDecoder.MIME_VP9;
    }

    private static String pick(String mime, int avProfile, Kind kind) {
        String software = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || info.getName().contains(".secure") || !decodes(info, mime)) continue;
            // An alias shares its codec's instances
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && info.isAlias()) continue;
            MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(mime);
            if (!listsProfile(caps, mime, avProfile)) continue;
            if (reserved.getOrDefault(info.getName(), 0) >= caps.getMaxSupportedInstances()) continue;
            boolean hardware = isHardware(info);
            if (hardware && kind != Kind.SOFTWARE) return info.getName();
            if (!hardware && kind != Kind.HARDWARE && software == null) software = info.getName();
        }
        return software;
    }

    private static boolean decodes(MediaCodecInfo info, String mime) {
        for (String type : info.getSupportedTypes()) {
            if (type.equalsIgnoreCase(mime)) return true;
        }
        return false;
    }

    /**
     * Android VP9 decoders rarely list the 4:4:4 profiles 1 and 3, and MediaCodec has no AV1
     * profile beyond Main. An unknown (negative) profile counts as listed.
     */
    private static boolean listsProfile(MediaCodecInfo.CodecCapabilities caps, String mime, int avProfile) {
        if (avProfile < 0) return true;
        if (SurfaceTextureVideoDecoder.MIME_AV1.equalsIgnoreCase(mime)) return avProfile == 0;
        if (avProfile > 3) return false;
        int profile = MediaCodecInfo.CodecProfileLevel.VP9Profile0 << avProfile;  // 1, 2, 4, 8
        for (MediaCodecInfo.CodecProfileLevel level : caps.profileLevels) {
            if (level.profile == profile) return true;
        }
        return false;
    }

    private static boolean isHardware(MediaCodecInfo info) {
        if (info == null) return false;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) return info.isHardwareAccelerated();
        String name = info.getName().toLowerCase();
        return !name.contains("sw") && !name.contains("google") && !name.contains("android");
    }

    private static MediaCodecInfo infoFor(String name) {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.getName().equals(name)) return info;
        }
        return null;
    }
}
