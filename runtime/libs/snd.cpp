// The 989SND sound driver behind the SIF RPC (libraries.md section 15): requests decoded, the driver's state kept,
// the reference's replies made. See bozo/libs/snd.h.
#include "bozo/libs/libraries.h"

#include <cmath>
#include <cstring>

namespace bozo::libs
{
    namespace
    {
        constexpr double kEeClock = 294912000.0;

        uint32_t le32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        uint16_t le16(const uint8_t *p)
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

        // Reads `bytes` from the disc at `lbn` + `offset`.
        bool discBytes(Libraries &l, uint32_t lbn, uint32_t offset, uint32_t bytes, std::vector<uint8_t> &out)
        {
            const uint32_t first = offset / kSectorBytes, last = (offset + bytes + kSectorBytes - 1) / kSectorBytes;
            std::vector<uint8_t> s(static_cast<size_t>(last - first) * kSectorBytes);
            if (!discRead(l, lbn + first, last - first, s.data()))
                return false;
            out.assign(s.begin() + (offset - first * kSectorBytes), s.begin() + (offset - first * kSectorBytes) + bytes);
            return true;
        }

        // A bank file: a chunk table (u32 1, u32 count, then offset and size of each), chunk 0 the SBv2 header, chunk
        // 1 the samples (SPU memory), chunk 2 (when present) MIDI data.
        bool parseBank(Libraries &l, uint32_t lbn, SndBank &b, std::vector<std::pair<uint32_t, uint32_t>> &chunks)
        {
            std::vector<uint8_t> head;
            if (!discBytes(l, lbn, 0, kSectorBytes, head))
                return false;
            const uint32_t count = le32(&head[4]);
            if (count < 2 || count > 8)
                return false;
            chunks.clear();
            for (uint32_t i = 0; i < count; ++i)
                chunks.emplace_back(le32(&head[8 + 8 * i]), le32(&head[12 + 8 * i]));
            std::vector<uint8_t> h;
            if (!discBytes(l, lbn, chunks[0].first, chunks[0].second, h) || h.size() < 0x28 || std::memcmp(h.data(), "SBv2", 4) != 0)
                return false;
            discBytes(l, lbn, chunks[1].first, chunks[1].second, b.samples);
            if (count > 2)
                discBytes(l, lbn, chunks[2].first, chunks[2].second, b.midi);
            b.lbn = lbn;
            b.id.assign(reinterpret_cast<const char *>(&h[0x0C]), 4);
            const int16_t sounds = static_cast<int16_t>(le16(&h[0x14])), programs = static_cast<int16_t>(le16(&h[0x16]));
            const uint32_t firstSound = le32(&h[0x1C]), firstProgram = le32(&h[0x20]);
            for (int i = 0; i < sounds; ++i)
            {
                const uint32_t at = firstSound + 28u * static_cast<uint32_t>(i);
                if (at + 28 > h.size())
                    break;
                SndSound s;
                std::memcpy(s.raw, &h[at], 28);
                s.type = static_cast<int32_t>(le32(&h[at]));
                s.program = h[at + 13];
                s.note = h[at + 14];
                b.sounds.push_back(s);
            }
            for (int p = 0; p < programs; ++p)
            {
                const uint32_t at = firstProgram + 8u * static_cast<uint32_t>(p);
                std::vector<SndTone> tones;
                if (at + 8 <= h.size())
                {
                    const uint32_t n = h[at], off = le32(&h[at + 4]);
                    for (uint32_t k = 0; k < n && off + 24 * k + 24 <= h.size(); ++k)
                    {
                        const uint8_t *t = &h[off + 24 * k];
                        SndTone tone;
                        tone.priority = t[0];
                        tone.volume = t[1];
                        tone.centerNote = t[2];
                        tone.centerFine = t[3];
                        tone.pan = static_cast<int16_t>(le16(t + 4));
                        tone.mapLow = t[6];
                        tone.mapHigh = t[7];
                        tone.bendDown = t[8];
                        tone.bendUp = t[9];
                        tone.adsr1 = le16(t + 10);
                        tone.adsr2 = le16(t + 12);
                        tone.flags = le16(t + 14);
                        tone.vag = le32(t + 16);
                        // The sample's 16-byte ADPCM blocks to the one flagged as its end (flag bit 0), which loops
                        // back when bit 1 is set too.
                        for (uint32_t q = tone.vag; q + 16 <= b.samples.size(); q += 16)
                        {
                            ++tone.vagBlocks;
                            const uint8_t f = b.samples[q + 1];
                            if (f & 1)
                            {
                                tone.loops = (f & 3) == 3;
                                break;
                            }
                        }
                        tones.push_back(tone);
                    }
                }
                b.programs.push_back(std::move(tones));
            }
            return true;
        }

        // The native estimate of a one-shot sound's life (free mode): its longest tone's sample at the note's pitch
        // (48 kHz at the tone's centre note).
        // A sound's native lifetime estimate (libraries.md section 15.5): the longest of its tones in the note's key
        // range, its sample (ADPCM blocks to the end flag, 28 samples each) at 48 kHz shifted by the note's distance
        // from the tone's centre note and the play's pitch bend (the tone's bend range, bytes 8-9, in semitones), then
        // 1.166 s more (settled on the two natural ends match1p shows: sound 3 of bank 0xe8310 started at EE cycles
        // 39,259,868,387 and 46,869,503,443, both ended 2.9002-2.9169 s later). -1: it plays until stopped (a looping tone).
        double lifetime(const SndBank &b, const SndSound &s, int32_t bend)
        {
            if ((s.type != 1 && s.type != 2) || s.program >= b.programs.size())
                return -1;
            double longest = 0;
            for (const SndTone &t : b.programs[s.program])
            {
                if (s.note < t.mapLow || s.note > t.mapHigh)
                    continue;
                if (t.loops)
                    return -1;
                const double semis = (int(t.centerNote) - int(s.note)) -
                                     (bend >= 0 ? bend * t.bendUp : bend * t.bendDown) / 32767.0;
                longest = std::max(longest, t.vagBlocks * 28.0 / 48000.0 * std::pow(2.0, semis / 12.0));
            }
            return longest + 1.166;
        }

        // Which commands return a result (the others leave the driver's last result in their reply slot).
        bool returnsResult(uint32_t id)
        {
            switch (id)
            {
            case 0x03: case 0x05: case 0x0A: case 0x0C: case 0x11: case 0x19: case 0x1A: case 0x1C: case 0x1D:
            case 0x21: case 0x26: case 0x28: case 0x2C: case 0x30: case 0x31: case 0x32: case 0x33: case 0x36:
            case 0x3B: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x47: case 0x48:
                return true;
            default:
                return false;
            }
        }
    }

    bool SndDriver::alive(Libraries &l, Guest &g, uint32_t handle) const
    {
        auto it = handles.find(handle);
        if (it == handles.end())
        {
            it = unseen_.find(handle);
            if (it == unseen_.end())
                return false;
        }
        const Handle &h = it->second;
        if (h.stopped || h.ended)
            return false;
        return h.seconds < 0 || (l.now(g) - h.start) / kEeClock < h.seconds;
    }

    uint32_t SndDriver::modelHandle(Libraries &l, Guest &g, uint32_t pool)
    {
        // The lowest slot of the pool whose sound is not playing; its generation one more (a slot's first is 2).
        std::vector<Slot> &slots = slots_[pool];
        if (slots.size() < 64)
            slots.resize(64);
        for (uint32_t i = 0; i < slots.size(); ++i)
            if (!slots[i].occupant || !alive(l, g, slots[i].occupant))
                return (pool << 24) | (i << 16) | ((slots[i].generation + 1) & 0xFFFFu);
        return 0;
    }

    void SndDriver::occupy(uint32_t handle)
    {
        std::vector<Slot> &slots = slots_[handle >> 24];
        if (slots.size() < 64)
            slots.resize(64);
        const uint32_t i = (handle >> 16) & 0xFFu;
        if (i < slots.size())
        {
            slots[i].generation = handle & 0xFFFFu;
            slots[i].occupant = handle;
        }
    }

    void SndDriver::stopHandle(Handle &h)
    {
        if (h.stopped || h.ended)
            return;
        h.stopped = true;
        if (sink)
        {
            if (h.type < 0)
                sink->streamStopped(h.handle);
            else
                sink->soundStopped(h.handle);
        }
    }

    uint32_t SndDriver::loadBank(Libraries &l, uint32_t lbn, uint32_t slot, const LogWord *log, bool follow, uint32_t id)
    {
        SndBank b;
        std::vector<std::pair<uint32_t, uint32_t>> chunks;
        uint32_t address = 0;
        if (parseBank(l, lbn, b, chunks))
        {
            // The bank's header takes an IOP heap block, and its MIDI data (the third chunk's inner chunk: its size at
            // 12) the next one. A bank with MIDI data starts 4 bytes into its block (libraries.md section 15.4).
            const bool midi = chunks.size() > 2;
            b.block = l.sif.heap.allocate(chunks[0].second + (midi ? 4u : 0u));
            if (midi && b.midi.size() >= 16)
                b.midiBlock = l.sif.heap.allocate(le32(&b.midi[12]));
            address = b.block ? b.block + (midi ? 4u : 0u) : 0;
        }
        if (follow || (!address && log))
        {
            uint32_t v = 0;
            if (log && (*log)(slot, v))
            {
                if (follow && v != address)
                    ++stats.followMismatch;
                if (!follow)
                    ++stats.pinned[id];
                address = v;
            }
        }
        else
            ++stats.native[id];
        if (address)
        {
            b.address = address;
            if (sink)
                sink->bankLoaded(b);
            banks[address] = std::move(b);
        }
        return address;
    }

    uint32_t SndDriver::liveness(Libraries &l, Guest &g, uint32_t id, uint32_t handle, uint32_t slot, const LogWord *log, bool follow)
    {
        auto it = handles.find(handle);
        if (follow)
        {
            uint32_t v = 0;
            if (log)
                (*log)(slot, v);
            if (it != handles.end() && v == 0)
                it->second.ended = true;
            return v;
        }
        if (it == handles.end())
        {
            // A handle the driver never gave: 0, unless it may be one given inside a call answered from the log whose
            // request was lost (then the reference decides).
            uint32_t v = 0;
            if (handle != 0 && handle != 0xFFFFFFFFu && (handle & 0x80000000u) && log && (*log)(slot, v))
            {
                ++stats.pinned[id];
                if (v)
                    handles[handle] = Handle{handle, 0, 0, 1, false, false, l.now(g), -1};
                return v;
            }
            ++stats.native[id];
            return 0;
        }
        Handle &h = it->second;
        if (h.stopped || h.ended)
        {
            ++stats.native[id];
            return 0;
        }
        const double elapsed = (l.now(g) - h.start) / kEeClock;
        // Looping programs and MIDI sequences play until stopped; a stream until its data is played.
        if (h.type == 3 || h.type == 4 || (h.type < 0 && (h.seconds < 0 || elapsed + 1.0 < h.seconds)))
        {
            ++stats.native[id];
            return handle;
        }
        // A one-shot sound (or a stream near its end) lives as long as its voices: the reference's IOP time decides.
        const bool estimate = h.seconds < 0 || elapsed < h.seconds;
        uint32_t v = 0;
        if (log && (*log)(slot, v))
        {
            ++stats.pinned[id];
            if ((v != 0) != estimate)
                ++stats.estimateWrong;
        }
        else
        {
            ++stats.native[id];
            v = estimate ? handle : 0;
        }
        if (v == 0)
            h.ended = true;
        return v;
    }

    uint32_t SndDriver::command(Libraries &l, Guest &g, uint32_t id, const uint32_t a[7], uint32_t slot, const LogWord *log,
                                bool follow, bool &returns)
    {
        returns = returnsResult(id);
        const uint32_t idx = id < 0x50 ? id : 0x4F;
        auto pinnedOr = [&](uint32_t native, bool timeDecides) {
            uint32_t v = native;
            if (follow || timeDecides)
            {
                if (log && (*log)(slot, v))
                {
                    if (!follow)
                        ++stats.pinned[idx];
                    return v;
                }
            }
            if (!follow)
                ++stats.native[idx];
            return native;
        };
        auto count = [&] {
            if (!follow)
                ++stats.native[idx];
        };
        switch (id)
        {
        case 0x03: // BankLoadByLoc(lbn, offset): the bank's IOP address
            return loadBank(l, a[0], slot, log, follow, idx);
        case 0x06: // UnloadBank(bank)
        {
            auto it = banks.find(a[0]);
            if (it != banks.end())
            {
                l.sif.heap.release(it->second.block);
                if (it->second.midiBlock)
                    l.sif.heap.release(it->second.midiBlock);
                if (sink)
                    sink->bankUnloaded(a[0]);
                banks.erase(it);
            }
            count();
            return 0;
        }
        case 0x09: // SetMasterVolume(group, volume)
            masterVolume[a[0] & 31] = static_cast<int32_t>(a[1]);
            masterSet[a[0] & 31] = true;
            if (sink)
                sink->masterVolume(a[0] & 31, static_cast<int32_t>(a[1]));
            count();
            return 0;
        case 0x0A: // GetMasterVolume(group): 0x400 (full) until set
            return pinnedOr(masterSet[a[0] & 31] ? static_cast<uint32_t>(masterVolume[a[0] & 31]) : 0x400u, false);
        case 0x0B: // SetPlaybackMode
            playbackMode = a[0];
            count();
            return 0;
        case 0x0E: // SetReverbType(core, type)
            if (sink)
                sink->reverb(a[0], static_cast<int32_t>(a[1]), -1, -1);
            count();
            return 0;
        case 0x0F: // SetReverbDepth(core, left, right)
            if (sink)
                sink->reverb(a[0], -1, static_cast<int32_t>(a[1]), static_cast<int32_t>(a[2]));
            count();
            return 0;
        case 0x11: // PlaySoundVolPanPMPB(bank, sound, volume, pan, pitch modifier, pitch bend): the sound's handle
        case 0x12: // the same, asynchronous (no handle for the game)
        {
            auto b = banks.find(a[0]);
            const SndSound *s = b != banks.end() && a[1] < b->second.sounds.size() ? &b->second.sounds[a[1]] : nullptr;
            uint32_t handle = 0;
            if (id == 0x11)
            {
                if (!s || s->type == 0)
                {
                    // An empty sound (or none) never plays: 0.
                    handle = follow ? pinnedOr(0, false) : (count(), 0u);
                }
                else
                {
                    // The handle names a handler slot and its generation; which slot is free depends on which sounds
                    // have ended: the reference's IOP time decides (section 15.5). The native model's handle is
                    // compared with it.
                    const uint32_t model = modelHandle(l, g, s->type == 4 ? 0x81u : 0x80u);
                    handle = pinnedOr(model, true);
                    if (log && !follow)
                        ++(handle == model ? stats.modelAgrees : stats.modelDiffers);
                }
                if (handle && s)
                {
                    handles[handle] = Handle{handle, a[0], a[1], s->type, false, false, l.now(g), lifetime(b->second, *s, static_cast<int32_t>(a[5]))};
                    occupy(handle);
                }
            }
            else
            {
                // The game gets no handle, but the sound takes a handler slot all the same.
                if (s && s->type != 0)
                {
                    const uint32_t model = modelHandle(l, g, s->type == 4 ? 0x81u : 0x80u);
                    if (model)
                    {
                        unseen_[model] = Handle{model, a[0], a[1], s->type, false, false, l.now(g), lifetime(b->second, *s, static_cast<int32_t>(a[5]))};
                        occupy(model);
                    }
                }
                count();
            }
            if (sink && s && s->type != 0 && (id == 0x12 || handle))
                sink->soundStarted(handle, b->second, a[1], static_cast<int32_t>(a[2]), static_cast<int32_t>(a[3]),
                                   static_cast<int32_t>(a[4]), static_cast<int32_t>(a[5]), l.now(g));
            return handle;
        }
        case 0x15: // StopSound(handle)
        case 0x2F: // StopVAGStream(handle)
            if (auto it = handles.find(a[0]); it != handles.end())
                stopHandle(it->second);
            count();
            return 0;
        case 0x18: // StopAllSounds
            for (auto &[k, h] : handles)
                if (h.type >= 0)
                    stopHandle(h);
            unseen_.clear();
            count();
            return 0;
        case 0x34: // StopAllStreams
            for (auto &[k, h] : handles)
                if (h.type < 0)
                    stopHandle(h);
            count();
            return 0;
        case 0x19: // SoundIsStillPlaying(handle): the handle, or 0
            return liveness(l, g, idx, a[0], slot, log, follow);
        case 0x21: // SetSoundParams(handle, mask, volume, pan, pitch modifier, pitch bend): the handle, or 0
        {
            const uint32_t v = liveness(l, g, idx, a[0], slot, log, follow);
            if (v && sink)
                sink->soundParams(a[0], a[1], static_cast<int32_t>(a[2]), static_cast<int32_t>(a[3]),
                                  static_cast<int32_t>(a[4]), static_cast<int32_t>(a[5]));
            return v;
        }
        case 0x20: // SetSoundPitchModifier(handle, modifier)
            if (sink)
                sink->soundPitchModifier(a[0], static_cast<int32_t>(a[1]));
            count();
            return 0;
        case 0x2A: // InitVAGStreamingEx(streams, buffer bytes): the streams' buffers and state in IOP memory
            streamBlock = l.sif.heap.allocate(a[0] * a[1] + 0x6200u);
            count();
            return 0;
        case 0x2C: // PlayVAGStreamByLoc(left, right, flags, ..., volume...): the stream's handle
        {
            const uint32_t model = modelHandle(l, g, 0x84u);
            const uint32_t handle = pinnedOr(model, true);
            if (log && !follow)
                ++(handle == model ? stats.modelAgrees : stats.modelDiffers);
            if (handle)
            {
                occupy(handle);
                // Its length from the left VAGp header (big-endian data size at 12, rate at 16): 28 samples a block.
                double seconds = -1;
                std::vector<uint8_t> v;
                if (discBytes(l, a[0], 0, 64, v) && std::memcmp(v.data(), "VAGp", 4) == 0 && be32(&v[16]))
                    seconds = be32(&v[12]) / 16.0 * 28.0 / be32(&v[16]);
                handles[handle] = Handle{handle, 0, 0, -1, false, false, l.now(g), seconds};
                if (sink)
                    sink->streamStarted(handle, a[0], a[1], a[2], static_cast<int32_t>(a[4]), l.now(g));
            }
            return handle;
        }
        case 0x2D: // PauseVAGStream(handle)
            if (sink)
                sink->streamPaused(a[0]);
            count();
            return 0;
        case 0x3B: // InitMovieSound(...): the movie's sound buffer in IOP memory (0x6000 bytes and its state)
        {
            const uint32_t native = l.sif.heap.allocate(0x6010u);
            const uint32_t v = pinnedOr(native, false);
            if (follow && v != native)
                ++stats.followMismatch;
            movieBuffer = native;
            return v;
        }
        case 0x3C: // CloseMovieSound
            if (movieBuffer)
                l.sif.heap.release(movieBuffer);
            movieBuffer = 0;
            count();
            return 0;
        default:
            if (returns)
                return pinnedOr(0, true); // a result the driver does not model: the reference's
            count();
            return 0;
        }
    }


    void SndDriver::call(Libraries &l, Guest &g, uint32_t fno, const std::vector<uint32_t> &request, uint32_t recv, uint32_t rsize,
                         std::vector<uint32_t> &reply, const LogWord *log, bool follow)
    {
        const uint32_t words = rsize / 4;
        reply.assign(words, 0);
        auto put = [&](uint32_t i, uint32_t v) {
            if (i < words)
                reply[i] = v;
        };
        if (fno == 0x4A)
        {
            // A batch: the count, then 32-byte commands (seven argument words, the command id); the reply is -1, a
            // word per command, -1.
            const uint32_t n = request.empty() ? 0 : request[0];
            put(0, 0xFFFFFFFFu);
            for (uint32_t k = 0; k < n && 1 + 8 * k + 8 <= request.size(); ++k)
            {
                const uint32_t *c = &request[1 + 8 * k];
                bool returns = false;
                const uint32_t r = command(l, g, c[7], c, recv + 4 + 4 * k, log, follow, returns);
                if (returns)
                    last = r;
                put(1 + k, last);
                if (follow)
                    ++stats.followed;
            }
            put(n + 1, 0xFFFFFFFFu);
            return;
        }
        // A direct call: the request is its arguments; the reply -1, the result, -1.
        uint32_t a[7] = {};
        for (size_t i = 0; i < 7 && i < request.size(); ++i)
            a[i] = request[i];
        bool returns = false;
        const uint32_t r = command(l, g, fno, a, recv + 4, log, follow, returns);
        if (returns)
            last = r;
        put(0, 0xFFFFFFFFu);
        put(1, last);
        put(2, 0xFFFFFFFFu);
        if (follow)
            ++stats.followed;
    }

    void SndDriver::followTraced(Libraries &l, Guest &g, const std::vector<std::array<uint32_t, 4>> &calls)
    {
        LogWord memory = [&g](uint32_t a, uint32_t &v) {
            v = g.read32(a);
            return true;
        };
        for (size_t i = 0; i < calls.size(); ++i)
        {
            const uint32_t fno = calls[i][1], send = calls[i][3];
            auto r = recvFor_.find(send);
            if (r == recvFor_.end())
            {
                ++stats.lost;
                continue;
            }
            const uint32_t recv = r->second;
            // A later call with the same receive buffer overwrote this one's reply (and, in a batch, its request).
            bool reused = false;
            for (size_t j = i + 1; j < calls.size(); ++j)
                if (auto o = recvFor_.find(calls[j][3]); o != recvFor_.end() && o->second == recv)
                    reused = true;
            if (reused)
            {
                ++stats.lost;
                continue;
            }
            std::vector<uint32_t> request, reply;
            uint32_t rsize = 12;
            if (fno == 0x4A)
            {
                // The batch's count is zeroed once the client switched back to this buffer: the reply's closing -1
                // gives it.
                uint32_t n = 0;
                while (n < 128 && g.read32(recv + 4 + 4 * n) != 0xFFFFFFFFu)
                    ++n;
                request.push_back(n);
                for (uint32_t k = 0; k < 8 * n; ++k)
                    request.push_back(g.read32(send + 4 + 4 * k));
                rsize = 4 * n + 8;
            }
            else if (send)
                for (uint32_t k = 0; k < 7; ++k)
                    request.push_back(g.read32(send + 4 * k));
            call(l, g, fno, request, recv, rsize, reply, &memory, true);
        }
    }
}
