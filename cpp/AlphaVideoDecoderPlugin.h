/*
 * The interface between the player's core and a software decoder: libvpx in the vpx codec module,
 * dav1d in the av1 module, or a third party's. A module provides a list of plugins, one per codec,
 * and the app (or the platform's discovery) registers them with the core.
 *
 * C, not C++: on Android each module is its own .so with its own static libc++, and on iOS a
 * module may be built apart from the core. A plugin built for another ABI is refused.
 */
#ifndef ALPHA_VIDEO_DECODER_PLUGIN_H
#define ALPHA_VIDEO_DECODER_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALPHA_VIDEO_DECODER_ABI 1

/* AlphaDemuxer::Codec */
#define ALPHA_VIDEO_CODEC_VP8 0
#define ALPHA_VIDEO_CODEC_VP9 1
#define ALPHA_VIDEO_CODEC_AV1 2

/* A bitstream to decode, as the demuxer read it. Colour numbers are H.273's. */
typedef struct AlphaVideoStreamInfo {
    int codec;
    int profile;
    int bitDepth;
    int width;
    int height;
    const uint8_t* codecPrivate; /* Matroska CodecPrivate (av1C for AV1); may be null */
    size_t codecPrivateSize;
} AlphaVideoStreamInfo;

/* A decoded frame, in the decoder's own buffers: valid until its next call. */
typedef struct AlphaVideoPicture {
    const uint8_t* planes[3]; /* Y, U, V; G, B, R when rgb. U and V null when monochrome. */
    int strides[3];
    int width;
    int height;
    int chromaShiftX; /* 4:2:0 is 1, 1; 4:2:2 is 1, 0; 4:4:4 is 0, 0 */
    int chromaShiftY;
    int bitDepth;     /* Deeper samples are 16 bits each */
    int colorRange;   /* 1 limited, 2 full */
    int rgb;
    int monochrome;
    int64_t ptsUs;
} AlphaVideoPicture;

/*
 * One codec's decoder. Each decoder `open` returns is used from one thread at a time. send() a
 * packet, then receive() until it returns 0. Frames come out in order, perhaps some packets late;
 * drain() at the end of the stream lets receive() return the frames still held.
 */
typedef struct AlphaVideoDecoderPlugin {
    uint32_t abi;        /* ALPHA_VIDEO_DECODER_ABI */
    uint32_t structSize; /* sizeof(AlphaVideoDecoderPlugin) */
    const char* name;    /* "libvpx" */
    const char* version; /* "v1.14.0" */
    int codec;           /* ALPHA_VIDEO_CODEC_* */
    /* 1 if it decodes this stream (libvpx here: 8-bit only) */
    int (*supports)(const AlphaVideoStreamInfo* stream);
    /* Null, with a message in `error`, if it can't open. threads: 0 for the decoder's choice. */
    void* (*open)(const AlphaVideoStreamInfo* stream, int threads, char* error, size_t errorSize);
    /* 0 if the packet can't be decoded */
    int (*send)(void* decoder, const uint8_t* data, int size, int64_t ptsUs);
    /* 1 with a frame in `out`, 0 when none is ready */
    int (*receive)(void* decoder, AlphaVideoPicture* out);
    void (*drain)(void* decoder);
    /* Drops the frames held and in progress, for a seek */
    void (*flush)(void* decoder);
    void (*close)(void* decoder);
} AlphaVideoDecoderPlugin;

/* A codec module's entry point: its plugins, `*count` of them, valid for the process's life. */
typedef const AlphaVideoDecoderPlugin* const* (*AlphaVideoDecoderPluginList)(int* count);

#ifdef __cplusplus
}
#endif

#endif
