// The HD movie decoder (hd_video.hpp) over FFmpeg, loaded at run time; a stub when built
// without NOXSDL_HD_VIDEO.
#include "hd_video.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#if NOXSDL_HD_VIDEO

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace noxsdl {

namespace {

/// FFmpeg's DLLs, dependencies first (from CMake: the names in the build it was linked with).
constexpr const char* kDlls[] = {NOXSDL_FFMPEG_DLLS};

/// __HrLoadAllImportsForDll raises an exception for an import it can't bind: kept in a function
/// of its own, with no C++ objects, for __try.
bool bindImports(const char* dll)
{
    __try {
        return SUCCEEDED(__HrLoadAllImportsForDll(dll));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Loads every DLL from `dir` by full path, then binds each one's delay-loaded imports; false
// (the VQAs play) at the first that fails.
bool loadDlls(const std::filesystem::path& dir)
{
    for (const char* name : kDlls) {
        const std::filesystem::path p = dir / name;
        if (!LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            SDL_Log("HD video: %s not loaded (error %lu); the VQAs play", p.string().c_str(), GetLastError());
            return false;
        }
    }
    for (const char* name : kDlls) {
        if (!bindImports(name)) {
            SDL_Log("HD video: %s lacks what noxsdl needs (another FFmpeg version?); the VQAs play", name);
            return false;
        }
    }
    SDL_Log("HD video: FFmpeg %s from %s", av_version_info(), dir.string().c_str());
    return true;
}

// FFmpeg's text for an error code.
std::string errorText(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

/// A stream's decoder opened; AV1 by libdav1d when the build has it.
AVCodecContext* openDecoder(AVStream* s)
{
    const AVCodec* codec = nullptr;
    if (s->codecpar->codec_id == AV_CODEC_ID_AV1) {
        codec = avcodec_find_decoder_by_name("libdav1d");
    }
    if (!codec) {
        codec = avcodec_find_decoder(s->codecpar->codec_id);
    }
    if (!codec) {
        return nullptr;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, s->codecpar) < 0 || avcodec_open2(c, codec, nullptr) < 0) {
        avcodec_free_context(&c);
        return nullptr;
    }
    return c;
}

} // namespace

// The open movie's FFmpeg state: the container, the video decoder and its stream, a reusable
// packet and frame, and the converter to RGBA.
struct HdVideo::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    int stream = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    bool flushing = false;
    bool ended = false;

    ~Impl()
    {
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avformat_close_input(&format);
    }
};

bool HdVideo::available(const std::filesystem::path& dllDir)
{
    static int state = -1;  // tried once a run: -1 not yet, 1 loaded, 0 failed
    if (state < 0) {
        state = loadDlls(dllDir) ? 1 : 0;
    }
    return state == 1;
}

HdVideo::~HdVideo() = default;

namespace {

/// The whole sound track as interleaved 16-bit stereo (or mono) at its own rate.
nox::audio::Pcm decodeSound(const std::filesystem::path& file)
{
    nox::audio::Pcm pcm;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, reinterpret_cast<const char*>(file.u8string().c_str()), nullptr, nullptr) < 0) {
        return pcm;
    }
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const int si = avformat_find_stream_info(fmt, nullptr) >= 0
                       ? av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
                       : -1;
    // Converted to 16-bit at the source's rate, mono kept mono and anything more mixed to stereo.
    if (si >= 0 && (dec = openDecoder(fmt->streams[si])) != nullptr) {
        const int channels = std::clamp(dec->ch_layout.nb_channels, 1, 2);
        AVChannelLayout out{};
        av_channel_layout_default(&out, channels);
        if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, dec->sample_rate, &dec->ch_layout, dec->sample_fmt,
                                dec->sample_rate, 0, nullptr) >= 0 &&
            swr_init(swr) >= 0) {
            pcm.rate = static_cast<std::uint32_t>(dec->sample_rate);
            pcm.channels = static_cast<std::uint16_t>(channels);
            const auto drain = [&] {
                while (avcodec_receive_frame(dec, frame) >= 0) {
                    // Room for the most the resampler could give, trimmed to what it gave.
                    const int room = swr_get_out_samples(swr, frame->nb_samples);
                    const std::size_t at = pcm.samples.size();
                    pcm.samples.resize(at + static_cast<std::size_t>(room) * channels);
                    auto* dst = reinterpret_cast<std::uint8_t*>(pcm.samples.data() + at);
                    const int n = swr_convert(swr, &dst, room, const_cast<const std::uint8_t**>(frame->extended_data),
                                              frame->nb_samples);
                    pcm.samples.resize(at + static_cast<std::size_t>(std::max(n, 0)) * channels);
                    av_frame_unref(frame);
                }
            };
            // Every packet of the sound stream, then the decoder flushed for what it still holds.
            while (av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) {
                    drain();
                }
                av_packet_unref(pkt);
            }
            avcodec_send_packet(dec, nullptr);
            drain();
        }
        av_channel_layout_uninit(&out);
    }
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return pcm;
}

} // namespace

std::unique_ptr<HdVideo> HdVideo::open(const std::filesystem::path& file, std::string& error)
{
    std::unique_ptr<HdVideo> v(new HdVideo);
    v->impl_ = std::make_unique<Impl>();
    Impl& m = *v->impl_;
    const std::string utf8 = reinterpret_cast<const char*>(file.u8string().c_str());
    if (const int e = avformat_open_input(&m.format, utf8.c_str(), nullptr, nullptr); e < 0) {
        error = errorText(e);
        return nullptr;
    }
    if (const int e = avformat_find_stream_info(m.format, nullptr); e < 0) {
        error = errorText(e);
        return nullptr;
    }
    m.stream = av_find_best_stream(m.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m.stream < 0) {
        error = "no video stream";
        return nullptr;
    }
    AVStream* s = m.format->streams[m.stream];
    m.video = openDecoder(s);
    if (!m.video) {
        error = std::string("no decoder for ") + avcodec_get_name(s->codecpar->codec_id);
        return nullptr;
    }
    m.packet = av_packet_alloc();
    m.frame = av_frame_alloc();
    v->width_ = m.video->width;
    v->height_ = m.video->height;
    if (s->avg_frame_rate.num > 0 && s->avg_frame_rate.den > 0) {
        v->fps_ = av_q2d(s->avg_frame_rate);
    }
    // The frame count from the stream, else estimated from the file's duration.
    if (s->nb_frames > 0) {
        v->frameCount_ = static_cast<int>(s->nb_frames);
    } else if (m.format->duration > 0) {
        v->frameCount_ = static_cast<int>(static_cast<double>(m.format->duration) / AV_TIME_BASE * v->fps_ + 0.5);
    }
    v->sound_ = decodeSound(file);
    SDL_Log("HD video: %s, %s %dx%d %.3g fps, %d frames, sound %u Hz x %u", file.string().c_str(),
            m.video->codec->name, v->width_, v->height_, v->fps_, v->frameCount_, v->sound_.rate, v->sound_.channels);
    return v;
}

bool HdVideo::nextFrame(std::vector<std::uint8_t>& rgba)
{
    Impl& m = *impl_;
    if (m.ended) {
        return false;
    }
    // Until the decoder gives a frame: feed it the video stream's packets, flushing it at the end
    // of the file.
    for (;;) {
        const int r = avcodec_receive_frame(m.video, m.frame);
        if (r >= 0) {
            break;
        }
        if (r == AVERROR_EOF) {
            m.ended = true;
            return false;
        }
        if (m.flushing) {
            m.ended = true;
            return false;
        }
        // More packets wanted.
        bool sent = false;
        while (!sent) {
            if (av_read_frame(m.format, m.packet) < 0) {
                avcodec_send_packet(m.video, nullptr);
                m.flushing = true;
                break;
            }
            if (m.packet->stream_index == m.stream) {
                avcodec_send_packet(m.video, m.packet);
                sent = true;
            }
            av_packet_unref(m.packet);
        }
    }
    const int w = m.frame->width;
    const int h = m.frame->height;
    width_ = w;
    height_ = h;
    m.sws = sws_getCachedContext(m.sws, w, h, static_cast<AVPixelFormat>(m.frame->format), w, h, AV_PIX_FMT_RGBA,
                                 SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!m.sws) {
        m.ended = true;
        return false;
    }
    // The colours: the file's matrix and range if it says, else BT.601 (swscale's own default,
    // which an FFmpeg encode from RGB used).
    const int space = m.frame->colorspace != AVCOL_SPC_UNSPECIFIED ? static_cast<int>(m.frame->colorspace) : SWS_CS_ITU601;
    const int* coef = sws_getCoefficients(space);
    // Out to full-range RGB: brightness 0, contrast and saturation 1.0 (16.16 fixed point).
    sws_setColorspaceDetails(m.sws, coef, m.frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT),
                             1, 0, 1 << 16, 1 << 16);
    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    std::uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(m.sws, m.frame->data, m.frame->linesize, 0, h, dst, dstStride);
    av_frame_unref(m.frame);
    ++frame_;
    return true;
}

} // namespace noxsdl

#else // NOXSDL_HD_VIDEO

namespace noxsdl {

struct HdVideo::Impl {};

bool HdVideo::available(const std::filesystem::path&)
{
    return false;
}

std::unique_ptr<HdVideo> HdVideo::open(const std::filesystem::path&, std::string& error)
{
    error = "built without HD video";
    return nullptr;
}

HdVideo::~HdVideo() = default;

bool HdVideo::nextFrame(std::vector<std::uint8_t>&)
{
    return false;
}

} // namespace noxsdl

#endif
