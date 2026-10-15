package dev.mediacache.alphavideo;

import android.media.MediaFormat;

/**
 * MediaCodec's colour keys from a stream's colour tags. The decoder's output buffers carry them,
 * and the external-texture sampler converts YUV to RGB with them; without them a driver may use
 * BT.601 for everything.
 */
final class MediaCodecColor {
    // The core's ColorRange and ColorMatrix values (YuvToRgb.h), numbered as FFmpeg's
    private static final int AVCOL_RANGE_JPEG = 2;
    private static final int AVCOL_SPC_BT709 = 1;
    private static final int AVCOL_SPC_UNSPECIFIED = 2;
    private static final int AVCOL_SPC_BT470BG = 5;
    private static final int AVCOL_SPC_SMPTE170M = 6;
    private static final int AVCOL_SPC_BT2020_NCL = 9;
    private static final int AVCOL_SPC_BT2020_CL = 10;

    private MediaCodecColor() {}

    /** Untagged clips follow the C++ core (yuvToRgbFor): BT.601 up to 720x576, BT.709 above. */
    static int standard(int avColorSpace, int width, int height) {
        switch (avColorSpace) {
            case AVCOL_SPC_BT709: return MediaFormat.COLOR_STANDARD_BT709;
            case AVCOL_SPC_BT470BG: return MediaFormat.COLOR_STANDARD_BT601_PAL;
            case AVCOL_SPC_SMPTE170M: return MediaFormat.COLOR_STANDARD_BT601_NTSC;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL: return MediaFormat.COLOR_STANDARD_BT2020;
            case AVCOL_SPC_UNSPECIFIED:
            default:
                boolean sd = width <= 720 && height <= 576;
                return sd ? MediaFormat.COLOR_STANDARD_BT601_NTSC : MediaFormat.COLOR_STANDARD_BT709;
        }
    }

    /** Untagged clips are limited range, like the C++ core. */
    static int range(int avColorRange) {
        return avColorRange == AVCOL_RANGE_JPEG ? MediaFormat.COLOR_RANGE_FULL : MediaFormat.COLOR_RANGE_LIMITED;
    }

    static void apply(MediaFormat format, int avColorRange, int avColorSpace, int width, int height) {
        format.setInteger(MediaFormat.KEY_COLOR_STANDARD, standard(avColorSpace, width, height));
        format.setInteger(MediaFormat.KEY_COLOR_RANGE, range(avColorRange));
        format.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO);
    }
}
