#!/bin/bash
# Makes the AV1 files whose sequence headers core_tests reads with the core's own parser and with
# dav1d's ("AV1 sequence headers match dav1d's"). Each is two 64x64 frames, chosen to reach every
# branch of the header: profiles 0-2, 8/10/12-bit, 4:2:0/4:2:2/4:4:4, monochrome, sRGB (identity
# matrix), colour tags, timing and decoder model info, a reduced still-picture header, and three
# encoders. Needs ffmpeg with libaom, SVT-AV1 and rav1e (brew install ffmpeg).
set -euo pipefail
cd "$(dirname "$0")"

source=(-f lavfi -i testsrc2=size=64x64:rate=30 -frames:v 2)
aom=(-c:v libaom-av1 -cpu-used 8 -crf 40)

make() {
    local name=$1
    shift
    ffmpeg -v error -y "${source[@]}" "$@" "$name.webm"
}

make p0_bt709_full   "${aom[@]}" -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range pc
make p0_monochrome   "${aom[@]}" -pix_fmt gray
make p1_444          "${aom[@]}" -pix_fmt yuv444p
make p1_srgb         "${aom[@]}" -pix_fmt gbrp -colorspace rgb -color_primaries bt709 -color_trc iec61966-2-1
make p2_10bit_422    "${aom[@]}" -pix_fmt yuv422p10le
make p2_12bit_420    "${aom[@]}" -pix_fmt yuv420p12le
make p2_12bit_422    "${aom[@]}" -pix_fmt yuv422p12le
make p2_12bit_444    "${aom[@]}" -pix_fmt yuv444p12le
make timing_model    "${aom[@]}" -aom-params timing-info=model
make still_picture   -c:v libaom-av1 -cpu-used 8 -still-picture 1 -frames:v 1
make svtav1_10bit    -c:v libsvtav1 -preset 10 -pix_fmt yuv420p10le -svtav1-params enable-hdr=1
make rav1e_422_10bit -c:v librav1e -pix_fmt yuv422p10le
echo "Made $(ls ./*.webm | wc -l | tr -d ' ') files in $(pwd)"
