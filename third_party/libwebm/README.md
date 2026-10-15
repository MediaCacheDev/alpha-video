# libwebm's WebM parser

`webm_parser/` is `webm_parser/include` and `webm_parser/src` from [libwebm](https://chromium.googlesource.com/webm/libwebm), unmodified, at commit `6184f4484a826724b5293837134ab9492261b941` (21 April 2026). BSD licence: `LICENSE.TXT`, with the patent grant in `PATENTS.TXT`.

The C++ core's demuxer (`cpp/AlphaDemuxer.cpp`) reads WebM with it on Android and iOS. Its sources compile with the core's, with `webm_parser/` and `webm_parser/include` on the include path.

To update, copy the two directories from a newer libwebm checkout and change the commit above.
