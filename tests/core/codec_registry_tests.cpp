// The core with codec modules registered one at a time, as an app installs them. One process, in
// order, since registrations last for the process's life.
#include "VideoDecoder.h"
#include "WebMAlphaPlayer.h"

#include "../../codecs/av1/Dav1dDecoderPlugin.h"
#include "../../codecs/vpx/VpxDecoderPlugin.h"

#include <cstdio>
#include <string>

using namespace webm;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::string lastErrorCode;

// The load's error event, or "" if it loaded and decoded its first frame. The error's code in lastErrorCode.
std::string play(const std::string& file) {
    WebMAlphaPlayer p;
    std::string error;
    lastErrorCode.clear();
    p.setEventCallback([&](const std::string& event, const std::string& data) {
        if (event != "error") return;
        error = data;
        lastErrorCode = p.getLastErrorCode();
    });
    if (!p.loadFromFile(std::string(FIXTURES_DIR) + "/" + file)) return error.empty() ? "failed" : error;
    return p.getFrame(0) ? "" : "no frame";
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

int occurrences(const std::string& s, const std::string& part) {
    int n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) n++;
    return n;
}

AlphaVideoDecoderPlugin copyOfLibvpx() {
    int count = 0;
    return *alpha_video_vpx_plugins(&count)[0];
}

int refuse(const AlphaVideoStreamInfo*) {
    return 0;
}

// The registration's error, or "" if it registered
std::string registerError(const AlphaVideoDecoderPlugin* plugin) {
    std::string error;
    return registerDecoderPlugin(plugin, &error) ? "" : error;
}

std::string registerError(AlphaVideoDecoderPluginList list) {
    std::string error;
    return registerDecoderPlugins(list, &error) ? "" : error;
}

} // namespace

int main() {
    const std::string vp9 = "card_256x256/dual_vp9.webm", av1 = "card_256x256/dual_av1.webm";
    const std::string vp8 = "card_256x256/dual_vp8.webm", vp8BlockAdditional = "card_256x256/yuva_vp8.webm";

    std::string result = play(vp9);
    check(contains(result, "no VP9 decoder is installed (the vpx codec module)"), "no modules, VP9: " + result);
    check(lastErrorCode == WebMAlphaPlayer::kErrorNoDecoder, "its code: " + lastErrorCode);
    result = play("missing.webm");
    check(!result.empty() && lastErrorCode.empty(), "another error has no code: " + result + ", " + lastErrorCode);
    result = play(av1);
    check(contains(result, "no AV1 decoder is installed (the av1 codec module)"), "no modules, AV1: " + result);
    result = play(vp8);
    check(contains(result, "no VP8 decoder is installed (the vpx codec module)"), "no modules, VP8: " + result);
    check(decoderLibraryVersions().empty(), "no modules, versions: " + decoderLibraryVersions());

    AlphaVideoDecoderPlugin future = copyOfLibvpx();
    future.abi = ALPHA_VIDEO_DECODER_ABI + 1;
    result = registerError(&future);
    check(contains(result, "decoder ABI 2"), "another ABI: " + result);
    AlphaVideoDecoderPlugin older = copyOfLibvpx();
    older.structSize = sizeof(AlphaVideoDecoderPlugin) - sizeof(void*);
    result = registerError(&older);
    check(contains(result, "another alpha-video release"), "a smaller plugin struct: " + result);

    static AlphaVideoDecoderPlugin picky = copyOfLibvpx();
    picky.name = "picky";
    picky.supports = refuse;
    check(registerError(&picky).empty(), "registering a VP9 plugin that refuses every stream");
    result = play(vp9);
    check(contains(result, "picky doesn't decode VP9 profile 0, 8-bit"), "only it, VP9: " + result);

    check(registerError(alpha_video_vpx_plugins).empty(), "registering vpx");
    check(play(vp9).empty(), "vpx after the refusing plugin, VP9 plays");
    check(contains(play(av1), "(the av1 codec module)"), "vpx, AV1 still needs av1");
    check(play(vp8).empty(), "vpx, VP8 plays");
    check(play(vp8BlockAdditional).empty(), "vpx, VP8 with BlockAdditional alpha plays");
    result = play("vp9_10bit.webm");
    check(contains(result, "picky doesn't decode VP9 profile 2, 10-bit"), "10-bit VP9: " + result);
    check(registerError(alpha_video_vpx_plugins).empty(), "registering vpx again");
    check(occurrences(decoderLibraryVersions(), "libvpx v") == 1, "vpx twice, versions: " + decoderLibraryVersions());

    check(registerError(alpha_video_av1_plugins).empty(), "registering av1");
    check(play(av1).empty(), "vpx and av1, AV1 plays");
    check(play("card_256x256/dual_av1_10bit.webm").empty(), "vpx and av1, 10-bit AV1 plays");
    check(play(vp9).empty(), "vpx and av1, VP9 plays");
    check(occurrences(decoderLibraryVersions(), "dav1d ") == 1, "vpx and av1, versions: " + decoderLibraryVersions());

    std::printf("%s: %d failed\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
