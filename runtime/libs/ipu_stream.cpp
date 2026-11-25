// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A game's own IPU streams (bozo/libs/ipu_stream.h): FFmpeg's "ipu" parser and decoder, built with BOZO_HAVE_FFMPEG
// like libmpeg's decoder (mpeg_ffmpeg.cpp); without FFmpeg, open() fails.
#include "bozo/libs/ipu_stream.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef BOZO_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

namespace bozo::libs
{
    struct IpuStream::Impl
    {
        std::vector<uint8_t> file;
        int width = 0, height = 0;
        std::vector<std::pair<size_t, size_t>> frames; // (offset, bytes) of each frame in `file`
#ifdef BOZO_HAVE_FFMPEG
        AVCodecContext *codec = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        ~Impl()
        {
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
        }
#endif
    };

    namespace
    {
        uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
        uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
        uint8_t clamp8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

        // The IPU's colour conversion (ITU-R BT.601, video range), in 16.16 fixed point.
        void yuvToRgb(int y, int cb, int cr, uint8_t *out, uint8_t alpha)
        {
            const int c = (y - 16) * 76309, d = cb - 128, e = cr - 128;
            out[0] = clamp8((c + 104597 * e + 32768) >> 16);
            out[1] = clamp8((c - 25675 * d - 53279 * e + 32768) >> 16);
            out[2] = clamp8((c + 132201 * d + 32768) >> 16);
            out[3] = alpha;
        }
    }

    IpuStream::IpuStream() = default;
    IpuStream::~IpuStream() = default;

    bool IpuStream::available()
    {
#ifdef BOZO_HAVE_FFMPEG
        return avcodec_find_decoder(AV_CODEC_ID_IPU) != nullptr;
#else
        return false;
#endif
    }

    bool IpuStream::isOpen() const { return impl_ && !impl_->frames.empty(); }
    int IpuStream::width() const { return impl_ ? impl_->width : 0; }
    int IpuStream::height() const { return impl_ ? impl_->height : 0; }
    int IpuStream::frames() const { return impl_ ? static_cast<int>(impl_->frames.size()) : 0; }

    bool IpuStream::open(const uint8_t *file, size_t bytes)
    {
        impl_.reset();
#ifdef BOZO_HAVE_FFMPEG
        if (!file || bytes < 16 || std::memcmp(file, "ipum", 4) != 0)
            return false;
        auto impl = std::make_unique<Impl>();
        const size_t size = std::min<size_t>(bytes, std::max<size_t>(16, le32(file + 4)));
        impl->file.assign(file, file + size);
        impl->width = le16(file + 8);
        impl->height = le16(file + 10);
        const uint32_t count = le32(file + 12);
        if (impl->width <= 0 || impl->height <= 0 || (impl->width | impl->height) & 15)
            return false;

        const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_IPU);
        AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_IPU);
        if (!decoder || !parser)
        {
            if (parser)
                av_parser_close(parser);
            return false;
        }
        impl->codec = avcodec_alloc_context3(decoder);
        impl->packet = av_packet_alloc();
        impl->frame = av_frame_alloc();
        if (!impl->codec || !impl->packet || !impl->frame)
        {
            av_parser_close(parser);
            return false;
        }
        impl->codec->width = impl->width;
        impl->codec->height = impl->height;
        impl->codec->thread_count = 1;
        if (avcodec_open2(impl->codec, decoder, nullptr) < 0)
        {
            av_parser_close(parser);
            return false;
        }

        // The frames, as the parser splits the stream after the header (as FFmpeg's ipu demuxer feeds it).
        const uint8_t *data = impl->file.data() + 16;
        size_t left = impl->file.size() - 16, at = 16;
        while (true)
        {
            uint8_t *out = nullptr;
            int outBytes = 0;
            const int used = av_parser_parse2(parser, impl->codec, &out, &outBytes, left ? data : nullptr,
                                              static_cast<int>(left), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0)
                break;
            if (outBytes > 0 && out)
            {
                // A frame the parser returns lies in the file (it hands back the input) or in its own buffer when it
                // spans calls; find it in the file by its bytes, from where the previous one ended.
                const uint8_t *found = std::search(impl->file.data() + at, impl->file.data() + impl->file.size(), out,
                                                   out + outBytes);
                if (found == impl->file.data() + impl->file.size())
                    break;
                const size_t offset = static_cast<size_t>(found - impl->file.data());
                impl->frames.emplace_back(offset, static_cast<size_t>(outBytes));
                at = offset + static_cast<size_t>(outBytes);
            }
            if (!left && outBytes <= 0)
                break;
            data += used;
            left -= static_cast<size_t>(used);
        }
        av_parser_close(parser);
        if (impl->frames.empty())
            return false;
        if (count && impl->frames.size() > count)
            impl->frames.resize(count);
        impl_ = std::move(impl);
        return true;
#else
        (void)file;
        (void)bytes;
        return false;
#endif
    }

    bool IpuStream::decodeRaster(int index, uint8_t *out, uint8_t alpha)
    {
#ifdef BOZO_HAVE_FFMPEG
        if (!isOpen() || index < 0 || index >= frames() || !out)
            return false;
        Impl &s = *impl_;
        const auto [offset, bytes] = s.frames[static_cast<size_t>(index)];
        if (av_new_packet(s.packet, static_cast<int>(bytes)) < 0)
            return false;
        std::memcpy(s.packet->data, s.file.data() + offset, bytes);
        const int sent = avcodec_send_packet(s.codec, s.packet);
        av_packet_unref(s.packet);
        if (sent < 0 || avcodec_receive_frame(s.codec, s.frame) < 0)
            return false;
        const AVFrame &f = *s.frame;
        if (f.format != AV_PIX_FMT_YUV420P || f.width < s.width || f.height < s.height)
        {
            av_frame_unref(s.frame);
            return false;
        }
        for (int y = 0; y < s.height; ++y)
        {
            const uint8_t *py = f.data[0] + y * f.linesize[0];
            const uint8_t *pu = f.data[1] + (y / 2) * f.linesize[1];
            const uint8_t *pv = f.data[2] + (y / 2) * f.linesize[2];
            uint8_t *row = out + static_cast<size_t>(y) * s.width * 4;
            for (int x = 0; x < s.width; ++x)
                yuvToRgb(py[x], pu[x / 2], pv[x / 2], row + x * 4, alpha);
        }
        av_frame_unref(s.frame);
        return true;
#else
        (void)index;
        (void)out;
        (void)alpha;
        return false;
#endif
    }

    bool IpuStream::decodeMacroblocks(int index, uint8_t *out, uint8_t alpha)
    {
        if (!isOpen() || !out)
            return false;
        const int w = impl_->width, h = impl_->height;
        std::vector<uint8_t> raster(static_cast<size_t>(w) * h * 4);
        if (!decodeRaster(index, raster.data(), alpha))
            return false;
        // Macroblocks in raster order, each one's 16 rows of 16 pixels.
        uint8_t *dst = out;
        for (int my = 0; my < h / 16; ++my)
            for (int mx = 0; mx < w / 16; ++mx)
                for (int y = 0; y < 16; ++y)
                {
                    std::memcpy(dst, raster.data() + (static_cast<size_t>(my * 16 + y) * w + mx * 16) * 4, 16 * 4);
                    dst += 16 * 4;
                }
        return true;
    }
}
