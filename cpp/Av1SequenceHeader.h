#pragma once

#include <cstddef>
#include <cstdint>

// What the core reads from an AV1 sequence header (AV1 spec 5.5), so that the demuxer and the
// MediaCodec path know a track's format without an AV1 decoder linked in. The fields and their
// defaults are dav1d_parse_sequence_header's.
struct Av1SequenceHeader {
    int profile = 0;
    int bitDepth = 8;
    bool monochrome = false;
    int subsamplingX = 1;
    int subsamplingY = 1;
    int colorPrimaries = 2;           // 2: unspecified
    int transferCharacteristics = 2;
    int matrixCoefficients = 2;
    bool fullRange = false;
    int maxWidth = 0;
    int maxHeight = 0;
};

// Parses the first sequence header OBU in `data`, a run of OBUs (av1C's configOBUs, or a packet).
// False when there is none, or it is cut short or invalid.
bool parseAv1SequenceHeader(const uint8_t* data, size_t size, Av1SequenceHeader* out);
