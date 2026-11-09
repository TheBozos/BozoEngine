// libmpeg as the native runtime answers it (libraries.md section 9): the PSS demux, the pictures (decoded by FFmpeg's
// mpeg2video decoder when the build has it) delivered to the game's memory as the reference delivered them, the
// decoder-feeding protocol the game observes (when sceMpegGetPicture delivers, waits for a vblank or asks the game for
// data), and the movie's sound to a native sink.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "bozo/ee/kernel.h"

namespace bozo::libs
{
    // A decoded picture (4:2:0, display order).
    struct YuvPicture
    {
        int width = 0, height = 0;
        std::vector<uint8_t> y, u, v; // width x height, and (width / 2) x (height / 2) each
    };

    // The video decoder behind libmpeg: coded pictures in, pictures out in display order.
    class VideoDecoder
    {
    public:
        virtual ~VideoDecoder() = default;
        // One coded picture with the headers before it (sequence, GOP).
        virtual void picture(const uint8_t *data, size_t bytes) = 0;
        // The stream ended: every picture still held comes out.
        virtual void flush() = 0;
        // The next picture in display order, if one is out.
        virtual bool next(YuvPicture &out) = 0;
    };
    // FFmpeg's mpeg2video decoder (null when the build has no FFmpeg: libmpeg then stays answered from a log).
    std::unique_ptr<VideoDecoder> makeVideoDecoder();
    bool haveVideoDecoder();

    // Where a movie's sound goes (the future mixer). TMB's movies carry Sony's PCM in private stream 1: a 4-byte
    // sub-stream header (0xff 0xa0 0 0), then, in the first packet, an "SShd" header (format 1, rate 48000, 2 channels,
    // 512-byte interleave) and an "SSbd" data header; the samples are 16-bit little-endian, 512 bytes of the left channel
    // then 512 of the right (libraries.md section 9).
    struct MovieSoundFormat
    {
        uint32_t format = 0, rate = 0, channels = 0, interleave = 0;
    };
    class MovieSoundSink
    {
    public:
        virtual ~MovieSoundSink() = default;
        // A movie starts (sceMpegCreate) and ends (sceMpegDelete).
        virtual void start() {}
        virtual void stop() {}
        // The sound stream's header, once (the first packet's SShd).
        virtual void format(const MovieSoundFormat &) {}
        // Interleaved 16-bit samples (host order), as they are demuxed; `pts` the packet's (90 kHz, -1 if none).
        virtual void samples(const int16_t *frames, size_t count, int64_t pts) { (void)frames, (void)count, (void)pts; }
    };

    // How libmpeg answers a game: by default as TMB's reference build does (its sceMpeg layout, results and pacing).
    // A port whose game uses libmpeg the SDK's way (The Third Age: sceMpegAddBs from a Nodata callback, its own
    // pacing) sets them; they survive sceMpegCreate.
    struct MpegOptions
    {
        // The reference's layout: sceMpegCreate writes its sceMpeg and work area (with TMB's addresses) and takes a
        // block and each callback's data from the game's heap, sceMpegReset clears TMB's static word, and the results
        // are the reference's. Off: libmpeg writes nothing outside the work area the game gave sceMpegCreate (the
        // Nodata callback's 32-byte data is its first bytes), and the results are the SDK's (sceMpegCreate and
        // sceMpegDelete 1, sceMpegAddCallback the previous callback).
        bool referenceLayout = true;
        // sceMpegGetPicture holds each picture until its two fields are due; off, a game that paces itself gets it at
        // once and sceMpegIsEnd answers from the stream alone.
        bool paced = true;
        // The Nodata callback as the SDK calls it: (mp, callback data, the data given to sceMpegAddCallback); a 0 it
        // returns ends the input (the pictures left come out, then sceMpegIsEnd answers 1).
        bool sdkNodata = false;
    };

    // One sceMpeg (TMB plays one movie at a time).
    struct MpegState
    {
        bool active = false;
        uint32_t mp = 0, work = 0, size = 0, block = 0; // block: the 0x600 bytes taken from the game's heap

        struct Callback
        {
            uint32_t function = 0, data = 0, gp = 0;
            bool set = false;
        };
        Callback callbacks[8];    // sceMpegAddCallback types (1: nodata)
        Callback strCallbacks[8]; // sceMpegAddStrCallback types (0: video, 2: PCM)
        uint32_t callbacksAdded = 0;

        // The demux: bytes taken from the ring so far, those not yet parsed into whole packets, the program end.
        uint64_t consumed = 0;
        std::vector<uint8_t> pending; // bytes [pendingAt, consumed) of the stream
        uint64_t pendingAt = 0;
        uint32_t ring = 0, ringSize = 0, ringBias = 0; // ring address of stream byte x: ring + (x + ringBias) % ringSize
        bool programEnd = false;
        // The port says the game aborted the playback (TMB: its videoDecAbort, a skipped movie; the fork's
        // abortMpegPlayback): sceMpegIsEnd answers 1 from then on (libraries.md section 9.5).
        bool aborted = false;

        // The video elementary stream: its bytes, its start codes counted (pictures and the sequence end), the cuts
        // into coded pictures.
        std::vector<uint8_t> es;
        size_t scanned = 0;
        uint32_t starts = 0;        // picture starts and sequence ends whole in the stream so far
        uint32_t pictureStarts = 0; // picture starts alone
        bool sequenceEnd = false;
        bool inputEnded = false; // the Nodata callback said there is no more input (MpegOptions::sdkNodata)
        bool headerOpen = false;
        std::vector<size_t> cuts; // where each coded picture's data (with its headers) begins
        size_t sent = 0;          // coded pictures given to the decoder
        bool flushed = false;

        // The callbacks a demux call queues as it ends (their data already taken from the heap).
        struct DueCallback
        {
            ee::QueuedHandler handler;
            uint32_t entry = 0;
        };
        std::vector<DueCallback> callbacksDue;
        // The thread waiting in sceMpegGetPicture for data (0: none).
        int32_t waiting = 0;

        // Pictures delivered to the game, and the vblank count the first was due at.
        uint32_t delivered = 0;
        uint64_t base = 0;
        // The vblank count the last picture (not an empty delivery) reached the game at.
        uint64_t lastShown = 0;
        // Diagnostic only (BOZO_TIMING_LOG): host time of the first decoded picture.
        int64_t firstPictureNs = 0;
        std::unique_ptr<VideoDecoder> decoder;
        std::deque<YuvPicture> decoded;

        // The sound stream.
        bool soundHeader = false;
        std::vector<uint8_t> soundCarry;
        MovieSoundFormat sound;
    };
}
