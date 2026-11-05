#include "video_encoder.h"

#include <climits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace ebridge {

namespace {

struct Candidate {
    const char* name;
    // Low-latency private options; unknown ones are ignored.
    std::vector<std::pair<const char*, const char*>> options;
};

// Order matters: the Switch decodes in software, and only spreads a frame over
// its cores when the frame is split into slices. NVENC, QSV and x264 honour
// ctx->slices; AMF always writes one slice per frame (checked on an RX 7900 XTX),
// which leaves the Switch on one core and too slow for real games. So on an AMD
// PC x264 on the CPU is used, and AMF only if x264 is missing.
const std::vector<Candidate>& Candidates() {
    static const std::vector<Candidate> list = {
        {"h264_nvenc",
         {{"preset", "p1"}, {"tune", "ull"}, {"zerolatency", "1"}, {"delay", "0"},
          {"rc", "cbr"}, {"forced-idr", "1"}}},
        {"h264_qsv",
         {{"preset", "veryfast"}, {"async_depth", "1"}, {"forced_idr", "1"}}},
        {"libx264",
         {{"preset", "ultrafast"}, {"tune", "zerolatency"}, {"forced-idr", "1"},
          {"x264-params", "slices=4:sliced-threads=1:threads=4"}}},
        {"h264_amf",
         {{"usage", "ultralowlatency"}, {"quality", "speed"}, {"rc", "cbr"}, {"latency", "1"},
          {"async_depth", "1"}, {"forced_idr", "1"}, {"header_spacing", "1"}}},
    };
    return list;
}

std::string AvError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

} // namespace

struct VideoEncoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int64_t pts = 0;

    void Free() {
        if (ctx) avcodec_free_context(&ctx);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (sws) sws_freeContext(sws);
        sws = nullptr;
        pts = 0;
    }
};

VideoEncoder::VideoEncoder() : impl(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() { Close(); }

bool VideoEncoder::IsOpen() const { return impl->ctx != nullptr; }

void VideoEncoder::Close() { impl->Free(); }

bool VideoEncoder::Open(int width, int height, int fps, int bitrate_kbps, std::string* chosen,
                        std::string* error) {
    Close();
    std::string errors;
    for (const auto& c : Candidates()) {
        const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
        if (!codec) continue;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = {1, fps};
        ctx->framerate = {fps, 1};
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        ctx->bit_rate = int64_t{bitrate_kbps} * 1000;
        ctx->rc_max_rate = ctx->bit_rate;
        ctx->rc_buffer_size = static_cast<int>(ctx->bit_rate / fps);  // ~1 frame: low latency
        ctx->gop_size = fps * 5;          // safety keyframe every 5 s, plus on request
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->slices = 4;                  // lets the Switch decode with 4 cores in parallel
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->color_range = AVCOL_RANGE_MPEG;
        for (const auto& [k, v] : c.options) av_opt_set(ctx->priv_data, k, v, 0);

        const int rc = avcodec_open2(ctx, codec, nullptr);
        if (rc < 0) {
            errors += std::string(c.name) + ": " + AvError(rc) + "; ";
            avcodec_free_context(&ctx);
            continue;
        }
        impl->ctx = ctx;
        impl->frame = av_frame_alloc();
        impl->frame->format = AV_PIX_FMT_NV12;
        impl->frame->width = width;
        impl->frame->height = height;
        av_frame_get_buffer(impl->frame, 0);
        impl->packet = av_packet_alloc();
        if (chosen) *chosen = c.name;
        return true;
    }
    if (error) *error = errors.empty() ? "no H.264 encoder in this FFmpeg build" : errors;
    return false;
}

bool VideoEncoder::Encode(const uint8_t* bgra, int src_width, int src_height, int stride,
                          bool force_idr, std::vector<EncodedPacket>& out) {
    auto& d = *impl;
    if (!d.ctx) return false;

    d.sws = sws_getCachedContext(d.sws, src_width, src_height, AV_PIX_FMT_BGRA, d.ctx->width,
                                 d.ctx->height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr,
                                 nullptr);
    if (!d.sws) return false;
    // Full-range RGB in, limited-range BT.709 out (what the Switch renderer expects).
    sws_setColorspaceDetails(d.sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);

    if (av_frame_make_writable(d.frame) < 0) return false;
    const uint8_t* src[1] = {bgra};
    const int src_stride[1] = {stride};
    sws_scale(d.sws, src, src_stride, 0, src_height, d.frame->data, d.frame->linesize);

    d.frame->pts = d.pts++;
    d.frame->pict_type = force_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 29, 100)
    if (force_idr) d.frame->flags |= AV_FRAME_FLAG_KEY;
    else d.frame->flags &= ~AV_FRAME_FLAG_KEY;
#else
    d.frame->key_frame = force_idr ? 1 : 0;
#endif

    if (avcodec_send_frame(d.ctx, d.frame) < 0) return false;
    for (;;) {
        const int rc = avcodec_receive_packet(d.ctx, d.packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return false;
        EncodedPacket p;
        p.data.assign(d.packet->data, d.packet->data + d.packet->size);
        p.key = (d.packet->flags & AV_PKT_FLAG_KEY) != 0;
        out.push_back(std::move(p));
        av_packet_unref(d.packet);
    }
    return true;
}

} // namespace ebridge
