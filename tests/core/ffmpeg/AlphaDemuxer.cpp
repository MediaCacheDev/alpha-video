#include "AlphaDemuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>
}

#include <cmath>

namespace webm {

namespace {

bool isSupportedVideoCodec(AVCodecID id) {
    return id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_AV1 || id == AV_CODEC_ID_VP8;
}

AlphaDemuxer::Codec codecOf(AVCodecID id) {
    return id == AV_CODEC_ID_AV1 ? AlphaDemuxer::AV1 : id == AV_CODEC_ID_VP8 ? AlphaDemuxer::VP8 : AlphaDemuxer::VP9;
}

// Opens io as a demuxer input, or returns null. Never frees io.
AVFormatContext* openInput(AVIOContext* io) {
    AVFormatContext* formatCtx = avformat_alloc_context();
    if (!formatCtx) return nullptr;
    formatCtx->pb = io;
    formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&formatCtx, nullptr, nullptr, nullptr) < 0) return nullptr;
    return formatCtx;
}

// The alpha payload of a packet's BlockAdditional (ID 1, after FFmpeg's 8-byte BlockAddID), or null.
const uint8_t* blockAdditionalAlpha(const AVPacket* packet, size_t* size) {
    size_t sideSize = 0;
    const uint8_t* side = av_packet_get_side_data(packet, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &sideSize);
    if (!side || sideSize <= 8 || AV_RB64(side) != 1) return nullptr;
    *size = sideSize - 8;
    return side + 8;
}

// Whether the first packet of a VP8 or VP9 stream carries alpha in its BlockAdditional. Leaves the read
// position past that packet.
bool hasBlockAdditionalAlpha(AVFormatContext* formatCtx, int streamIndex) {
    const AVCodecID id = formatCtx->streams[streamIndex]->codecpar->codec_id;
    if (id != AV_CODEC_ID_VP9 && id != AV_CODEC_ID_VP8) return false;
    AVPacket* packet = av_packet_alloc();
    bool found = false;
    while (packet && av_read_frame(formatCtx, packet) >= 0) {
        bool ours = packet->stream_index == streamIndex;
        size_t size = 0;
        found = ours && blockAdditionalAlpha(packet, &size) != nullptr;
        av_packet_unref(packet);
        if (ours) break;
    }
    av_packet_free(&packet);
    return found;
}

// From the decoded pixel format when probing found one; otherwise from AV1's av1C or VP9's profile.
int bitDepthOf(const AVCodecParameters* par) {
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format))) {
        return desc->comp[0].depth;
    }
    if (par->codec_id == AV_CODEC_ID_AV1 && par->extradata_size >= 3) {
        const uint8_t flags = par->extradata[2];
        return (flags & 0x40) ? ((flags & 0x20) ? 12 : 10) : 8;
    }
    return par->profile >= 2 ? 10 : 8;
}

AlphaDemuxer::StreamInfo streamInfoOf(const AVCodecParameters* par) {
    AlphaDemuxer::StreamInfo s;
    s.codec = codecOf(par->codec_id);
    s.profile = par->profile;
    s.bitDepth = bitDepthOf(par);
    s.colorRange = par->color_range;
    s.colorSpace = par->color_space;
    s.colorPrimaries = par->color_primaries;
    s.colorTrc = par->color_trc;
    if (par->extradata && par->extradata_size > 0) {
        s.codecPrivate.assign(par->extradata, par->extradata + par->extradata_size);
    }
    return s;
}

} // namespace

bool isVp9Keyframe(const uint8_t* data, int size) {
    if (!data || size < 1) return false;
    const uint8_t b = data[0];
    if ((b >> 6) != 2) return false;  // frame_marker
    const int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
    // profile 3 has a reserved bit before show_existing_frame
    const int showExistingBit = profile == 3 ? 2 : 3;
    if ((b >> showExistingBit) & 1) return false;
    return ((b >> (showExistingBit - 1)) & 1) == 0;  // frame_type 0 is a keyframe
}

bool isVp8Keyframe(const uint8_t* data, int size) {
    return data && size >= 3 && (data[0] & 1) == 0;  // frame type 0 is a keyframe
}

std::unique_ptr<AlphaDemuxer> AlphaDemuxer::open(AVIOContext* io) {
    std::unique_ptr<AlphaDemuxer> demuxer(new AlphaDemuxer());
    demuxer->formatCtx_ = openInput(io);
    if (!demuxer->formatCtx_) return nullptr;
    demuxer->packet_ = av_packet_alloc();
    if (!demuxer->packet_ || !demuxer->readLayout()) return nullptr;
    return demuxer;
}

std::vector<int> AlphaDemuxer::colorAlphaTracks(AVIOContext* io) {
    std::vector<int> tracks;
    AVFormatContext* formatCtx = openInput(io);
    if (!formatCtx) return tracks;
    int firstTrack = -1;
    if (avformat_find_stream_info(formatCtx, nullptr) >= 0) {
        for (unsigned i = 0; i < formatCtx->nb_streams; i++) {
            AVCodecParameters* par = formatCtx->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && isSupportedVideoCodec(par->codec_id)) {
                if (firstTrack < 0) firstTrack = static_cast<int>(i);
                tracks.push_back(codecOf(par->codec_id));
                tracks.push_back(par->profile);
            }
        }
        if (tracks.size() == 2 && hasBlockAdditionalAlpha(formatCtx, firstTrack)) {
            tracks.push_back(tracks[0]);
            tracks.push_back(tracks[1]);
        }
    }
    avformat_close_input(&formatCtx);
    return tracks;
}

AlphaDemuxer::~AlphaDemuxer() {
    av_packet_free(&packet_);
    if (formatCtx_) avformat_close_input(&formatCtx_);
}

bool AlphaDemuxer::readLayout() {
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) return false;

    for (unsigned i = 0; i < formatCtx_->nb_streams; i++) {
        AVStream* stream = formatCtx_->streams[i];
        AVCodecParameters* par = stream->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO || !isSupportedVideoCodec(par->codec_id)) continue;
        if (colorStreamIndex_ < 0) {
            colorStreamIndex_ = static_cast<int>(i);
            info_.width = par->width;
            info_.height = par->height;
            info_.color = streamInfoOf(par);
            if (stream->avg_frame_rate.den > 0) {
                info_.frameRate = av_q2d(stream->avg_frame_rate);
            } else if (stream->r_frame_rate.den > 0) {
                info_.frameRate = av_q2d(stream->r_frame_rate);
            }
            if (formatCtx_->duration > 0) {
                info_.durationUs = av_rescale(formatCtx_->duration, 1000000, AV_TIME_BASE);
                info_.totalFrames = static_cast<int>(std::lround(static_cast<double>(formatCtx_->duration) / AV_TIME_BASE * info_.frameRate));
            } else if (stream->nb_frames > 0) {
                info_.totalFrames = static_cast<int>(stream->nb_frames);
            } else {
                info_.totalFrames = 100;
            }
        } else if (alphaStreamIndex_ < 0) {
            alphaStreamIndex_ = static_cast<int>(i);
            info_.alpha = streamInfoOf(par);
        }
    }
    if (colorStreamIndex_ < 0) return false;

    if (alphaStreamIndex_ < 0) {
        info_.alphaInBlockAdditional = hasBlockAdditionalAlpha(formatCtx_, colorStreamIndex_);
        // The probe read packets, whether or not it found alpha
        av_seek_frame(formatCtx_, colorStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
    }
    if (info_.alphaInBlockAdditional) {
        info_.alpha = info_.color;
        info_.alpha.codecPrivate.clear();
        // libvpx's alpha stream says limited range but holds alpha as-is, 0-255
        info_.alpha.colorRange = AVCOL_RANGE_JPEG;
    }
    info_.hasAlpha = alphaStreamIndex_ >= 0 || info_.alphaInBlockAdditional;
    return true;
}

void AlphaDemuxer::resetCounters() {
    colorFrameIndex_ = 0;
    alphaFrameIndex_ = 0;
    hasPendingAlpha_ = false;
}

bool AlphaDemuxer::readPacket(Packet* out) {
    // A colour packet read last time left its BlockAdditional alpha. Empty when it had none.
    if (hasPendingAlpha_) {
        hasPendingAlpha_ = false;
        out->stream = 1;
        out->data = pendingAlpha_.data();
        out->size = static_cast<int>(pendingAlpha_.size());
        out->ptsUs = pendingAlphaPtsUs_;
        out->frameIndex = alphaFrameIndex_++;
        out->key = info_.color.codec == VP8 ? isVp8Keyframe(out->data, out->size) : isVp9Keyframe(out->data, out->size);
        return true;
    }

    av_packet_unref(packet_);
    while (av_read_frame(formatCtx_, packet_) >= 0) {
        const int streamIndex = packet_->stream_index;
        if (streamIndex != colorStreamIndex_ && streamIndex != alphaStreamIndex_) {
            av_packet_unref(packet_);
            continue;
        }
        const bool color = streamIndex == colorStreamIndex_;
        int64_t ptsUs = 0;
        if (packet_->pts != AV_NOPTS_VALUE) {
            ptsUs = av_rescale_q(packet_->pts, formatCtx_->streams[streamIndex]->time_base, AVRational{1, 1000000});
        }
        if (info_.alphaInBlockAdditional) {
            size_t alphaSize = 0;
            const uint8_t* alpha = blockAdditionalAlpha(packet_, &alphaSize);
            pendingAlpha_.assign(alpha, alpha + (alpha ? alphaSize : 0));
            pendingAlphaPtsUs_ = ptsUs;
            hasPendingAlpha_ = true;
        }
        out->stream = color ? 0 : 1;
        out->data = packet_->data;
        out->size = packet_->size;
        out->ptsUs = ptsUs;
        out->frameIndex = color ? colorFrameIndex_++ : alphaFrameIndex_++;
        out->key = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        return true;
    }
    return false;
}

void AlphaDemuxer::seekToStart() {
    av_seek_frame(formatCtx_, colorStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
    resetCounters();
}

void AlphaDemuxer::seekToTimeUs(int64_t timeUs) {
    AVStream* stream = formatCtx_->streams[colorStreamIndex_];
    const int64_t ts = av_rescale_q(timeUs, AVRational{1, 1000000}, stream->time_base);
    av_seek_frame(formatCtx_, colorStreamIndex_, ts, AVSEEK_FLAG_BACKWARD);
    hasPendingAlpha_ = false;
}

} // namespace webm
