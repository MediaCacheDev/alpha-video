#pragma once

#include "AlphaDemuxer.h"
#include "AlphaVideoDecoderPlugin.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace webm {

/** A decoded frame, in the decoder's own buffers: valid until the decoder's next call. */
struct Picture {
    const uint8_t* planes[3] = {};  // Y, U, V; G, B, R when rgb. U and V are null when monochrome.
    int strides[3] = {};
    int width = 0;
    int height = 0;
    int chromaShiftX = 1;  // 4:2:0 is 1, 1; 4:2:2 is 1, 0; 4:4:4 is 0, 0
    int chromaShiftY = 1;
    int bitDepth = 8;  // Deeper samples are 16 bits each
    int colorRange = 1;  // ColorRange (YuvToRgb.h)
    bool rgb = false;
    bool monochrome = false;
    int64_t ptsUs = 0;
};

/**
 * Registers a codec module's decoders with every player in the process. False, with `error` set,
 * for a plugin built for another ABI; the others in the list are still registered. Registering a
 * plugin again does nothing.
 */
bool registerDecoderPlugins(AlphaVideoDecoderPluginList list, std::string* error);
bool registerDecoderPlugin(const AlphaVideoDecoderPlugin* plugin, std::string* error);

/**
 * The first registered plugin that decodes `stream`, or null with `whyNot` set: "no VP9 decoder
 * is installed (the vpx codec module)", or "libvpx doesn't decode VP9 profile 2, 10-bit".
 */
const AlphaVideoDecoderPlugin* findDecoderPlugin(const AlphaDemuxer::StreamInfo& stream, int width, int height,
                                                 std::string* whyNot);

/** The registered decoders, each "<name> <version>" once: {"libvpx v1.14.0", "dav1d 1.5.1"}. */
std::vector<std::string> registeredDecoders();

/** registeredDecoders(), comma-separated: "libvpx v1.14.0, dav1d 1.5.1"; empty with none. */
std::string decoderLibraryVersions();

/**
 * Decodes one VP9 or AV1 bitstream with a registered plugin. Not thread-safe.
 *
 * send() a packet, then receive() until it returns false. Frames come out in order, perhaps
 * some packets late (dav1d's frame threads); drain() at the end of the stream lets receive()
 * return the frames still held.
 */
class VideoDecoder {
public:
    /** Null with `error` set if no plugin decodes the stream, or the plugin's decoder can't open. */
    static std::unique_ptr<VideoDecoder> create(const AlphaDemuxer::StreamInfo& stream, int width, int height,
                                                int threads, std::string* error);

    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /** False if the packet can't be decoded. */
    bool send(const uint8_t* data, int size, int64_t ptsUs);
    bool receive(Picture* out);
    void drain();
    /** Drops the frames held and in progress, for a seek. */
    void flush();

    /** The plugin's name: "libvpx" or "dav1d" */
    const char* name() const;

private:
    VideoDecoder(const AlphaVideoDecoderPlugin* plugin, void* decoder) : plugin_(plugin), decoder_(decoder) {}

    const AlphaVideoDecoderPlugin* plugin_;
    void* decoder_;
};

} // namespace webm
