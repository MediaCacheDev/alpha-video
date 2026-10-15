#pragma once

namespace webm {

/**
 * Colour ranges and matrices, numbered as FFmpeg's AVColorRange and AVColorSpace. The matrix
 * numbers are ISO/IEC 23091-2's (H.273), as in WebM's Colour element and AV1's sequence header.
 * Android's Java code and the web's TypeScript use the same numbers.
 */
enum ColorRange { kRangeUnspecified = 0, kRangeLimited = 1, kRangeFull = 2 };
enum ColorMatrix {
    kMatrixRgb = 0,
    kMatrixBt709 = 1,
    kMatrixUnspecified = 2,
    kMatrixReserved = 3,
    kMatrixBt470bg = 5,
    kMatrixSmpte170m = 6,
    kMatrixSmpte240m = 7,
    kMatrixBt2020Ncl = 9,
    kMatrixBt2020Cl = 10,
};

/**
 * YUV to RGB for the shaders: rgb = m * (y, u, v, 1), with y, u and v the texture samples
 * (0..1) and the result in 0..1 before clamping. Column-major, as GLSL's mat4 and Metal's
 * float4x4 take it.
 */
struct YuvToRgb {
    float m[16];
};

/**
 * The conversion for a clip's ColorRange and ColorMatrix. An unspecified range is limited
 * (16-235), which VP9 and AV1 video almost always is. An unspecified matrix is BT.601 up to
 * 720x576 and BT.709 above, Android's default for untagged video, so the software path matches
 * MediaCodec's.
 */
YuvToRgb yuvToRgbFor(int colorRange, int colorSpace, int width, int height);

} // namespace webm
