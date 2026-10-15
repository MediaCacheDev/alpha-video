#include "YuvToRgb.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace webm {

YuvToRgb yuvToRgbFor(int colorRange, int colorSpace, int width, int height) {
    // Luma and chroma weights of the matrix
    double kr = 0.299, kb = 0.114;  // BT.601
    bool sd = width <= 720 && height <= 576;
    if (colorSpace == AVCOL_SPC_BT709 || (colorSpace == AVCOL_SPC_UNSPECIFIED && !sd)) {
        kr = 0.2126; kb = 0.0722;
    } else if (colorSpace == AVCOL_SPC_BT2020_NCL || colorSpace == AVCOL_SPC_BT2020_CL) {
        kr = 0.2627; kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;
    const double rv = 2 * (1 - kr);
    const double bu = 2 * (1 - kb);
    const double gu = 2 * kb * (1 - kb) / kg;
    const double gv = 2 * kr * (1 - kr) / kg;

    // Y' = ys * (y - yo), U' = cs * (u - co), from the 0..1 texture samples
    const bool full = colorRange == AVCOL_RANGE_JPEG;
    const double ys = full ? 1.0 : 255.0 / 219.0, yo = full ? 0.0 : 16.0 / 255.0;
    const double cs = full ? 1.0 : 255.0 / 224.0, co = 128.0 / 255.0;

    // Rows R, G, B of [y u v 1], stored column-major
    const double rows[3][4] = {
        {ys, 0, rv * cs, -ys * yo - rv * cs * co},
        {ys, -gu * cs, -gv * cs, -ys * yo + (gu + gv) * cs * co},
        {ys, bu * cs, 0, -ys * yo - bu * cs * co},
    };
    YuvToRgb out{};
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 3; row++) out.m[col * 4 + row] = static_cast<float>(rows[row][col]);
    }
    out.m[15] = 1;
    return out;
}

} // namespace webm
