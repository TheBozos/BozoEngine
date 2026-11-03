// libmpeg's video decoder: FFmpeg's mpeg2video (libavcodec, libavutil), linked when the build finds FFmpeg
// (BOZO_FFMPEG_ROOT, else pkg-config: libraries.md section 9). Without it this file is built with BOZO_HAVE_FFMPEG unset
// and libmpeg stays answered from a log.
#include "bozo/libs/mpeg.h"

#ifdef BOZO_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#include <cstring>
#endif

namespace bozo::libs
{
#ifdef BOZO_HAVE_FFMPEG
    namespace
    {
        class Mpeg2Decoder final : public VideoDecoder
        {
        public:
            Mpeg2Decoder()
            {
                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                c_ = codec ? avcodec_alloc_context3(codec) : nullptr;
                if (c_)
                {
                    c_->thread_count = 1; // deterministic, in the caller's thread
                    if (avcodec_open2(c_, codec, nullptr) < 0)
                        avcodec_free_context(&c_);
                }
                packet_ = av_packet_alloc();
                frame_ = av_frame_alloc();
            }
            ~Mpeg2Decoder() override
            {
                av_frame_free(&frame_);
                av_packet_free(&packet_);
                avcodec_free_context(&c_);
            }
            void picture(const uint8_t *data, size_t bytes) override
            {
                if (!c_ || av_new_packet(packet_, static_cast<int>(bytes)) < 0)
                    return;
                std::memcpy(packet_->data, data, bytes);
                avcodec_send_packet(c_, packet_);
                av_packet_unref(packet_);
                drain();
            }
            void flush() override
            {
                if (!c_)
                    return;
                avcodec_send_packet(c_, nullptr);
                drain();
            }
            bool next(YuvPicture &out) override
            {
                if (out_.empty())
                    return false;
                out = std::move(out_.front());
                out_.pop_front();
                return true;
            }

        private:
            void drain()
            {
                while (avcodec_receive_frame(c_, frame_) == 0)
                {
                    YuvPicture p;
                    p.width = frame_->width;
                    p.height = frame_->height;
                    const int cw = p.width / 2, ch = p.height / 2;
                    p.y.resize(static_cast<size_t>(p.width) * p.height);
                    p.u.resize(static_cast<size_t>(cw) * ch);
                    p.v.resize(static_cast<size_t>(cw) * ch);
                    for (int y = 0; y < p.height; ++y)
                        std::memcpy(&p.y[static_cast<size_t>(y) * p.width], frame_->data[0] + y * frame_->linesize[0], p.width);
                    for (int y = 0; y < ch; ++y)
                    {
                        std::memcpy(&p.u[static_cast<size_t>(y) * cw], frame_->data[1] + y * frame_->linesize[1], cw);
                        std::memcpy(&p.v[static_cast<size_t>(y) * cw], frame_->data[2] + y * frame_->linesize[2], cw);
                    }
                    out_.push_back(std::move(p));
                    av_frame_unref(frame_);
                }
            }

            AVCodecContext *c_ = nullptr;
            AVPacket *packet_ = nullptr;
            AVFrame *frame_ = nullptr;
            std::deque<YuvPicture> out_;
        };
    }

    std::unique_ptr<VideoDecoder> makeVideoDecoder() { return std::make_unique<Mpeg2Decoder>(); }
    bool haveVideoDecoder() { return true; }
#else
    std::unique_ptr<VideoDecoder> makeVideoDecoder() { return nullptr; }
    bool haveVideoDecoder() { return false; }
#endif
}
