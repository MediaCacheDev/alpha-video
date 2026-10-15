#pragma once

namespace webm {

/**
 * YUV to RGB for the shaders: rgb = m * (y, u, v, 1), with y, u and v the texture samples
 * (0..1) and the result in 0..1 before clamping. Column-major, as GLSL's mat4 and Metal's
 * float4x4 take it.
 */
struct YuvToRgb {
    float m[16];
};

/**
 * The conversion for a clip's AVColorRange and AVColorSpace. An unspecified range is limited
 * (16-235), which VP9 and AV1 video almost always is. An unspecified matrix is BT.601 up to
 * 720x576 and BT.709 above, Android's default for untagged video, so the software path matches
 * MediaCodec's.
 */
YuvToRgb yuvToRgbFor(int colorRange, int colorSpace, int width, int height);

} // namespace webm
