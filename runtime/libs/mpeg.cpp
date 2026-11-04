// libmpeg (libraries.md section 9): sceMpegInit, sceMpegCreate, sceMpegDelete, sceMpegReset, sceMpegAddCallback,
// sceMpegAddStrCallback, sceMpegDemuxPssRing, sceMpegGetPicture, sceMpegIsEnd, and the reference's stub of the game's
// own videoCallback. Every rule here was settled on the call logs (the records are named in libraries.md).
#include "bozo/libs/libraries.h"
#include "bozo/render/movie.h"

#include "bozo/ee/dispatcher.h"

#include <algorithm>
#include <cstring>

namespace bozo::libs
{
    namespace
    {
        constexpr uint32_t kPicturesAhead = 8;   // the demux stops this many pictures ahead of the game
        constexpr uint32_t kFieldsPerPicture = 2; // TMB's movies: 29.97 pictures a second, two fields each
        constexpr uint32_t kCbNodata = 1;          // sceMpegAddCallback type
        constexpr uint32_t kStrVideo = 0, kStrPcm = 2; // sceMpegAddStrCallback types

        MpegState &state(Libraries &l) { return l.mpeg; }

        // ---- The picture as the game receives it ----

        uint8_t clamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

        // RGBA32, alpha 0x80, in 16 x 16 macroblocks in column-major order (macroblock (x, y) at x * rows + y), each
        // pixel from its Y and its 2 x 2 block's Cb and Cr with the reference's fixed-point terms (13 fraction bits,
        // each product shifted on its own).
        std::vector<uint8_t> toGame(const YuvPicture &p)
        {
            const int w = p.width, h = p.height, mbw = w / 16, mbh = h / 16;
            std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
            size_t o = 0;
            for (int mx = 0; mx < mbw; ++mx)
                for (int my = 0; my < mbh; ++my)
                    for (int yy = 0; yy < 16; ++yy)
                        for (int xx = 0; xx < 16; ++xx)
                        {
                            const int x = mx * 16 + xx, y = my * 16 + yy;
                            const int Y = p.y[static_cast<size_t>(y) * w + x];
                            const size_t ci = static_cast<size_t>(y / 2) * (w / 2) + x / 2;
                            const int U = p.u[ci] - 128, V = p.v[ci] - 128;
                            const int l = (9539 * (Y - 16)) >> 13;
                            out[o++] = clamp(l + ((13074 * V) >> 13));
                            out[o++] = clamp(l + ((-3210 * U) >> 13) + ((-6660 * V) >> 13));
                            out[o++] = clamp(l + ((16524 * U) >> 13));
                            out[o++] = 0x80;
                        }
            return out;
        }

        // ---- The video elementary stream ----

        // New bytes of the video stream: its start codes counted (pictures and the sequence end, once whole in the
        // stream) and the cuts into coded pictures (each picture's data begins at its first sequence or GOP header, else
        // at its picture header).
        void scan(MpegState &s)
        {
            size_t i = s.scanned;
            while (i + 4 <= s.es.size())
            {
                if (s.es[i] == 0 && s.es[i + 1] == 0 && s.es[i + 2] == 1)
                {
                    const uint8_t code = s.es[i + 3];
                    if (code == 0x00)
                    {
                        if (!s.headerOpen)
                            s.cuts.push_back(i);
                        s.headerOpen = false;
                        ++s.starts;
                        ++s.pictureStarts;
                    }
                    else if (code == 0xB3 || code == 0xB8)
                    {
                        if (!s.headerOpen)
                            s.cuts.push_back(i);
                        s.headerOpen = true;
                    }
                    else if (code == 0xB7)
                    {
                        ++s.starts;
                        s.sequenceEnd = true;
                    }
                    i += 4;
                    continue;
                }
                ++i;
            }
            s.scanned = i;
        }

        bool ended(const MpegState &s) { return s.sequenceEnd || s.programEnd; }

        // Whether display picture k can be decoded: its data and the next picture's are whole (the decoder holds one
        // picture back), or the stream has ended and its data is whole.
        bool available(const MpegState &s, uint32_t k)
        {
            if (ended(s))
                return true; // past the last picture: an empty delivery (libraries.md section 9)
            const uint32_t complete = s.starts ? s.starts - 1 : 0;
            return k + 1 < complete;
        }

        // Coded pictures to the decoder as their data becomes whole; at the end, the rest and a flush.
        void pump(MpegState &s)
        {
            if (!s.decoder)
                return;
            while (s.sent + 1 < s.cuts.size())
            {
                s.decoder->picture(s.es.data() + s.cuts[s.sent], s.cuts[s.sent + 1] - s.cuts[s.sent]);
                ++s.sent;
            }
            if (ended(s) && !s.flushed)
            {
                if (s.sent < s.cuts.size())
                {
                    s.decoder->picture(s.es.data() + s.cuts[s.sent], s.es.size() - s.cuts[s.sent]);
                    ++s.sent;
                }
                s.decoder->flush();
                s.flushed = true;
            }
            YuvPicture p;
            while (s.decoder->next(p))
                s.decoded.push_back(std::move(p));
        }

        // ---- The sound stream ----

        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

        void sound(Libraries &l, MpegState &s, const uint8_t *payload, size_t bytes, int64_t pts)
        {
            if (bytes < 4)
                return;
            const uint8_t *p = payload + 4; // the sub-stream header
            size_t n = bytes - 4;
            if (!s.soundHeader && n >= 40 && std::memcmp(p, "SShd", 4) == 0)
            {
                s.sound.format = le32(p + 8);
                s.sound.rate = le32(p + 12);
                s.sound.channels = le32(p + 16);
                s.sound.interleave = le32(p + 20);
                s.soundHeader = true;
                if (l.movieSound)
                    l.movieSound->format(s.sound);
                const size_t header = 8 + le32(p + 4) + 8; // SShd and its size, then "SSbd" and its size
                p += header;
                n -= std::min(n, header);
            }
            if (!l.movieSound || !s.soundHeader || s.sound.channels == 0 || s.sound.interleave == 0)
                return;
            s.soundCarry.insert(s.soundCarry.end(), p, p + n);
            const size_t block = static_cast<size_t>(s.sound.interleave) * s.sound.channels;
            const size_t blocks = s.soundCarry.size() / block;
            if (!blocks)
                return;
            const size_t perChannel = s.sound.interleave / 2;
            std::vector<int16_t> frames(blocks * perChannel * s.sound.channels);
            for (size_t b = 0; b < blocks; ++b)
                for (size_t c = 0; c < s.sound.channels; ++c)
                    for (size_t i = 0; i < perChannel; ++i)
                    {
                        const uint8_t *q = s.soundCarry.data() + b * block + c * s.sound.interleave + i * 2;
                        frames[(b * perChannel + i) * s.sound.channels + c] = static_cast<int16_t>(q[0] | (q[1] << 8));
                    }
            s.soundCarry.erase(s.soundCarry.begin(), s.soundCarry.begin() + static_cast<std::ptrdiff_t>(blocks * block));
            l.movieSound->samples(frames.data(), blocks * perChannel, pts);
        }

        // ---- The demux ----

        int64_t timestamp(const uint8_t *b)
        {
            return (static_cast<int64_t>((b[0] >> 1) & 7) << 30) | (static_cast<int64_t>(b[1]) << 22) |
                   (static_cast<int64_t>(b[2] >> 1) << 15) | (static_cast<int64_t>(b[3]) << 7) | (b[4] >> 1);
        }

        // A stream packet whole in the demux: its callback, if the game registered one for its stream (its data, the
        // callback queued as a handler on the calling thread, kind 4), and the decoder's or the sound sink's share of it.
        void packet(Libraries &l, Guest &g, MpegState &s, uint64_t at, uint8_t id, size_t payload, size_t length, int64_t pts,
                    int64_t dts)
        {
            const uint8_t *data = s.pending.data() + (at - s.pendingAt) + payload;
            int type = -1;
            if (id == 0xE0)
            {
                type = kStrVideo;
                s.es.insert(s.es.end(), data, data + length);
                scan(s);
            }
            else if (id == 0xBD)
            {
                type = kStrPcm;
                sound(l, s, data, length, pts);
            }
            if (type < 0 || !s.strCallbacks[type].set)
                return;
            const MpegState::Callback &cb = s.strCallbacks[type];
            const uint64_t streamAt = at + payload;
            const uint32_t pointer = s.ring + static_cast<uint32_t>((streamAt + s.ringBias) % s.ringSize);
            // The callback's data: 32 bytes from the game's heap, given back as its handler ends (the reference's; its
            // addresses show where the game's own blocks are).
            const uint32_t entry = l.gameHeap().allocate(0x20);
            uint8_t e[32] = {};
            const uint32_t words[4] = {static_cast<uint32_t>(type), 0, pointer, static_cast<uint32_t>(length)};
            std::memcpy(e, words, 16);
            std::memcpy(e + 16, &pts, 8);
            std::memcpy(e + 24, &dts, 8);
            g.write(entry, e, 32);
            ee::QueuedHandler h;
            h.kind = ee::kInvokeRpcCallback;
            h.pc = cb.function;
            h.a[0] = s.mp;
            h.a[1] = entry;
            h.a[2] = cb.data;
            h.gp = cb.gp;
            h.completion = true;
            s.callbacksDue.push_back({h, entry}); // queued once the call's demux is done
        }

        // Whole packets out of the bytes taken so far (none after the program end).
        void parse(Libraries &l, Guest &g, MpegState &s)
        {
            size_t i = 0;
            const std::vector<uint8_t> &p = s.pending;
            if (s.programEnd)
                i = p.size();
            while (!s.programEnd && i + 4 <= p.size())
            {
                if (!(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
                {
                    ++i;
                    continue;
                }
                const uint8_t id = p[i + 3];
                size_t total = 0;
                if (id == 0xBA)
                {
                    if (i + 14 > p.size())
                        break;
                    total = (p[i + 4] >> 6) == 1 ? 14 + (p[i + 13] & 7) : 12;
                }
                else if (id == 0xB9)
                {
                    // The program end: counted as one more start towards the demux's limit; what follows is not read.
                    s.programEnd = true;
                    ++s.starts;
                    i = p.size();
                    break;
                }
                else if (id >= 0xBB)
                {
                    if (i + 6 > p.size())
                        break;
                    total = 6 + ((p[i + 4] << 8) | p[i + 5]);
                }
                else
                {
                    ++i;
                    continue;
                }
                if (i + total > p.size())
                    break;
                if (id == 0xE0 || id == 0xBD || (id >= 0xC0 && id < 0xE0))
                {
                    const uint8_t flags = p[i + 7];
                    const size_t payload = 9 + p[i + 8];
                    const int64_t pts = (flags & 0x80) ? timestamp(&p[i + 9]) : -1;
                    const int64_t dts = (flags & 0x40) ? timestamp(&p[i + 14]) : -1;
                    packet(l, g, s, s.pendingAt + i, id, payload, total - payload, pts, dts);
                }
                i += total;
            }
            s.pending.erase(s.pending.begin(), s.pending.begin() + static_cast<std::ptrdiff_t>(i));
            s.pendingAt += i;
        }

        // ---- The functions ----

        // sceMpegInit(): 0.
        void init(Libraries &, Guest &g) { g.ret(0); }

        // sceMpegCreate(mp, work, size): the sceMpeg and its work area as the reference sets them, 0x600 bytes from the
        // game's heap; returns work + 0x118.
        void create(Libraries &l, Guest &g)
        {
            MpegState &s = state(l);
            const uint32_t mp = g.arg(0), work = g.arg(1), size = g.arg(2);
            s = MpegState{};
            s.active = true;
            s.mp = mp;
            s.work = work;
            s.size = size;
            s.block = l.gameHeap().allocate(0x600);
            s.decoder = makeVideoDecoder();
            const uint32_t table[9] = {0x171A50, 0x171C58, 0x171CC0, 0x171D28, 0x171D90, 0x171AB8, 0x171B20, 0x171B88, 0x171BF0};
            g.write32(0x1717BC, mp);
            g.write(0x171800, table, sizeof(table));
            g.fill(mp, 0, 8);
            g.fill(mp + 0x10, 0xFF, 16);
            g.fill(mp + 0x28, 0xFF, 16);
            g.write32(mp + 0x40, work);
            g.write32(work + 0x1C, 0x1209F8);
            g.write32(work + 0x24, 0x120A08);
            g.write32(work + 0x44, s.block);
            g.write32(work + 0x80, 0xFFFFFFFFu);
            g.fill(work + 0x94, 0xFF, 12);
            g.write8(work + 0xB0, 1);
            g.write32(work + 0xD8, 0);
            g.write32(work + 0xE4, 0);
            g.fill(work + 0xF0, 0xFF, 8);
            g.write32(work + 0x108, work + 0x118);
            g.write32(work + 0x10C, size - 0x118);
            g.write32(work + 0x110, work + 0x118);
            g.write32(work + 0x114, work + 0x118);
            if (l.movieSound)
                l.movieSound->start();
            g.retU(work + 0x118);
        }

        // sceMpegDelete(mp): 0.
        void destroy(Libraries &l, Guest &g)
        {
            MpegState &s = state(l);
            s.active = false;
            s.decoder.reset();
            s.decoded.clear();
            if (l.movieSound)
                l.movieSound->stop();
            g.ret(0);
        }

        // sceMpegReset(mp): the picture count and libmpeg's static word at 0x171904 cleared (no result).
        void reset(Libraries &, Guest &g)
        {
            g.write32(g.arg(0) + 8, 0);
            g.write32(0x171904, 0);
        }

        // sceMpegAddCallback(mp, type, function, data): how many callbacks are set (1, 2, ...).
        void addCallback(Libraries &l, Guest &g)
        {
            MpegState &s = state(l);
            const uint32_t type = g.arg(1);
            if (type < 8)
                s.callbacks[type] = {g.arg(2), g.arg(3), g.context()->r[28].uw[0], true};
            g.ret(static_cast<int32_t>(++s.callbacksAdded));
        }

        // sceMpegAddStrCallback(mp, type, channel, function, data): 0.
        void addStrCallback(Libraries &l, Guest &g)
        {
            MpegState &s = state(l);
            const uint32_t type = g.arg(1);
            if (type < 8)
                s.strCallbacks[type] = {g.arg(3), g.arg(4), g.context()->r[28].uw[0], true};
            g.ret(0);
        }

        // sceMpegDemuxPssRing(mp, data, bytes, ring, ringSize): a sector at a time while fewer than kPicturesAhead
        // pictures are ahead of the game (picture starts, the sequence end and the program end whole in the stream, less
        // those delivered; nothing after the program end is read); each whole packet of a registered stream gets its
        // callback. Returns the bytes taken.
        void demux(Libraries &l, Guest &g)
        {
            MpegState &s = state(l);
            const uint32_t data = g.arg(1), bytes = g.arg(2);
            s.ring = g.arg(3);
            s.ringSize = g.arg(4);
            if (!s.ringSize)
            {
                g.ret(0);
                return;
            }
            s.ringBias = static_cast<uint32_t>(((data - s.ring) + s.ringSize - (s.consumed % s.ringSize)) % s.ringSize);
            uint32_t taken = 0;
            while (taken + 0x800 <= bytes && s.starts < s.delivered + kPicturesAhead)
            {
                const uint32_t offset = static_cast<uint32_t>((s.consumed + s.ringBias) % s.ringSize);
                for (uint32_t k = 0; k < 0x800; ++k)
                    s.pending.push_back(g.read8(s.ring + (offset + k) % s.ringSize));
                s.consumed += 0x800;
                taken += 0x800;
                parse(l, g, s);
            }
            // A thread waiting for the data that is now there is woken first, then the packets' callbacks are queued.
            if (s.waiting && available(s, s.delivered))
            {
                l.dispatcher()->kernel().wake(s.waiting, 0, true, ee::kWaitMpeg);
                s.waiting = 0;
            }
            for (const MpegState::DueCallback &c : s.callbacksDue)
            {
                const uint32_t entry = c.entry;
                l.queueHandler(c.handler, [entry](Libraries &l2, Guest &) { l2.gameHeap().release(entry); });
            }
            s.callbacksDue.clear();
            g.ret(static_cast<int32_t>(taken));
        }

        // sceMpegIsEnd(mp): 1 once the stream has ended, every picture was delivered and the last one's display time is
        // over (the vblank count past the first's plus two fields a picture); until then a sceMpegGetPicture gets an
        // empty delivery.
        void isEnd(Libraries &l, Guest &g)
        {
            const MpegState &s = state(l);
            const uint64_t endsAt = s.base + static_cast<uint64_t>(kFieldsPerPicture) * s.pictureStarts;
            g.ret(ended(s) && s.delivered >= s.pictureStarts && l.dispatcher()->kernel().vblankCount() > endsAt ? 1 : 0);
        }

        // The picture to the game: its pixels, the sceMpeg's size and picture index, the work area's buffer and size.
        void deliver(Libraries &l, Guest &g, uint32_t rgb, uint32_t mbcount)
        {
            MpegState &s = state(l);
            pump(s);
            if (!s.decoded.empty()) // none past the last picture: an empty delivery
            {
                const YuvPicture &p = s.decoded.front();
                const std::vector<uint8_t> pixels = toGame(p);
                g.write(rgb, pixels.data(), static_cast<uint32_t>(pixels.size()));
                // A port that shows the movie itself (bozo/render/movie.h) gets the picture in rows.
                if (const bozo::render::MoviePictureHook hook = bozo::render::moviePictureHook())
                {
                    const int w = p.width, h = p.height, mbh = h / 16;
                    std::vector<uint8_t> rows(pixels.size());
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            const size_t from = ((static_cast<size_t>(x / 16) * mbh + y / 16) * 256 + (y % 16) * 16 + x % 16) * 4;
                            std::memcpy(&rows[(static_cast<size_t>(y) * w + x) * 4], &pixels[from], 4);
                        }
                    hook(rgb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), rows.data());
                }
                g.write32(s.mp, static_cast<uint32_t>(p.width));
                g.write32(s.mp + 4, static_cast<uint32_t>(p.height));
                s.decoded.pop_front();
            }
            if (s.delivered == 0)
                s.base = l.dispatcher()->kernel().vblankCount() - 1;
            g.write32(s.mp + 8, s.delivered);
            g.write32(s.work + 0xD8, rgb);
            g.write32(s.work + 0xE4, mbcount);
            ++s.delivered;
            g.ret(0);
        }

        void getPictureOn(Libraries &l, Guest &g, uint32_t rgb, uint32_t mbcount);

        // sceMpegGetPicture(mp, rgb32, mbcount): the next picture. Not decodable yet: the game's nodata callback is queued
        // as a library handler (kind 7) and the thread waits for libmpeg's picture (its end wakes it; as it resumes
        // this is decided again). Not due yet (two fields a picture from the first): a vblank wait, the picture
        // delivered as the thread resumes. Else delivered now; 0.
        void getPicture(Libraries &l, Guest &g) { getPictureOn(l, g, g.arg(1), g.arg(2)); }

        void getPictureOn(Libraries &l, Guest &g, uint32_t rgb, uint32_t mbcount)
        {
            MpegState &s = state(l);
            ee::Kernel &k = l.dispatcher()->kernel();
            if (!available(s, s.delivered))
            {
                const uint8_t data[32] = {static_cast<uint8_t>(kCbNodata)};
                const uint32_t entry = l.gameHeap().allocate(0x20);
                g.write(entry, data, 32);
                const MpegState::Callback &cb = s.callbacks[kCbNodata];
                ee::QueuedHandler h;
                h.kind = ee::kInvokeLibraryCall;
                h.pc = cb.function;
                h.a[0] = s.mp;
                h.a[1] = entry;
                h.gp = cb.gp;
                h.completion = true;
                const int32_t thread = k.running();
                // Its end wakes the thread, unless a demux already did.
                l.queueHandler(h, [thread, entry](Libraries &l2, Guest &) {
                    l2.gameHeap().release(entry);
                    MpegState &s2 = state(l2);
                    if (s2.waiting == thread)
                    {
                        s2.waiting = 0;
                        l2.dispatcher()->kernel().wake(thread, 0, true, ee::kWaitMpeg);
                    }
                });
                l.io.ipuDemand = true; // the IPU's input was taken (libraries.md section 11)
                s.waiting = thread;
                l.waitFor(ee::kWaitMpeg, 1, s.mp, 0, [rgb, mbcount](Libraries &l2, Guest &g2) { getPictureOn(l2, g2, rgb, mbcount); });
                return;
            }
            if (s.delivered > 0 && !(ended(s) && s.delivered >= s.pictureStarts)) // the empty delivery is not paced
            {
                const uint64_t due = s.base + kFieldsPerPicture * s.delivered;
                if (k.vblankCount() <= due)
                {
                    l.waitVblank(due, -1, [rgb, mbcount](Libraries &l2, Guest &g2) { deliver(l2, g2, rgb, mbcount); });
                    return;
                }
            }
            deliver(l, g, rgb, mbcount);
        }

        // The reference's stub of TMB's videoCallback (the game's own, 0x1ec2c8): 1, nothing copied (the decoder takes
        // the video from the demux).
        void videoCallback(Libraries &, Guest &g) { g.ret(1); }
    }

    void addMpeg(std::vector<Function> &out)
    {
        if (!haveVideoDecoder())
            return;
        out.push_back({"sceMpegInit", kMpeg, init});
        out.push_back({"sceMpegCreate", kMpeg, create});
        out.push_back({"sceMpegDelete", kMpeg, destroy});
        out.push_back({"sceMpegReset", kMpeg, reset});
        out.push_back({"sceMpegAddCallback", kMpeg, addCallback});
        out.push_back({"sceMpegAddStrCallback", kMpeg, addStrCallback});
        out.push_back({"sceMpegDemuxPssRing", kMpeg, demux});
        out.push_back({"sceMpegGetPicture", kMpeg, getPicture});
        out.push_back({"sceMpegIsEnd", kMpeg, isEnd});
        out.push_back({"videoCallback__FP7sceMpegP16sceMpegCbDataStrPv", kMpeg, videoCallback});
    }
}
