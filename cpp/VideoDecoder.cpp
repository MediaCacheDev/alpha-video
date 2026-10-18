#include "VideoDecoder.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace webm {

namespace {

// Constructed on first use, so that modules can register from static initializers
std::vector<const AlphaVideoDecoderPlugin*>& registry() {
    static std::vector<const AlphaVideoDecoderPlugin*> plugins;
    return plugins;
}

std::mutex registryMutex;

const char* codecName(int codec) {
    return codec == ALPHA_VIDEO_CODEC_AV1 ? "AV1" : "VP9";
}

const char* codecModuleName(int codec) {
    return codec == ALPHA_VIDEO_CODEC_AV1 ? "av1" : "vpx";
}

AlphaVideoStreamInfo pluginStreamInfo(const AlphaDemuxer::StreamInfo& stream, int width, int height) {
    AlphaVideoStreamInfo info = {};
    info.codec = stream.codec;
    info.profile = stream.profile;
    info.bitDepth = stream.bitDepth;
    info.width = width;
    info.height = height;
    info.codecPrivate = stream.codecPrivate.empty() ? nullptr : stream.codecPrivate.data();
    info.codecPrivateSize = stream.codecPrivate.size();
    return info;
}

const AlphaVideoDecoderPlugin* findPlugin(const AlphaVideoStreamInfo& stream, std::string* whyNot) {
    std::lock_guard<std::mutex> lock(registryMutex);
    const AlphaVideoDecoderPlugin* refusing = nullptr;
    for (const AlphaVideoDecoderPlugin* plugin : registry()) {
        if (plugin->codec != stream.codec) continue;
        if (plugin->supports(&stream)) return plugin;
        if (!refusing) refusing = plugin;
    }
    if (refusing) {
        *whyNot = std::string(refusing->name) + " doesn't decode " + codecName(stream.codec) + " profile " +
                  std::to_string(stream.profile) + ", " + std::to_string(stream.bitDepth) + "-bit";
    } else {
        *whyNot = std::string("no ") + codecName(stream.codec) + " decoder is installed (the " +
                  codecModuleName(stream.codec) + " codec module)";
    }
    return nullptr;
}

} // namespace

bool registerDecoderPlugin(const AlphaVideoDecoderPlugin* plugin, std::string* error) {
    if (plugin->abi != ALPHA_VIDEO_DECODER_ABI || plugin->structSize < sizeof(AlphaVideoDecoderPlugin)) {
        *error = "A codec module from another alpha-video release (decoder ABI " + std::to_string(plugin->abi) +
                 ", plugin size " + std::to_string(plugin->structSize) + "; this core's ABI " +
                 std::to_string(ALPHA_VIDEO_DECODER_ABI) + ", size " + std::to_string(sizeof(AlphaVideoDecoderPlugin)) +
                 "): use the codec modules of this release";
        return false;
    }
    std::lock_guard<std::mutex> lock(registryMutex);
    std::vector<const AlphaVideoDecoderPlugin*>& plugins = registry();
    if (std::find(plugins.begin(), plugins.end(), plugin) == plugins.end()) plugins.push_back(plugin);
    return true;
}

bool registerDecoderPlugins(AlphaVideoDecoderPluginList list, std::string* error) {
    int count = 0;
    const AlphaVideoDecoderPlugin* const* plugins = list(&count);
    bool registered = true;
    for (int i = 0; i < count; i++) {
        if (!registerDecoderPlugin(plugins[i], error)) registered = false;
    }
    return registered;
}

const AlphaVideoDecoderPlugin* findDecoderPlugin(const AlphaDemuxer::StreamInfo& stream, int width, int height,
                                                 std::string* whyNot) {
    return findPlugin(pluginStreamInfo(stream, width, height), whyNot);
}

std::vector<std::string> registeredDecoders() {
    std::lock_guard<std::mutex> lock(registryMutex);
    std::vector<std::string> versions;
    for (const AlphaVideoDecoderPlugin* plugin : registry()) {
        std::string version = std::string(plugin->name) + " " + plugin->version;
        if (std::find(versions.begin(), versions.end(), version) == versions.end()) versions.push_back(version);
    }
    return versions;
}

std::string decoderLibraryVersions() {
    std::string joined;
    for (const std::string& version : registeredDecoders()) joined += (joined.empty() ? "" : ", ") + version;
    return joined;
}

std::unique_ptr<VideoDecoder> VideoDecoder::create(const AlphaDemuxer::StreamInfo& stream, int width, int height,
                                                   int threads, std::string* error) {
    const AlphaVideoStreamInfo info = pluginStreamInfo(stream, width, height);
    const AlphaVideoDecoderPlugin* plugin = findPlugin(info, error);
    if (!plugin) return nullptr;
    char message[256] = "";
    void* decoder = plugin->open(&info, threads, message, sizeof(message));
    if (!decoder) {
        *error = message[0] ? message : std::string(plugin->name) + ": failed to open";
        return nullptr;
    }
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(plugin, decoder));
}

VideoDecoder::~VideoDecoder() {
    plugin_->close(decoder_);
}

bool VideoDecoder::send(const uint8_t* data, int size, int64_t ptsUs) {
    return plugin_->send(decoder_, data, size, ptsUs) != 0;
}

bool VideoDecoder::receive(Picture* out) {
    AlphaVideoPicture picture = {};
    if (!plugin_->receive(decoder_, &picture)) return false;
    for (int i = 0; i < 3; i++) {
        out->planes[i] = picture.planes[i];
        out->strides[i] = picture.strides[i];
    }
    out->width = picture.width;
    out->height = picture.height;
    out->chromaShiftX = picture.chromaShiftX;
    out->chromaShiftY = picture.chromaShiftY;
    out->bitDepth = picture.bitDepth;
    out->colorRange = picture.colorRange;
    out->rgb = picture.rgb != 0;
    out->monochrome = picture.monochrome != 0;
    out->ptsUs = picture.ptsUs;
    return true;
}

void VideoDecoder::drain() {
    plugin_->drain(decoder_);
}

void VideoDecoder::flush() {
    plugin_->flush(decoder_);
}

const char* VideoDecoder::name() const {
    return plugin_->name;
}

} // namespace webm
