// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// 989SND's behaviour on the native mixer. Section references are to docs/specs/989snd.md (Part A programs, voices
// and the tick; Part B MIDI; Part C streams) and docs/specs/iop-timing.md for the clock. See bozo/libs/snd989.h.
#include "bozo/libs/snd989.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bozo::libs::snd989
{
    namespace
    {
        constexpr uint64_t kTick = 153600;      // IOP cycles of the 240 Hz tick
        constexpr int kVoices = 48;
        constexpr int kPrograms = 48, kMidis = 32, kEffectNodes = 64;

        // libsd entries (id | voice << 1 | core).
        constexpr uint32_t kVolL = 0x000, kVolR = 0x100, kPitch = 0x200, kAdsr1 = 0x300, kAdsr2 = 0x400, kEnvx = 0x500;
        constexpr uint32_t kKon = 0x1500, kKoff = 0x1600, kVmixL = 0x1800, kVmixEL = 0x1900, kVmixR = 0x1A00,
                           kVmixER = 0x1B00;
        constexpr uint32_t kEea = 0x1D00, kSsa = 0x2040, kLsax = 0x2140, kNax = 0x2240;

        int32_t s8(uint8_t v) { return static_cast<int8_t>(v); }
        uint32_t le32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        int32_t le16s(const uint8_t *p)
        {
            int16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        uint32_t le16(const uint8_t *p)
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        int32_t tdiv(int64_t a, int64_t b) { return static_cast<int32_t>(a / b); } // C++ division truncates

        // The cost model (IOP instructions, the replay's means): a command's own time inside its RPC call, and the
        // tick's (iop-timing.md section 4-5). libraries.md 15.13b: outcomes do not depend on these within a factor 2.
        uint32_t commandCost(uint32_t id)
        {
            switch (id)
            {
            case 0x00: return 8947;
            case 0x03: return 2614;
            case 0x06: return 1367;
            case 0x08: return 7109;
            case 0x09: return 779;
            case 0x0a: return 46;
            case 0x0b: return 84;
            case 0x0e: return 2431;
            case 0x0f: return 201;
            case 0x11: return 1380;
            case 0x12: return 2941;
            case 0x15: return 1502;
            case 0x18: return 17944;
            case 0x19: return 128;
            case 0x20: return 1404;
            case 0x21: return 3795;
            case 0x2a: return 1935;
            case 0x2c: return 1128;
            case 0x2d: return 240;
            default: return 150;
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    struct Engine::Impl
    {
        Engine &e;
        explicit Impl(Engine &engine) : e(engine) {}

        // libsd's exports through Libsd::call, as 989SND calls them: each brings the clocked mixer up to the IOP's
        // cycle first (bozo/audio/libsd.h), at the engine's current time (Engine::m_now).
        struct Sd
        {
            Impl &I;
            uint32_t c(uint32_t ordinal, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0)
            {
                const uint32_t args[5] = {a0, a1, a2, a3, a4};
                uint32_t r = 0;
                I.e.m_libsd->call(ordinal, args, 0, r);
                if (I.traceCalls && !I.inBatch)
                    I.callLog.push_back({ordinal, a0 & 0xFFFFu, I.e.m_now - I.tickService, r});
                return r;
            }
            // sceSdProcBatch: one libsd call for several entries (traced as one).
            void begin()
            {
                if (I.traceCalls)
                    I.callLog.push_back({15, 0, I.e.m_now - I.tickService, 0});
                I.inBatch = true;
            }
            void end() { I.inBatch = false; }
            void setParam(uint32_t en, uint32_t v) { c(5, en, v); }
            uint32_t getParam(uint32_t en) { return c(6, en); }
            void setSwitch(uint32_t en, uint32_t v) { c(7, en, v); }
            uint32_t getSwitch(uint32_t en) { return c(8, en); }
            void setAddr(uint32_t en, uint32_t v) { c(9, en, v); }
            uint32_t getAddr(uint32_t en) { return c(10, en); }
            uint32_t voiceTrans(uint32_t ch, uint32_t mode, uint32_t iop, uint32_t spu, uint32_t size) { return c(17, ch, mode, iop, spu, size); }
        };
        Sd sd() { return Sd{*this}; }
        struct CallRec
        {
            uint32_t ordinal, entry;
            uint64_t offset;
            uint32_t result;
        };
        bool traceCalls = false, inBatch = false;
        uint64_t tickService = 0;
        std::vector<CallRec> callLog;

        // The cost model: 989SND's instruction counts per step, fitted on the analysis replay (libraries.md 15.13b):
        // each step advances the IOP clock (Engine::m_now), so libsd calls happen at the reference's times.
        enum Tok : uint32_t
        {
            TickBase, VoicePending, VoiceRead, VoiceZeroOff, VoiceZeroOn, VoiceZeroEnd, VoiceZeroNone, Callback,
            CallbackFree, FreeHandler, FreeVoice, EndVoice, UnlinkStep, LinkStep, HandlerProgram, HandlerMidi,
            HandlerStream, HandlerEffect, SeqFirst, SeqDec, SeqEvent, SeqDelta, SeqDeltaTicks, EvNoteOn, EvNoteOff,
            EvProgram, EvBend, EvTempo, EvEnd, EvCtl, NoteOnBase, SelectTone, NoteOnVoice, AllocScan, AllocSteal,
            KeyOnVoice, NoteOffStep, NoteOffMatch, BendStep, BendMatch, FlushKoffNone, FlushKoff, FlushKonNone,
            FlushKon, TickEnd, BufTick, StreamTick, VolPanVoice, PitchVoice, KeyOnPitch, NoteOffOwner, NoteOffCh, BendOwner, SelectLow, TokCount
        };
        std::array<int32_t, TokCount> cost{};
        void tok(Tok t, uint32_t n = 1)
        {
            e.m_now = static_cast<uint64_t>(static_cast<int64_t>(e.m_now) + static_cast<int64_t>(cost[t]) * n);
            if (traceCalls)
                for (uint32_t i = 0; i < n; ++i)
                    callLog.push_back({1000u + t, 0, e.m_now - tickService, 0});
        }

        // ---- the sound memory allocator (989SND's free list: a size-ordered tree of 65 nodes) ----
        struct Node
        {
            uint32_t start = 0, size = 0;
            bool used = false;
            int parent = -1, left = -1, right = -1;
        };
        std::array<Node, 65> nodes{};
        int root = -1;

        void treeInsert(int n)
        {
            if (n < 0)
                return;
            nodes[n].used = true;
            if (root < 0)
            {
                root = n;
                nodes[n].parent = -1;
                return;
            }
            int cur = root;
            for (;;)
            {
                if (nodes[n].size < nodes[cur].size)
                {
                    if (nodes[cur].left < 0)
                    {
                        nodes[cur].left = n;
                        nodes[n].parent = cur;
                        return;
                    }
                    cur = nodes[cur].left;
                }
                else
                {
                    if (nodes[cur].right < 0)
                    {
                        nodes[cur].right = n;
                        nodes[n].parent = cur;
                        return;
                    }
                    cur = nodes[cur].right;
                }
            }
        }
        void treeRemove(int n)
        {
            Node &x = nodes[n];
            if (n == root)
                root = x.right;
            else
            {
                Node &p = nodes[x.parent];
                if (p.left == n)
                    p.left = x.right;
                else
                    p.right = x.right;
            }
            if (x.right >= 0)
                nodes[x.right].parent = x.parent;
            treeInsert(x.left); // the left subtree, hung again by its root's size
            x.used = false;
            x.left = x.right = -1;
        }
        int treeFind(int n, uint32_t size) const
        {
            while (n >= 0)
            {
                if (nodes[n].size >= size)
                    return n;
                n = nodes[n].right;
            }
            return -1;
        }
        static uint32_t round64(uint32_t s) { return (s & 63u) ? s + 64u - (s & 63u) : s; }
        uint32_t spuAlloc(uint32_t size)
        {
            size = round64(size);
            int n = treeFind(root, size);
            if (n < 0)
                return 0;
            for (int m; (m = treeFind(nodes[n].left, size)) >= 0;)
                n = m;
            const uint32_t at = nodes[n].start;
            treeRemove(n);
            nodes[n].size -= size;
            if (nodes[n].size)
            {
                nodes[n].start += size;
                treeInsert(n);
            }
            return at;
        }
        void spuFree(uint32_t addr, uint32_t size)
        {
            size = round64(size);
            int slot = 65;
            for (int i = 0; i < 65; ++i)
            {
                Node &n = nodes[i];
                if (n.used)
                {
                    if (n.start + n.size == addr)
                    {
                        treeRemove(i);
                        size += n.size;
                        addr = n.start;
                    }
                    else if (n.start == addr + size)
                    {
                        treeRemove(i);
                        size += n.size;
                    }
                    else
                        continue;
                }
                if (i < slot)
                    slot = i;
            }
            if (slot < 65)
            {
                nodes[slot] = Node{addr, size, false, -1, -1, -1};
                treeInsert(slot);
            }
        }
        // 0x5038: takes [addr, addr + size) out of the node holding it.
        void spuCarve(uint32_t addr, uint32_t size)
        {
            size = round64(size);
            const uint32_t end = addr + size;
            int firstFree = 65;
            int64_t rest = 0;
            uint32_t restAt = 0;
            for (int i = 0; i < 65; ++i)
            {
                Node &n = nodes[i];
                if (!n.used)
                {
                    if (i < firstFree)
                        firstFree = i;
                    continue;
                }
                if (addr < n.start || addr >= n.start + n.size)
                    continue;
                treeRemove(i);
                if (addr == n.start)
                {
                    n.start = end;
                    n.size -= size;
                    if (n.size)
                        treeInsert(i);
                    return;
                }
                const uint32_t old = n.size;
                if (end == n.start + old)
                {
                    n.size -= size;
                    if (n.size)
                        treeInsert(i);
                    return;
                }
                n.size = addr - n.start;
                treeInsert(i);
                rest = static_cast<int64_t>(old) - n.size - size;
                restAt = end;
            }
            if (rest > 0 && firstFree < 65)
            {
                nodes[firstFree] = Node{restAt, static_cast<uint32_t>(rest), false, -1, -1, -1};
                treeInsert(firstFree);
            }
        }
        // 0x4e1c: the smallest node (in index order) with room below a 128 KB boundary at its end; taken there.
        uint32_t spuAllocAligned(uint32_t size)
        {
            size = round64(size);
            int best = -1;
            for (int i = 0; i < 65; ++i)
            {
                const Node &n = nodes[i];
                if (!n.used || n.size < size)
                    continue;
                const uint32_t end = n.start + n.size;
                if ((end - (end & 0x1FFFFu)) - n.start < size)
                    continue;
                if (best < 0 || n.size < nodes[best].size)
                    best = i;
            }
            if (best < 0)
                return 0;
            const uint32_t at = ((nodes[best].start + nodes[best].size) & ~0x1FFFFu) - size;
            spuCarve(at, size);
            return at;
        }

        // ---- banks ----
        struct Bank;
        struct Midi
        {
            Bank *owner = nullptr;          // the bank file it came in
            uint32_t id = 0, bankId = 0;
            Bank *bank = nullptr;           // the bank whose programs it plays (resolved at load)
            const uint8_t *data = nullptr;  // the event data
            uint32_t tempo = 0, ppq = 0;
        };
        struct Block
        {
            Bank *owner = nullptr;
            uint32_t id = 0;
            bool multi = false;
            Midi *mid = nullptr;
            std::vector<Midi *> subs;
        };
        struct Bank
        {
            uint32_t address = 0, id = 0;
            int32_t number = 0;
            std::vector<uint8_t> h, midi;
            uint32_t spu = 0, spuBytes = 0;
            int32_t soundCount = 0, programCount = 0;
            uint32_t sounds = 0, programs = 0;
            std::vector<std::unique_ptr<Midi>> mids;
            const uint8_t *sound(int i) const { return &h[sounds + 28u * static_cast<uint32_t>(i)]; }
            const uint8_t *program(int i) const { return &h[programs + 8u * static_cast<uint32_t>(i)]; }
        };
        std::vector<std::unique_ptr<Bank>> banks; // load order
        std::vector<Block> blocks;                 // the MIDI block list (load order)

        Bank *bankAt(uint32_t address)
        {
            for (auto &b : banks)
                if (b->address == address)
                    return b.get();
            return nullptr;
        }
        Bank *bankOf(const uint8_t *p)
        {
            for (auto &b : banks)
                if (!b->h.empty() && p >= b->h.data() && p < b->h.data() + b->h.size())
                    return b.get();
            return nullptr;
        }

        // ---- voices (Part A 1.2) ----
        struct Handler;
        enum class Cb : uint8_t { None, Program, Midi };
        struct Voice
        {
            int next = -1;
            int state = 0;
            Handler *owner = nullptr;
            Cb cb = Cb::None;
            const uint8_t *tone = nullptr;
            int32_t note = 0, fine = 0;
            uint8_t prio = 0;
            int32_t group = 0;
            uint32_t age = 0;
            int32_t volL = 0, volR = 0, pb = 0, pm = 0;
            int32_t ch = 0, vel = 0, sustained = 0;
            const uint8_t *prog = nullptr;
        };
        std::array<Voice, kVoices> V{};
        std::array<uint8_t, kVoices> silent{};
        int voiceHead = -1;
        uint32_t now = 0;
        uint32_t reverbMask[2] = {}, pendingKon[2] = {}, pendingKoff[2] = {}, keyedOn[2] = {}, keyedOff[2] = {};
        int32_t groupVol[8] = {1024, 1024, 1024, 127, 127, 127, 127, 1024};
        int32_t playbackMode = 0;
        int32_t panL[180] = {}, panR[180] = {};

        // ---- handlers (Part A 1.3, Part B 2.1, Part C 2.2) ----
        struct Buffer;
        struct Handler
        {
            uint32_t handle = 0;
            int type = 0;
            bool used = false; // sound (+4) non-zero
            const uint8_t *sound = nullptr;
            int32_t sndVol = 0, sndPan = 0, vol = 0, pan = 0, pm = 0, pb = 0;
            uint8_t flags = 0;
            int32_t group = 0;
            uint32_t mask[2] = {};
            Handler *next = nullptr;
            int effect = -1; // the auto-pan node (the only effect TMB's commands make)
            int32_t note = 0, fine = 0;
            // MIDI
            Bank *bank = nullptr;
            const uint8_t *loopStart = nullptr, *ptr = nullptr;
            uint32_t count = 0, tempo = 0, ppq = 0;
            int32_t x = 0, step = 0, rem = 0;
            uint32_t mute = 0, sustain = 0;
            int32_t prog[16] = {}, chVol[16] = {}, chPan[16] = {}, chBend[16] = {};
            uint8_t first = 0, runStatus = 0, loops = 0;
            // stream
            Buffer *bufL = nullptr;
        };
        std::array<Handler, kPrograms> H{};
        std::array<Handler, kMidis> M{};
        std::vector<Handler> S;
        Handler *hHead = nullptr, *hTail = nullptr;

        struct EffectNode
        {
            uint32_t flags = 0;
            int32_t count = 0, mode = 0, step = 0, period = 0, target = 0;
        };
        std::array<EffectNode, kEffectNodes> nodesFx{};

        // ---- streams (Part C) ----
        struct Request
        {
            uint32_t flags = 0, spu = 0, last = 0, bytes = 0;
            std::vector<uint8_t> iop;
        };
        struct Buffer
        {
            uint32_t flags = 1, size = 0, spuA = 0;
            Request req[2];
            uint32_t half64 = 0, lastNax = 0, swapTick = 0, err = 0;
            int voice = -1;
            uint32_t nextLbn = 0, firstLbn = 0, off = 0, toRead = 0, total = 0, played = 0, rate = 0, last124 = 0;
            int32_t vol = 0, pan = 0, group = 0;
            uint32_t pitch = 0;
            Buffer *R = nullptr;
            Handler *stream = nullptr;
        };
        std::vector<std::unique_ptr<Buffer>> buffers;
        bool streaming = false;
        struct Pending
        {
            uint64_t at;
            Buffer *b;
        };
        std::vector<Pending> keyOns; // keyOnStream moments (the stream threads' pipeline)
        bool keyOnPending = false;

        // ===========================================================================================================
        void init()
        {
            // Fitted on the analysis replay's shell (22,536 ticks, 95% of 241,428 libsd call times exact; collinear
            // steps share their sum, hence the negative ones).
            static constexpr int32_t kDefault[TokCount] = {
                55, 19, 32, 54, 77, 71, 0, 23, 38, 38, 54, 69, 7, 6, 23, 63, 0, 0, 59, 4, -4, 170, 25, 129, -34, -78,
                13, -33, 0, 0, 129, 8, 129, 11, 0, 129, 8, -2, 8, 116, -16, 9, 20, 84, 123, 60, 30, 80, 80, 96, 4, 4,
                1, 6};
            std::copy(std::begin(kDefault), std::end(kDefault), cost.begin());
            for (int i = 0; i < 180; ++i)
            {
                const double a = i * 0.5 * 3.14159265358979323846 / 180.0;
                panL[i] = static_cast<int32_t>(std::floor(16383.0 * std::cos(a) + 1e-9));
                panR[i] = static_cast<int32_t>(std::floor(16383.0 * std::sin(a) + 1e-9));
            }
            panL[90] = panR[90] = 11584;
            panR[1] = 182;
            panR[0] = 0;
            for (int i = 0; i < kVoices; ++i)
                V[i] = Voice{};
            for (int i = 0; i < kPrograms; ++i)
            {
                H[i] = Handler{};
                H[i].handle = static_cast<uint32_t>(i) << 16 | 1u;
                H[i].type = 0;
            }
            for (int i = 0; i < kMidis; ++i)
            {
                M[i] = Handler{};
                M[i].handle = 0x01000001u | static_cast<uint32_t>(i) << 16;
                M[i].type = 1;
            }
            spuFree(0x5040, 0x1FAFBF);
        }

        // ---- Note2Pitch (libsd: 2^(n/12) and 2^(f/1536) tables) ----
        static uint32_t semitone(int i) { return static_cast<uint32_t>(std::floor(32768.0 * std::exp2(i / 12.0))); }
        static uint32_t fineStep(int i) { return static_cast<uint32_t>(std::floor(32768.0 * std::exp2(i / 1536.0))); }
        static uint32_t note2pitch(int32_t cn, int32_t cf, uint32_t n, int32_t f)
        {
            int32_t a3 = static_cast<int16_t>(f) + static_cast<int32_t>(cf & 0xFFFF);
            const int32_t v1 = a3 >= 0 ? a3 >> 7 : (a3 + 127) >> 7;
            const int32_t off = static_cast<int16_t>(static_cast<uint16_t>(n + v1 - cn));
            const int32_t oct = off / 12; // truncating
            int32_t t0 = oct - 2, a1 = off - oct * 12;
            a3 -= v1 * 128;
            if (a1 < 0)
            {
                a1 += 12;
                t0 = oct - 3;
            }
            uint32_t p = (semitone(a1) * fineStep(a3)) >> 16;
            if (t0 < 0)
            {
                const int s = -t0;
                p = (p + (1u << (s - 1))) >> s;
            }
            return p & 0xFFFFu;
        }
        // Part A 3.1
        uint32_t voicePitch(const uint8_t *tone, int32_t pb, int32_t pm, int32_t note, int32_t fine)
        {
            int32_t x = note * 128 + fine + pm;
            if (pb >= 0)
                x += tdiv(static_cast<int64_t>(s8(tone[9])) * (pb * 128), 32767);
            else
                x += tdiv(static_cast<int64_t>(s8(tone[8])) * (pb * 128), 32768);
            const int32_t n = x / 128, f = x - n * 128;
            const int32_t cn = s8(tone[2]), cf = s8(tone[3]);
            if (traceCalls)
                callLog.push_back({13, static_cast<uint32_t>(cn >= 0 ? cn : -cn) & 0xFFFFu, e.m_now - tickService, 0});
            uint32_t p;
            if (cn >= 0)
            {
                p = note2pitch(cn, cf, static_cast<uint32_t>(n) & 0xFFFF, f) & 0xFFFF;
                p = static_cast<uint32_t>(static_cast<uint64_t>(p) * 44100 / 48000);
            }
            else
                p = note2pitch(-cn, cf, static_cast<uint32_t>(n) & 0xFFFF, f) & 0xFFFF;
            return p & 0xFFFF;
        }
        // Part A 2.2
        int32_t groupScale(int32_t vol, int32_t group) const
        {
            const int32_t x = tdiv(static_cast<int64_t>(vol) * groupVol[group & 7], 1024);
            const int32_t s = x < 0 ? -1 : 1;
            return static_cast<int16_t>(s * tdiv(static_cast<int64_t>(x) * x, 32512));
        }
        // Part A 3.2
        void voiceVolPan(int32_t master, int32_t vol, int32_t pan, int32_t progVol, int32_t progPan, int32_t toneVol,
                         int32_t tonePan, int32_t &outL, int32_t &outR) const
        {
            int32_t t = tdiv(static_cast<int64_t>(vol << 8) * toneVol, 127);
            t = tdiv(static_cast<int64_t>(t) * progVol, 127);
            t = tdiv(static_cast<int64_t>(t) * master, 127);
            if (t == 0)
            {
                outL = outR = 0;
                return;
            }
            if (playbackMode == 1)
            {
                outL = outR = t;
                return;
            }
            int32_t a = pan + tonePan + progPan;
            while (a >= 360)
                a -= 360;
            a = a < 270 ? a + 90 : a - 270;
            a = std::clamp(a, 0, 359);
            const int32_t oldL = outL, oldR = outR;
            int32_t L, R;
            if (a < 180)
            {
                L = tdiv(static_cast<int64_t>(t) * panL[a], 16383);
                R = tdiv(static_cast<int64_t>(t) * panR[a], 16383);
                if (oldL < 0 && oldR < 0)
                {
                    L = -L;
                    R = -R;
                }
                else if ((oldL < 0) != (oldR < 0))
                {
                    const int32_t s = std::abs(oldR) < std::abs(oldL) ? (oldL < 0 ? -1 : 1) : (oldR < 0 ? -1 : 1);
                    L *= s;
                    R *= s;
                }
            }
            else
            {
                a -= 180;
                R = tdiv(static_cast<int64_t>(t) * panL[a], 16383);
                L = tdiv(static_cast<int64_t>(t) * panR[a], 16383);
                if (playbackMode != 2)
                {
                    if (oldL >= 0 && oldR >= 0)
                        (R < L) ? (R = -R) : (L = -L);
                    else if (oldL >= 0 && oldR < 0)
                        R = -R;
                    else if (oldL < 0 && oldR >= 0)
                        L = -L;
                    else
                        (L < R) ? (R = -R) : (L = -L);
                }
            }
            outL = L;
            outR = R;
        }

        static uint32_t entry(int v, uint32_t id) { return id | static_cast<uint32_t>(v % 24) << 1 | static_cast<uint32_t>(v / 24); }

        // ---- the active voice list ----
        void linkVoice(int v)
        {
            V[v].next = -1;
            V[v].state = 1;
            if (voiceHead < 0)
            {
                voiceHead = v;
                return;
            }
            int p = voiceHead;
            while (V[p].next >= 0)
            {
                tok(LinkStep);
                p = V[p].next;
            }
            V[p].next = v;
        }
        void unlinkVoice(int v)
        {
            V[v].state = 0;
            if (voiceHead == v)
            {
                voiceHead = V[v].next;
                return;
            }
            for (int p = voiceHead; p >= 0; p = V[p].next)
                if (tok(UnlinkStep), V[p].next == v)
                {
                    V[p].next = V[v].next;
                    return;
                }
        }

        void callback(int v, int reason)
        {
            (void)reason;
            Voice &x = V[v];
            const Cb cb = x.cb;
            Handler *h = x.owner;
            if (cb == Cb::None || !h)
                return;
            tok(Callback);
            h->mask[v / 24] &= ~(1u << (v % 24));
            if (cb == Cb::Program && !h->mask[0] && !h->mask[1])
            {
                tok(CallbackFree);
                freeHandler(h);
            }
        }

        // Part A 2.1
        int allocVoice(int32_t group, int32_t prio)
        {
            (void)group;
            const int lo = 0, hi = kVoices;
            for (int v = lo; v < hi; ++v)
                if (tok(AllocScan), V[v].state == 0)
                {
                    V[v].state = 2;
                    V[v].age = now;
                    V[v].prio = static_cast<uint8_t>(prio);
                    V[v].group = static_cast<int8_t>(group);
                    return v;
                }
            int c = lo;
            tok(AllocSteal);
            for (int v = lo + 1; v < hi; ++v)
            {
                tok(AllocScan);
                if (V[v].prio < V[c].prio)
                    c = v;
                else if (V[v].prio == V[c].prio && V[v].age < V[c].age)
                    c = v;
            }
            if (prio < static_cast<int32_t>(V[c].prio) || V[c].prio == 127)
                return -1;
            if (V[c].cb != Cb::None)
                callback(c, 1);
            unlinkVoice(c);
            V[c].state = 2;
            V[c].age = now;
            V[c].prio = static_cast<uint8_t>(prio);
            V[c].group = static_cast<int8_t>(group);
            V[c].cb = Cb::None;
            return c;
        }

        void keyOnVoice(int v)
        {
            Voice &x = V[v];
            tok(KeyOnVoice);
            x.age = now;
            const int core = v / 24;
            const uint32_t bit = 1u << (v % 24);
            const uint32_t mixL = sd().getSwitch(kVmixL | core) | bit, mixR = sd().getSwitch(kVmixR | core) | bit;
            const uint32_t pitch = voicePitch(x.tone, x.pb, x.pm, x.note, x.fine);
            tok(KeyOnPitch);
            sd().begin();
            sd().setParam(entry(v, kVolL), static_cast<uint16_t>(groupScale(x.volL, x.group)) >> 1);
            sd().setParam(entry(v, kVolR), static_cast<uint16_t>(groupScale(x.volR, x.group)) >> 1);
            sd().setSwitch(kVmixL | core, mixL);
            sd().setSwitch(kVmixR | core, mixR);
            sd().setParam(entry(v, kPitch), pitch);
            sd().setAddr(entry(v, kSsa), le32(x.tone + 16));
            sd().setParam(entry(v, kAdsr1), le16(x.tone + 10));
            sd().setParam(entry(v, kAdsr2), le16(x.tone + 12));
            sd().end();
            if (le16(x.tone + 14) & 1)
                reverbMask[core] |= bit;
            else
                reverbMask[core] &= ~bit;
            pendingKon[core] |= bit;
            linkVoice(v);
        }

        void freeVoice(int v)
        {
            tok(FreeVoice);
            if (V[v].prio != 127)
                unlinkVoice(v);
            if (V[v].cb != Cb::None)
                callback(v, 2);
            V[v].cb = Cb::None;
            keyedOff[v / 24] &= ~(1u << (v % 24));
        }
        void endVoice(int v)
        {
            tok(EndVoice);
            if (V[v].state == 1 && V[v].cb != Cb::None)
                callback(v, 3);
            V[v].cb = Cb::None;
            const uint32_t bit = 1u << (v % 24);
            pendingKon[v / 24] &= ~bit;
            pendingKoff[v / 24] |= bit;
        }
        void keyOffMask(const uint32_t m[2])
        {
            const uint32_t mm[2] = {m[0], m[1]};
            for (int v = 0; v < kVoices; ++v)
            {
                const uint32_t bit = 1u << (v % 24);
                if (!(mm[v / 24] & bit) || V[v].state != 1)
                    continue;
                pendingKoff[v / 24] |= bit;
                if (V[v].cb != Cb::None)
                    callback(v, 3);
                V[v].cb = Cb::None;
                pendingKon[v / 24] &= ~bit;
            }
        }
        void silenceMask(const uint32_t m[2])
        {
            const uint32_t mm[2] = {m[0], m[1]};
            for (int v = 0; v < kVoices; ++v)
            {
                const uint32_t bit = 1u << (v % 24);
                if (!(mm[v / 24] & bit))
                    continue;
                if (V[v].state == 1 && V[v].cb != Cb::None)
                    callback(v, 3);
                V[v].cb = Cb::None;
                if (V[v].prio != 127)
                    unlinkVoice(v);
                sd().begin();
                sd().setParam(entry(v, kVolL), 0);
                sd().setParam(entry(v, kVolR), 0);
                sd().end();
                pendingKon[v / 24] &= ~bit;
            }
        }
        template <class F> void forMask(const Handler &h, F f)
        {
            const uint32_t mm[2] = {h.mask[0], h.mask[1]};
            for (int v = 0; v < kVoices; ++v)
                if (mm[v / 24] >> (v % 24) & 1u)
                    f(v);
        }

        // ---- handlers ----
        Handler *allocFrom(Handler *pool, int n, uint32_t prefix)
        {
            for (int i = 0; i < n; ++i)
                if (!pool[i].used)
                {
                    Handler &h = pool[i];
                    h.handle = prefix | static_cast<uint32_t>(i) << 16 | ((h.handle + 1) & 0xFFFFu);
                    h.flags = 0;
                    h.next = nullptr;
                    return &h;
                }
            return nullptr;
        }
        void activate(Handler *h)
        {
            h->handle |= 0x80000000u;
            h->next = nullptr;
            if (!hHead)
                hHead = hTail = h;
            else
            {
                hTail->next = h;
                hTail = h;
            }
        }
        void freeHandler(Handler *h)
        {
            if (!(h->handle & 0x80000000u))
                return;
            tok(FreeHandler);
            h->used = false;
            h->sound = nullptr;
            if (h->type == 4)
                h->bufL = nullptr;
            h->handle &= 0x7FFFFFFFu;
            Handler *prev = nullptr;
            Handler *p = hHead;
            while (p && p != h)
            {
                prev = p;
                p = p->next;
            }
            if (!p)
                return;
            if (prev)
                prev->next = h->next;
            else
                hHead = h->next;
            if (hTail == h)
                hTail = prev;
            h->effect = -1; // the node leaks (Part A 5.2)
        }
        Handler *lookup(uint32_t handle)
        {
            const int type = handle >> 24 & 0x1F;
            const uint32_t slot = handle >> 16 & 0xFF;
            Handler *h = nullptr;
            if (type == 0 && slot < kPrograms)
                h = &H[slot];
            else if (type == 1 && slot < kMidis)
                h = &M[slot];
            else if (type == 4 && slot < S.size())
                h = &S[slot];
            return h && h->handle == handle ? h : nullptr;
        }
        void stopHandler(Handler *h, bool immediate)
        {
            if (h->type == 4)
            {
                stopStream(h->handle);
                return;
            }
            if (immediate || (h->flags & 2))
                silenceMask(h->mask);
            else
                keyOffMask(h->mask);
            freeHandler(h);
        }

        // ---- programs (Part A 6) ----
        int selectTones(Bank *b, int32_t p, int32_t note, const uint8_t *out[16])
        {
            if (p >= b->programCount)
                return 0;
            const uint8_t *prog = b->program(p);
            const uint8_t *t = &b->h[le32(prog + 4)];
            int n = 0;
            for (int i = 0; i < s8(prog[0]); ++i)
                if (tok(SelectTone), s8(t[24 * i + 6]) <= note && (tok(SelectLow), true) && note <= s8(t[24 * i + 7]) && n < 16)
                    out[n++] = t + 24 * i;
            return n;
        }
        uint32_t playProgram(Bank *b, const uint8_t *snd, int32_t vol, int32_t pan, int32_t pm, int32_t pb)
        {
            if (le32(snd) == 2)
            {
                for (Handler *h = hHead; h;)
                {
                    Handler *nx = h->next;
                    if (h->sound == snd)
                        stopHandler(h, false);
                    h = nx;
                }
            }
            if (vol == 0x7FFFFFFF)
                vol = 1024;
            const int32_t sv = le16s(snd + 0x10);
            int32_t hv = static_cast<int32_t>((static_cast<int64_t>(vol) * sv) >> 10);
            if (hv >= 128)
                hv = 127;
            if (pan == -1 || pan == -2)
                pan = le16s(snd + 0x14);
            const int32_t group = s8(snd[0x13]);
            const uint8_t *list[16];
            const int32_t progIndex = s8(snd[0x0d]);
            const int n = selectTones(b, progIndex, s8(snd[0x0e]), list);
            const uint8_t *prog = progIndex < b->programCount ? b->program(progIndex) : nullptr;
            Handler *h = nullptr;
            bool first = true;
            for (int i = 0; i < n; ++i)
            {
                const uint8_t *tone = list[i];
                const int v = allocVoice(group, s8(tone[0]));
                if (v < 0)
                    continue;
                if (first)
                {
                    h = allocFrom(H.data(), kPrograms, 0);
                    if (h)
                    {
                        h->used = true;
                        h->sound = snd;
                        h->sndVol = sv;
                        h->sndPan = le16s(snd + 0x14);
                        h->vol = hv;
                        h->pan = pan;
                        h->pm = pm;
                        h->note = s8(snd[0x0e]);
                        h->fine = s8(snd[0x0f]);
                        h->pb = pb;
                        h->group = group;
                        h->flags = 0;
                        h->effect = -1;
                        h->bank = b;
                    }
                    first = false;
                }
                if (h)
                    h->mask[v / 24] |= 1u << (v % 24);
                Voice &x = V[v];
                x.owner = h;
                x.cb = h ? Cb::Program : Cb::None;
                x.tone = tone;
                x.note = s8(snd[0x0e]);
                x.fine = s8(snd[0x0f]);
                x.pb = pb;
                x.pm = pm;
                x.prio = tone[0];
                x.group = group;
                x.volL = x.volR = 0;
                voiceVolPan(127, hv, pan, s8(prog[1]), le16s(prog + 2), s8(tone[1]), le16s(tone + 4), x.volL, x.volR);
                keyOnVoice(v);
            }
            if (!h)
                return 0;
            activate(h);
            return h->handle;
        }
        void repitch(Handler *h)
        {
            forMask(*h, [&](int v) {
                V[v].pb = h->pb;
                V[v].pm = h->pm;
                sd().setParam(entry(v, kPitch), voicePitch(V[v].tone, h->pb, h->pm, h->note, h->fine));
            });
        }
        void setVolPan(uint32_t handle, int32_t vol, int32_t pan)
        {
            Handler *h = lookup(handle);
            if (!h)
                return;
            if (h->type == 4)
            {
                streamVolPan(h, vol, pan);
                return;
            }
            if (vol < 0)
                h->vol = -vol;
            else if (vol != 0x7FFFFFFF)
                h->vol = static_cast<int16_t>((static_cast<int64_t>(vol) * le16s(h->sound + 0x10)) >> 10);
            if (h->vol >= 128)
                h->vol = 127;
            if (pan == -1)
                h->pan = le16s(h->sound + 0x14);
            else if (pan != -2)
                h->pan = pan;
            if (h->type == 0)
            {
                Bank *b = h->bank;
                const uint8_t *prog = b->program(s8(h->sound[0x0d]));
                const int32_t group = s8(h->sound[0x13]);
                forMask(*h, [&](int v) {
                    Voice &x = V[v];
                    voiceVolPan(127, h->vol, h->pan, s8(prog[1]), le16s(prog + 2), s8(x.tone[1]), le16s(x.tone + 4), x.volL, x.volR);
                    sd().setParam(entry(v, kVolL), static_cast<uint16_t>(groupScale(x.volL, group)) >> 1);
                    sd().setParam(entry(v, kVolR), static_cast<uint16_t>(groupScale(x.volR, group)) >> 1);
                });
            }
            else if (h->type == 1)
            {
                for (int v = voiceHead; v >= 0; v = V[v].next)
                {
                    Voice &x = V[v];
                    if (x.owner != h)
                        continue;
                    int32_t p = h->chPan[x.ch] + h->pan;
                    if (p >= 360)
                        p -= 360;
                    const int32_t vv = tdiv(static_cast<int64_t>(x.vel) * s8(static_cast<uint8_t>(h->chVol[x.ch])), 127);
                    voiceVolPan(h->vol, vv, p, s8(x.prog[1]), le16s(x.prog + 2), s8(x.tone[1]), le16s(x.tone + 4), x.volL, x.volR);
                    sd().setParam(entry(v, kVolL), static_cast<uint16_t>(groupScale(x.volL, x.group)) >> 1);
                    sd().setParam(entry(v, kVolR), static_cast<uint16_t>(groupScale(x.volR, x.group)) >> 1);
                }
            }
        }
        void setPitchModifier(uint32_t handle, int32_t pm)
        {
            Handler *h = lookup(handle);
            if (!h)
                return;
            if (h->type == 0)
            {
                h->pm = pm;
                repitch(h);
            }
            else if (h->type == 1)
            {
                h->pm = pm;
                for (int v = voiceHead; v >= 0; v = V[v].next)
                    if (V[v].owner == h)
                    {
                        V[v].pm = pm;
                        sd().setParam(entry(v, kPitch), voicePitch(V[v].tone, V[v].pb, pm, V[v].note, V[v].fine));
                    }
            }
        }
        void setPitchBend(uint32_t handle, int32_t pb)
        {
            Handler *h = lookup(handle);
            if (h && h->type == 0)
            {
                h->pb = pb;
                repitch(h);
            }
        }

        // ---- auto-pan (Part A 8) ----
        int findEffect(Handler *h) { return h->effect; }
        void autoPan(uint32_t handle, int32_t target)
        {
            Handler *h = lookup(handle);
            if (!h)
                return;
            int32_t dir = 0, time = 30;
            int32_t d = target - h->pan;
            const bool autoDir = dir == 0;
            if (autoDir)
                dir = 1;
            if (dir > 0 && d < 0)
                d += 360;
            if (dir < 0 && d > 0)
                d -= 360;
            if (autoDir && std::abs(d) >= 181)
            {
                dir = -dir;
                d -= 360;
            }
            time = tdiv(static_cast<int64_t>(time) * std::abs(d), 180);
            if (time == 0)
            {
                removeEffect(h);
                setVolPan(handle, 0x7FFFFFFF, target);
                return;
            }
            if (d == 0)
            {
                removeEffect(h);
                return;
            }
            int e = findEffect(h);
            bool fresh = false;
            if (e < 0)
            {
                for (int i = 0; i < kEffectNodes; ++i)
                    if (!(nodesFx[i].flags & 1))
                    {
                        e = i;
                        break;
                    }
                if (e < 0)
                {
                    setVolPan(handle, 0x7FFFFFFF, target);
                    return;
                }
                nodesFx[e] = EffectNode{};
                nodesFx[e].flags = 1 | 4;
                fresh = true;
            }
            EffectNode &n = nodesFx[e];
            if (std::abs(d) < time)
            {
                n.mode = 1;
                n.period = n.count = std::abs(tdiv(time, d));
                n.step = d >= 0 ? 1 : -1;
            }
            else
            {
                n.mode = 0;
                n.count = 0;
                n.step = tdiv(d, time);
            }
            n.target = target;
            if (fresh)
                h->effect = e;
        }
        void removeEffect(Handler *h)
        {
            if (h->effect >= 0)
            {
                nodesFx[h->effect].flags &= ~1u;
                h->effect = -1;
            }
        }
        void runEffects(Handler *h)
        {
            const int e = h->effect;
            EffectNode &n = nodesFx[e];
            if (n.mode == 1)
            {
                if (--n.count != 0)
                    return;
            }
            const int32_t step = n.step, p = h->pan;
            int32_t np;
            if (std::abs(step) >= 2)
                np = static_cast<int16_t>(static_cast<uint32_t>(p * 32 + step) >> 5);
            else
                np = static_cast<int16_t>(p + step);
            int32_t cmp = n.target;
            if (np >= 360 && n.target < p)
                cmp = n.target + 360;
            else if (np < 0 && p < n.target)
                cmp = n.target - 360;
            if ((step > 0 && cmp < np && p < cmp) || (step < 0 && np < cmp && cmp < p))
                np = n.target;
            else if (np < 0)
                np += 360;
            else if (np >= 360)
                np -= 360;
            setVolPan(h->handle, 0x7FFFFFFF, np);
            if (np == n.target)
                removeEffect(h);
            else if (n.mode == 1)
                n.count = n.period;
        }

        // ---- MIDI (Part B) ----
        static uint32_t varlen(const uint8_t *p, int &len)
        {
            uint32_t v = 0;
            len = 0;
            for (;;)
            {
                const uint8_t c = p[len++];
                v = v << 7 | (c & 0x7Fu);
                if (!(c & 0x80) || len >= 5)
                    break;
            }
            return v;
        }
        static constexpr uint32_t kRate = 4170;
        static int32_t stepFor(uint32_t T, uint32_t P)
        {
            const uint32_t q = P / 10;
            const uint32_t d = q ? T / q : 1;
            return static_cast<int32_t>((kRate * 1000u) / (d ? d : 1));
        }
        static uint32_t ticksFor(uint32_t T, uint32_t P, int32_t n)
        {
            const uint32_t u = static_cast<uint32_t>(static_cast<int64_t>(T) * n) / (P ? P : 1);
            return (u + kRate - 1) / kRate;
        }
        Midi *midiBlockOf(const uint8_t *snd)
        {
            const uint32_t id = le32(snd + 0x0c);
            for (Block &b : blocks)
                if (b.id == id)
                {
                    if (!b.multi)
                        return b.mid;
                    const int32_t idx = s8(snd[0x16]);
                    if (static_cast<int32_t>(b.subs.size()) - 1 < idx || idx < 0)
                        return nullptr;
                    return b.subs[static_cast<size_t>(idx)];
                }
            return nullptr;
        }
        uint32_t playMidi(const uint8_t *snd, int32_t vol, int32_t pan, int32_t pm)
        {
            Midi *b = midiBlockOf(snd);
            if (!b || !b->bank)
                return 0;
            Handler *h = allocFrom(M.data(), kMidis, 0x01000000u);
            if (!h)
                return 0xFFFFFFFFu;
            if (vol == 0x7FFFFFFF)
                vol = 1024;
            const int32_t sv = le16s(snd + 0x10);
            h->vol = static_cast<int16_t>((static_cast<int64_t>(vol) * sv) >> 10);
            if (h->vol >= 128)
                h->vol = 127;
            h->pan = (pan == -1 || pan == -2) ? le16s(snd + 0x14) : pan;
            h->pm = static_cast<int16_t>(pm);
            h->loops = snd[0x12];
            h->used = true;
            h->sound = snd;
            h->sndVol = sv;
            h->effect = -1;
            h->flags = 0;
            h->sndPan = le16s(snd + 0x14);
            h->group = s8(snd[0x13]);
            h->bank = b->bank;
            h->loopStart = h->ptr = b->data;
            h->rem = 0;
            h->tempo = b->tempo;
            h->ppq = b->ppq;
            h->mute = 0;
            for (int c = 0; c < 16; ++c)
            {
                h->prog[c] = 0;
                h->chVol[c] = 127;
                h->chPan[c] = 0;
                h->chBend[c] = 0;
            }
            h->sustain = 0;
            h->first = 1;
            activate(h);
            return h->handle;
        }
        void noteOn(Handler *h, int ch, int32_t note, int32_t vel)
        {
            const int32_t prog = s8(static_cast<uint8_t>(h->prog[ch]));
            const uint8_t *list[16];
            tok(NoteOnBase);
            const int n = selectTones(h->bank, prog, note, list);
            if (!n)
                return;
            const uint8_t *P = h->bank->program(prog);
            const int32_t group = s8(h->sound[0x13]);
            for (int i = 0; i < n; ++i)
            {
                const uint8_t *tone = list[i];
                const int v = allocVoice(group, s8(tone[0]));
                if (v < 0)
                    continue;
                tok(NoteOnVoice);
                h->mask[v / 24] |= 1u << (v % 24);
                Voice &x = V[v];
                x.owner = h;
                x.cb = Cb::Midi;
                x.note = note;
                x.fine = 0;
                x.tone = tone;
                x.prio = tone[0];
                x.group = group;
                x.volL = x.volR = 0;
                int32_t pan = h->chPan[ch] + h->pan;
                if (pan >= 360)
                    pan -= 360;
                const int32_t vv = tdiv(static_cast<int64_t>(s8(static_cast<uint8_t>(vel))) * s8(static_cast<uint8_t>(h->chVol[ch])), 127);
                voiceVolPan(h->vol, vv, pan, s8(P[1]), le16s(P + 2), s8(tone[1]), le16s(tone + 4), x.volL, x.volR);
                x.ch = ch;
                x.pb = h->chBend[ch];
                x.vel = vel;
                x.pm = h->pm;
                x.sustained = 0;
                x.prog = P;
                keyOnVoice(v);
            }
        }
        void noteOff(Handler *h, int ch, int32_t note)
        {
            for (int v = voiceHead; v >= 0;)
            {
                const int nx = V[v].next;
                Voice &x = V[v];
                tok(NoteOffStep);
                if (x.owner == h)
                    tok(NoteOffOwner);
                if (x.owner == h && x.ch == ch)
                    tok(NoteOffCh);
                if (x.owner == h && x.ch == ch && static_cast<int8_t>(x.note) == static_cast<int8_t>(note))
                {
                    tok(NoteOffMatch);
                    if (h->sustain >> ch & 1u)
                        x.sustained = 1;
                    else
                        endVoice(v);
                }
                v = nx;
            }
        }
        bool controller(Handler *h, int ch, int c, int v)
        {
            switch (c)
            {
            case 0:
            {
                Bank *found = nullptr;
                for (auto &b : banks)
                    if (b->number == s8(static_cast<uint8_t>(v)))
                    {
                        found = b.get();
                        break;
                    }
                if (!found)
                    return true;
                h->bank = found;
                return false;
            }
            case 7: h->chVol[ch] = v; return false;
            case 10:
            {
                const int32_t p = s8(static_cast<uint8_t>(v));
                h->chPan[ch] = static_cast<int16_t>(p < 64 ? tdiv(90 * p, 64) + 270 : tdiv(90 * (p - 64), 63));
                return false;
            }
            case 64:
                if (v >= 64)
                    h->sustain |= 1u << ch;
                else
                {
                    h->sustain &= ~(1u << ch);
                    for (int x = voiceHead; x >= 0;)
                    {
                        const int nx = V[x].next;
                        if (V[x].owner == h && V[x].ch == ch && V[x].sustained == 1)
                            endVoice(x);
                        x = nx;
                    }
                }
                return false;
            default: return false;
            }
        }
        void pitchBend(Handler *h, int ch, int lsb, int msb)
        {
            const uint32_t raw = (lsb & 0x7Fu) | (msb & 0x7Fu) << 7;
            const int32_t pb = static_cast<int16_t>(static_cast<int32_t>(raw * 65535u / 16383u) - 32768);
            h->chBend[ch] = pb;
            for (int v = voiceHead; v >= 0; v = V[v].next)
                if (tok(BendStep), V[v].owner == h && (tok(BendOwner), V[v].ch == ch))
                {
                    tok(BendMatch);
                    V[v].pb = pb;
                    sd().setParam(entry(v, kPitch), voicePitch(V[v].tone, pb, V[v].pm, V[v].note, V[v].fine));
                }
        }
        int events = 0; // MIDI events this tick (the cost model)
        bool seqTick(Handler *h)
        {
            bool stop = false;
            if (h->first & 1)
            {
                tok(SeqFirst);
                h->step = stepFor(h->tempo, h->ppq);
                h->runStatus = 0;
                int len;
                const uint32_t d = varlen(h->ptr, len);
                h->x = static_cast<int32_t>(100u * d);
                h->count = ticksFor(h->tempo, h->ppq, h->x / 100);
                h->rem = 0;
                h->first &= ~1u;
                h->ptr += len;
            }
            else
            {
                tok(SeqDec);
                --h->count;
            }
            if (h->count != 0)
                return false;
            for (int guard = 0; guard < 100000; ++guard)
            {
                ++events;
                tok(SeqEvent);
                const uint8_t *p = h->ptr;
                if (p[0] & 0x80)
                {
                    h->runStatus = p[0];
                    h->ptr += 1;
                    p = h->ptr;
                }
                const uint8_t s = h->runStatus;
                const int ch = s & 0x0F;
                if (s == 0xFF)
                {
                    if (p[0] == 0x2F)
                    {
                        tok(EvEnd);
                        h->loops = static_cast<uint8_t>(h->loops - 1);
                        if (h->loops == 0)
                            stop = true;
                        else
                        {
                            h->ptr = h->loopStart;
                            if (static_cast<int8_t>(h->loops) < 0)
                                h->loops = 0;
                        }
                    }
                    else if (p[0] == 0x51)
                    {
                        tok(EvTempo);
                        h->tempo = uint32_t(p[2]) << 16 | uint32_t(p[3]) << 8 | p[4];
                        h->ptr += 5;
                        h->step = stepFor(h->tempo, h->ppq);
                    }
                }
                else if (s == 0xF0)
                {
                    const uint8_t *q = p;
                    while (*q != 0xF7)
                        ++q;
                    h->ptr = q + 1;
                }
                else
                {
                    switch (s & 0xF0)
                    {
                    case 0x80: tok(EvNoteOff); noteOff(h, ch, s8(p[0])); h->ptr += 2; break;
                    case 0x90:
                        tok(EvNoteOn);
                        if (p[1] == 0)
                            noteOff(h, ch, s8(p[0]));
                        else if (!(h->mute >> ch & 1u))
                            noteOn(h, ch, s8(p[0]), p[1]);
                        h->ptr += 2;
                        break;
                    case 0xB0: tok(EvCtl); stop = controller(h, ch, p[0], p[1]) || stop; h->ptr += 2; break;
                    case 0xC0: tok(EvProgram); h->prog[ch] = p[0]; h->ptr += 1; break;
                    case 0xD0: noteOff(h, ch, s8(p[0])); h->ptr += 1; break;
                    case 0xE0: tok(EvBend); pitchBend(h, ch, p[0], p[1]); h->ptr += 2; break;
                    default: break;
                    }
                }
                if (stop)
                    break;
                int len;
                tok(SeqDelta);
                const uint32_t d = varlen(h->ptr, len);
                int32_t x = static_cast<int32_t>(100u * d) + h->rem;
                h->x = x;
                if (x < 0 || x < h->step / 2)
                {
                    h->rem = x;
                    h->x = 0;
                }
                if (h->x != 0)
                {
                    tok(SeqDeltaTicks);
                    h->count = ticksFor(h->tempo, h->ppq, h->x / 100);
                    h->rem = h->x - h->step * static_cast<int32_t>(h->count);
                }
                h->ptr += len;
                if (h->count != 0)
                    break;
            }
            return stop;
        }

        // ---- streams (Part C) ----
        static uint32_t markBlocks(uint8_t *iop, uint32_t bytes, uint32_t last)
        {
            if (bytes == 16)
            {
                std::memcpy(iop + 16, iop, 16);
                std::memset(iop, 0, 16);
                iop[17] = 7;
                iop[1] = 1;
                return 32;
            }
            if (bytes == 32)
            {
                iop[1] = 1;
                return 32;
            }
            if (!last)
            {
                iop[1] = 6;
                iop[17] = 2;
                iop[bytes - 16 + 1] = 3;
            }
            else
            {
                iop[1] = 4;
                iop[bytes - 16 + 1] = 7;
                iop[bytes - 32 + 1] = 1;
            }
            return bytes;
        }
        void initStreaming(uint32_t n, uint32_t bufBytes)
        {
            if (streaming)
                return;
            streaming = true;
            const uint32_t size = bufBytes / 4096 * 4096;
            S.assign(n, Handler{});
            for (uint32_t i = 0; i < n; ++i)
            {
                S[i].handle = i << 16 | 0x04000001u;
                S[i].type = 4;
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                auto b = std::make_unique<Buffer>();
                b->flags = 1;
                b->size = size;
                b->spuA = spuAlloc(size);
                b->req[0].spu = b->spuA;
                b->req[1].spu = b->spuA + size / 2;
                b->req[0].iop.assign(size / 2, 0);
                b->req[1].iop.assign(size / 2, 0);
                buffers.push_back(std::move(b));
            }
        }
        Buffer *freeBuffer(Buffer *after = nullptr)
        {
            for (auto &b : buffers)
                if ((b->flags & 1) && b.get() != after)
                    return b.get();
            return nullptr;
        }
        void readChunk(Buffer *b, Request &r)
        {
            // startRead for data (Part C 3.5): the next bytes of the file into the request's IOP half.
            const uint32_t half = b->size / 2;
            uint32_t bytes;
            if (b->toRead > half)
            {
                r.last = 0;
                bytes = half;
            }
            else
            {
                b->flags |= 0x20;
                r.last = 1;
                bytes = b->toRead;
            }
            r.bytes = bytes;
            b->toRead -= bytes;
            const uint32_t sectors = (bytes + 2047) >> 11;
            std::vector<uint8_t> data(static_cast<size_t>(sectors) * 2048);
            if (e.m_disc)
                e.m_disc(b->nextLbn, sectors, data.data());
            std::memcpy(r.iop.data(), data.data(), std::min<size_t>(data.size(), r.iop.size()));
            b->nextLbn += sectors;
            if ((r.flags & 0xC) != 8)
                r.bytes = markBlocks(r.iop.data(), r.bytes, r.last);
            r.flags |= 0x80;
        }
        void transfer(Buffer *b, Request &r)
        {
            e.m_ram.resize(std::max<size_t>(e.m_ram.size(), r.iop.size()));
            std::memcpy(e.m_ram.data(), r.iop.data(), r.iop.size());
            sd().voiceTrans(0, 0, 0, r.spu, b->size / 2);
        }
        // The disc pipeline's start (Part C 3.5), done at once (the reference's reads complete at once): the data
        // in sound memory and the IOP halves as the threads leave them; keyOnStream at the pipeline's end.
        void startPipeline(Buffer *L, uint64_t at)
        {
            std::vector<uint8_t> head(4096);
            const uint32_t sectors = L->off <= 2000 ? 1 : 2;
            if (e.m_disc)
                e.m_disc(L->nextLbn, sectors, head.data());
            const uint8_t *hd = head.data() + L->off;
            const uint32_t size = be32(hd + 12), rate = be32(hd + 16);
            L->rate = rate;
            L->pitch = static_cast<uint32_t>((static_cast<uint64_t>(rate) * 4096u) / 48000u);
            L->total = (size >> 4) * 28;
            L->played = 0;
            L->req[0].flags = 9;
            L->req[1].flags = 1;
            L->toRead = size + L->off + 48;
            int kind;
            if (L->toRead < L->size / 2 + 1)
            {
                L->req[0].flags |= 4;
                kind = 0;
            }
            else if (L->toRead < L->size + 1)
            {
                L->req[1].flags = 5;
                kind = 1;
            }
            else
            {
                L->req[0].flags |= 0x20;
                kind = 2;
            }
            Buffer *R = L->R;
            if (R)
            {
                R->toRead = size + R->off + 48;
                R->pitch = L->pitch;
                R->req[0].flags = 9;
                R->req[1].flags = 1;
                R->played = 0;
            }
            // The reads in the reference's order (one disc operation at a time, reads complete at once): L.A, then the
            // ones queued with the header (R.A, and R.B and L.B when the file is longer than half a buffer), each
            // initial chunk transferred when read; then each transfer's completion (xferDone, 2,000 cycles later, so
            // after all of those reads) in the same order, which may re-read the next chunk into its IOP half.
            std::vector<std::pair<Buffer *, int>> order{{L, 0}};
            if (R)
            {
                order.push_back({R, 0});
                if (kind != 0)
                    order.push_back({R, 1});
            }
            if (kind != 0)
                order.push_back({L, 1});
            for (auto &[b, i] : order)
            {
                readChunk(b, b->req[i]);
                transfer(b, b->req[i]);
            }
            for (auto &[b, i] : order)
            {
                Request &r = b->req[i];
                const uint32_t fl = r.flags, last = r.last;
                r.flags = 0;
                if (fl & 4)
                    keyOnPending = true;
                if ((b->flags & 0x1A) && !(b->flags & 0x20))
                {
                    if (fl & 0x20)
                        r.flags = 0x40;
                    readChunk(b, r);
                    if (r.flags & 0x40)
                        keyOnPending = true;
                }
                if (last)
                    b->flags |= 0x40;
            }
            // keyOnStream when the threads get there (the replay: about 1,280 IOP cycles after the command for a
            // two-chunk mono file, 2,470 for a long stereo one).
            if (keyOnPending)
                keyOns.push_back({at + (kind == 2 ? 2470u : 1280u), L});
            keyOnPending = false;
        }
        void keyOnStream(Buffer *b)
        {
            if (b->voice < 0)
            {
                b->flags |= 0x80;
                return;
            }
            const bool paused = b->flags & 0x10;
            Buffer *R = b->R;
            auto one = [&](Buffer *c, int32_t progPan) {
                const int v = c->voice;
                Voice &x = V[v];
                x.volL = x.volR = 0;
                x.group = c->group;
                voiceVolPan(127, c->vol, c->pan, 127, progPan, 127, 0, x.volL, x.volR);
                const int core = v / 24;
                const uint32_t bit = 1u << (v % 24);
                sd().setParam(entry(v, kVolL), paused ? 0 : static_cast<uint16_t>(groupScale(x.volL, x.group)) >> 1);
                sd().setParam(entry(v, kVolR), paused ? 0 : static_cast<uint16_t>(groupScale(x.volR, x.group)) >> 1);
                sd().setSwitch(kVmixL | core, sd().getSwitch(kVmixL | core) | bit);
                sd().setSwitch(kVmixR | core, sd().getSwitch(kVmixR | core) | bit);
                sd().setParam(entry(v, kPitch), paused ? 0 : c->pitch);
                c->last124 = c->spuA + c->off + 48;
                sd().setAddr(entry(v, kSsa), c->last124);
                sd().setParam(entry(v, kAdsr1), 0x000F);
                sd().setParam(entry(v, kAdsr2), 0x1FC0);
                c->flags |= 4;
                pendingKon[core] |= bit;
                if (c->flags & 0x100)
                    reverbMask[core] |= bit;
                else
                    reverbMask[core] &= ~bit;
            };
            one(b, R ? 270 : 0);
            if (R)
                one(R, 90);
        }
        uint32_t playStream(const uint32_t a[7])
        {
            const uint32_t lbnL = a[0], lbnR = a[1], offL = a[2] & 0xFFFF, offR = a[3] & 0xFFFF;
            const int32_t vol = static_cast<int32_t>(a[2] >> 16), pan = static_cast<int32_t>(a[3] >> 16);
            const int32_t group = static_cast<int32_t>(a[4]);
            const uint32_t flags = a[6];
            if (!streaming)
                return 0;
            const bool stereo = lbnR != 0;
            Buffer *L = freeBuffer();
            if (!L)
                return 0;
            L->flags = 2;
            L->group = group;
            L->R = nullptr;
            Buffer *R = nullptr;
            if (stereo)
            {
                R = freeBuffer(L);
                if (!R)
                {
                    L->flags = 1;
                    return 0;
                }
                R->flags = 2;
                R->group = group;
                L->R = R;
            }
            const int v = allocVoice(group, 127);
            L->voice = v;
            if (v < 0)
            {
                L->flags = 1;
                if (R)
                    R->flags = 1;
                return 0;
            }
            V[v].state = 4;
            if (R)
            {
                const int w = allocVoice(group, 127);
                R->voice = w;
                if (w < 0)
                {
                    L->flags = R->flags = 1;
                    unlinkVoice(v);
                    return 0;
                }
                V[w].state = 4;
            }
            Handler *s = nullptr;
            for (size_t i = 0; i < S.size(); ++i)
                if (!S[i].used)
                {
                    s = &S[i];
                    s->flags = 0;
                    s->handle = 0x04000000u | static_cast<uint32_t>(i) << 16 | ((s->handle + 1) & 0xFFFFu);
                    s->next = nullptr;
                    break;
                }
            if (!s)
                return 0;
            s->vol = s->sndVol = static_cast<int32_t>((static_cast<int64_t>(vol) * 127) >> 10);
            s->used = true;
            s->bufL = L;
            s->pan = s->sndPan = pan;
            s->group = group;
            s->effect = -1;
            s->mask[0] = s->mask[1] = 0;
            s->mask[v / 24] |= 1u << (v % 24);
            if (R)
                s->mask[R->voice / 24] |= 1u << (R->voice % 24);
            auto setup = [&](Buffer *b, uint32_t lbn, uint32_t off) {
                b->toRead = 2048;
                b->nextLbn = b->firstLbn = lbn;
                b->off = off;
                b->req[0].flags = 0x10;
                b->req[1].flags = 0;
                b->req[0].last = b->req[1].last = 0;
                b->half64 = b->lastNax = b->swapTick = b->err = 0;
                b->vol = s->vol;
                b->pan = s->pan;
                b->pitch = 0;
                b->stream = s;
                b->rate = 0xFFFFFFFFu;
                b->total = 0;
                b->played = 0;
                if (flags & 1)
                    b->flags |= 0x10;
                if (flags & 2)
                    b->flags |= 0x100;
            };
            setup(L, lbnL, offL);
            if (R)
                setup(R, lbnR, offR);
            activate(s);
            pendingStarts.push_back(L);
            return s->handle;
        }
        std::vector<Buffer *> pendingStarts;

        void stopStream(uint32_t handle)
        {
            Handler *s = lookup(handle);
            if (!s)
                return;
            Buffer *L = s->bufL;
            for (Buffer *b : {L, L ? L->R : nullptr})
            {
                if (!b)
                    continue;
                b->flags = 1;
                if (b->voice >= 0)
                {
                    linkVoice(b->voice);
                    V[b->voice].prio = 0;
                    endVoice(b->voice);
                }
            }
            std::erase_if(keyOns, [&](const Pending &p) { return p.b == L; });
            freeHandler(s);
        }
        void pauseStream(uint32_t handle)
        {
            Handler *s = lookup(handle);
            if (!s)
                return;
            Buffer *L = s->bufL;
            if (!L || (L->flags & 0x10) || s->flags != 0)
                return;
            s->flags |= 2;
            for (Buffer *b : {L, L->R})
            {
                if (!b)
                    continue;
                b->flags |= 0x10;
                sd().setParam(entry(b->voice, kVolL), 0);
                sd().setParam(entry(b->voice, kVolR), 0);
                sd().setParam(entry(b->voice, kPitch), 0);
            }
        }
        void streamVolPan(Handler *s, int32_t vol, int32_t pan)
        {
            if (vol < 0)
                s->vol = -vol;
            else if (vol != 0x7FFFFFFF)
                s->vol = static_cast<int32_t>((static_cast<int64_t>(vol) * 127) >> 10);
            if (s->vol >= 128)
                s->vol = 127;
            if (pan == -1)
                s->pan = 0;
            else if (pan != -2)
                s->pan = pan;
            Buffer *b = s->bufL;
            if (!b)
                return;
            b->vol = s->vol;
            b->pan = s->pan;
            auto one = [&](Buffer *c, int32_t progPan) {
                if (c->voice < 0 || c->voice >= kVoices)
                    return;
                Voice &x = V[c->voice];
                voiceVolPan(127, c->vol, c->pan, 127, progPan, 127, 0, x.volL, x.volR);
                sd().setParam(entry(c->voice, kVolL), static_cast<uint16_t>(groupScale(x.volL, x.group)) >> 1);
                sd().setParam(entry(c->voice, kVolR), static_cast<uint16_t>(groupScale(x.volR, x.group)) >> 1);
            };
            one(b, b->R ? 270 : 0);
            if (b->R)
            {
                b->R->vol = s->vol;
                b->R->pan = s->pan;
                one(b->R, 90);
            }
        }
        void advancePlayed(Buffer *b, uint32_t pos)
        {
            const uint32_t end = b->spuA + b->size;
            if (pos > end)
                pos = end;
            if (pos >= b->last124)
                b->played += pos - b->last124;
            else
                b->played += (end - b->last124) + (pos - b->spuA);
            b->last124 = pos;
        }
        int bufTick(Buffer *b, bool &sig)
        {
            int ret = 0;
            if ((b->flags & 0x18) == 8)
            {
                const int v = b->voice;
                uint32_t pos = sd().getAddr(entry(v, kNax));
                const uint32_t spuB = b->spuA + b->size / 2;
                uint32_t half;
                if (b->spuA <= pos && pos < spuB)
                    half = 0;
                else if (spuB <= pos && pos < spuB + b->size / 2)
                    half = 1;
                else
                {
                    ++b->err;
                    if (b->played != 0 && b->lastNax != 0)
                        return -1;
                    pos = b->spuA;
                    half = 0;
                }
                if (b->flags & 0x40)
                {
                    const uint32_t env = sd().getParam(entry(v, kEnvx));
                    pos = sd().getAddr(entry(v, kNax));
                    if ((env & 0xFFFF) == 0)
                        ret = -1;
                }
                else
                {
                    if (half != b->half64)
                    {
                        if (b->swapTick != 0 && now <= b->swapTick + 120)
                            ++b->err;
                        else
                        {
                            Request &r = b->req[b->half64 ? 1 : 0];
                            if (r.flags & 0x80)
                            {
                                refills.push_back({b, b->half64 ? 1 : 0});
                                r.flags &= ~0x80u;
                                sig = true;
                                b->half64 = half;
                                b->swapTick = now;
                            }
                            else
                                ret = -2;
                        }
                    }
                    else if (b->lastNax != 0 && pos < b->lastNax)
                        ret = -2;
                    b->lastNax = pos;
                }
                advancePlayed(b, pos);
            }
            if ((b->flags & 6) == 6)
                b->flags = (b->flags & ~2u) | 8u;
            return ret;
        }
        struct Refill
        {
            Buffer *b;
            int i;
        };
        std::vector<Refill> refills;
        void doRefills()
        {
            // The SPU transfer and completion threads after the tick (Part C 3.5 xferDone): the half the voice left
            // gets its chunk, LSAX loops back to it, the next chunk is read into its IOP half.
            for (const Refill &f : refills)
            {
                Buffer *b = f.b;
                if (b->flags & 1)
                    continue;
                Request &r = b->req[f.i];
                transfer(b, r);
                const uint32_t last = r.last;
                r.flags = 0;
                sd().setAddr(entry(b->voice, kLsax), r.spu);
                if ((b->flags & 0x1A) && !(b->flags & 0x20))
                    readChunk(b, r);
                if (last)
                    b->flags |= 0x40;
            }
            refills.clear();
        }
        void streamTick(Handler *s)
        {
            Buffer *L = s->bufL;
            if (!L)
                return;
            Buffer *R = L->R;
            bool sig = false;
            int r = bufTick(L, sig);
            if (R && r == 0)
                r = bufTick(R, sig);
            if (r == 0)
            {
                if (L->err >= 5 || (R && R->err >= 5))
                    r = -2;
            }
            if (r == -1)
            {
                L->flags = 1;
                if (R)
                    R->flags = 1;
                for (Buffer *b : {L, R})
                {
                    if (!b || b->voice < 0)
                        continue;
                    linkVoice(b->voice);
                    V[b->voice].prio = 0;
                    endVoice(b->voice);
                }
                freeHandler(s);
            }
            else if (r == -2)
                stopStream(s->handle);
        }

        // ---- the tick (Part A 4) ----
        uint64_t tick(uint64_t service)
        {
            ++now;
            e.m_now = service;
            tok(TickBase);
            for (int v = voiceHead; v >= 0;)
            {
                Voice &x = V[v];
                const uint32_t bit = 1u << (v % 24);
                if (pendingKon[v / 24] & bit)
                {
                    tok(VoicePending);
                    v = x.next;
                    continue;
                }
                const uint32_t env = sd().getParam(entry(v, kEnvx));
                if ((env & 0xFFFF) != 0)
                    tok(VoiceRead);
                else if (keyedOff[v / 24] & bit)
                {
                    tok(VoiceZeroOff);
                    freeVoice(v);
                }
                else if (keyedOn[v / 24] & bit)
                {
                    silent[v] = static_cast<uint8_t>(silent[v] - 1);
                    if (silent[v] == 0)
                    {
                        tok(VoiceZeroEnd);
                        endVoice(v);
                    }
                    else
                        tok(VoiceZeroOn);
                }
                else
                    tok(VoiceZeroNone);
                v = x.next;
            }
            // handlerTick
            events = 0;
            for (Handler *h = hHead; h;)
            {
                Handler *nx = h->next;
                bool stop = false;
                if (h->type == 1)
                {
                    tok(HandlerMidi);
                    if (!(h->flags & 2))
                        stop = seqTick(h);
                }
                else if (h->type == 4)
                {
                    tok(HandlerStream);
                    streamTick(h);
                }
                else
                    tok(HandlerProgram);
                if (!stop && h->effect >= 0 && !(h->flags & 2) && (h->handle & 0x80000000u))
                {
                    tok(HandlerEffect);
                    runEffects(h);
                }
                if (stop)
                    stopHandler(h, false);
                h = nx;
            }
            if (pendingKoff[0] | pendingKoff[1])
            {
                tok(FlushKoff);
                sd().setSwitch(kKoff | 0, pendingKoff[0]);
                sd().setSwitch(kKoff | 1, pendingKoff[1]);
                for (int c = 0; c < 2; ++c)
                {
                    keyedOn[c] &= ~pendingKoff[c];
                    keyedOff[c] |= pendingKoff[c];
                    pendingKoff[c] = 0;
                }
            }
            else
                tok(FlushKoffNone);
            if (pendingKon[0] | pendingKon[1])
            {
                tok(FlushKon);
                sd().begin();
                sd().setSwitch(kVmixEL | 0, reverbMask[0]);
                sd().setSwitch(kVmixER | 0, reverbMask[0]);
                sd().setSwitch(kVmixEL | 1, reverbMask[1]);
                sd().setSwitch(kVmixER | 1, reverbMask[1]);
                sd().setSwitch(kKon | 0, pendingKon[0]);
                sd().setSwitch(kKon | 1, pendingKon[1]);
                sd().end();
                for (int c = 0; c < 2; ++c)
                {
                    keyedOff[c] &= ~pendingKon[c];
                    keyedOn[c] |= pendingKon[c];
                    pendingKon[c] = 0;
                }
            }
            else
                tok(FlushKonNone);
            tok(TickEnd);
            if (!refills.empty())
                doRefills();
            return e.m_now;
        }

        // ---- commands ----
        bool command(uint32_t id, const uint32_t a[7], uint32_t &result)
        {
            result = 0;
            const auto i32 = [](uint32_t v) { return static_cast<int32_t>(v); };
            switch (id)
            {
            case 0x09: // SetMasterVolume(group, volume)
                groupVol[a[0] & 7] = i32(a[1]);
                return false;
            case 0x0a:
                result = static_cast<uint32_t>(groupVol[a[0] & 7] == 1024 || (a[0] & 7) >= 3 ? groupVol[a[0] & 7] : groupVol[a[0] & 7]);
                return true;
            case 0x0b: playbackMode = static_cast<int16_t>(a[0]); return false;
            case 0x0e: setReverbType(a[0], a[1]); return false;
            case 0x11:
            case 0x12:
            {
                Bank *b = bankAt(a[0]);
                uint32_t r = 0;
                const int32_t idx = i32(a[1]);
                if (b && idx >= 0 && idx < b->soundCount)
                {
                    const uint8_t *snd = b->sound(idx);
                    const int32_t type = static_cast<int32_t>(le32(snd));
                    if (type >= 1 && type <= 3)
                        r = playProgram(b, snd, i32(a[2]), i32(a[3]), static_cast<int16_t>(a[4]), static_cast<int16_t>(a[5]));
                    else if (type == 4)
                        r = playMidi(snd, i32(a[2]), i32(a[3]), static_cast<int16_t>(a[4]));
                }
                result = r;
                return id == 0x11;
            }
            case 0x15:
            case 0x2f:
                if (Handler *h = lookup(a[0]))
                    stopHandler(h, false);
                return false;
            case 0x18:
                for (Handler *h = hHead; h;)
                {
                    Handler *nx = h->next;
                    stopHandler(h, true);
                    h = nx;
                }
                {
                    const uint32_t all[2] = {0xFFFFFFu, 0xFFFFFFu};
                    silenceMask(all);
                }
                return false;
            case 0x19:
                result = a[0] && lookup(a[0]) ? a[0] : 0;
                return true;
            case 0x20: setPitchModifier(a[0], static_cast<int16_t>(a[1])); return false;
            case 0x21:
            {
                if (!lookup(a[0]))
                    return true;
                const uint32_t mask = a[1];
                const int32_t vol = (mask & 1) ? i32(a[2]) : 0x7FFFFFFF;
                const int32_t pan = (mask & 2) ? i32(a[3]) : -2;
                if (mask & 3)
                    setVolPan(a[0], vol, pan);
                if (mask & 4)
                    autoPan(a[0], i32(a[3]));
                if (mask & 8)
                    setPitchModifier(a[0], static_cast<int16_t>(a[4]));
                if (mask & 0x10)
                    setPitchBend(a[0], static_cast<int16_t>(a[5]));
                result = a[0];
                return true;
            }
            case 0x2a: initStreaming(a[0], a[1]); return false;
            case 0x2c: result = playStream(a); return true;
            case 0x2d: pauseStream(a[0]); return false;
            case 0x34:
                for (Handler &s : S)
                    if (s.used)
                        stopStream(s.handle);
                return false;
            default: return false;
            }
        }
        uint32_t reverbType[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}, reverbArea[2] = {};
        void setReverbType(uint32_t cores, uint32_t type)
        {
            static constexpr uint32_t kArea[10] = {0x80, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};
            for (uint32_t core = 0; core < 2; ++core)
            {
                if (!(cores >> core & 1u) || reverbType[core] == type)
                    continue;
                if (static_cast<int32_t>(reverbType[core]) > 0)
                    spuFree(reverbArea[core], kArea[reverbType[core] % 10]);
                if (type)
                {
                    reverbArea[core] = spuAllocAligned(kArea[type % 10]);
                    if (!reverbArea[core])
                    {
                        reverbType[core] = 0xFFFFFFFFu;
                        continue;
                    }
                    sd().setAddr(kEea | core, reverbArea[core] + kArea[type % 10] - 1);
                }
                reverbType[core] = type;
            }
        }
    };

    // -----------------------------------------------------------------------------------------------------------------
    Engine::Engine()
        : m_mixer(std::make_unique<audio::Mixer>()), m_ram(0x10000), m_impl(std::make_unique<Impl>(*this))
    {
        m_mixer->setClocked(true);
        m_libsd = std::make_unique<audio::Libsd>(*m_mixer, *this);
        m_impl->init();
    }

    Engine::~Engine() = default;

    void Engine::bankLoaded(uint32_t address, const std::vector<uint8_t> &header, const std::vector<uint8_t> &samples,
                            const std::vector<uint8_t> &midi)
    {
        Impl &I = *m_impl;
        auto b = std::make_unique<Impl::Bank>();
        b->address = address;
        b->h = header;
        if (b->h.size() < 0x28)
            return;
        b->id = le32(&b->h[0x0c]);
        b->number = s8(b->h[0x10]);
        b->soundCount = le16s(&b->h[0x14]);
        b->programCount = le16s(&b->h[0x16]);
        b->sounds = le32(&b->h[0x1c]);
        b->programs = le32(&b->h[0x20]);
        b->spuBytes = static_cast<uint32_t>(samples.size());
        b->spu = I.spuAlloc(b->spuBytes);
        m_now = m_clock;
        if (b->spu && !samples.empty())
        {
            m_ram.resize(std::max<size_t>(m_ram.size(), samples.size()));
            std::memcpy(m_ram.data(), samples.data(), samples.size());
            I.sd().voiceTrans(0, 0, 0, b->spu, static_cast<uint32_t>(samples.size()));
        }
        // Tones' sample offsets become sound memory addresses.
        for (int p = 0; p < b->programCount; ++p)
        {
            const uint32_t at = b->programs + 8u * static_cast<uint32_t>(p);
            if (at + 8 > b->h.size())
                break;
            const int n = s8(b->h[at]);
            const uint32_t t = le32(&b->h[at + 4]);
            for (int k = 0; k < n && t + 24u * k + 24 <= b->h.size(); ++k)
            {
                uint8_t *q = &b->h[t + 24u * k + 16];
                const uint32_t v = le32(q) + b->spu;
                std::memcpy(q, &v, 4);
            }
        }
        Impl::Bank *raw = b.get();
        I.banks.push_back(std::move(b));
        // The MIDI chunk: 16 bytes (2, 1, data offset, data size), then one MID or MMID block (Part B 1.1).
        if (midi.size() >= 16)
        {
            const uint32_t off = le32(&midi[8]), size = le32(&midi[12]);
            if (off + size <= midi.size())
            {
                raw->midi.assign(midi.begin() + off, midi.begin() + off + size);
                auto loadMid = [&](uint32_t at) -> Impl::Midi * {
                    const uint8_t *m = &raw->midi[at];
                    auto mid = std::make_unique<Impl::Midi>();
                    mid->owner = raw;
                    mid->id = le32(m + 8);
                    mid->bankId = le32(m + 0x10);
                    for (auto &bk : I.banks)
                        if (bk->id == mid->bankId)
                        {
                            mid->bank = bk.get();
                            break;
                        }
                    mid->data = m + le32(m + 0x18);
                    mid->tempo = le32(m + 0x20);
                    mid->ppq = le16(m + 0x24);
                    Impl::Midi *p = mid.get();
                    raw->mids.push_back(std::move(mid));
                    return p;
                };
                const uint8_t *m = raw->midi.data();
                if (le32(m) == 0x2044494Du)
                {
                    Impl::Block blk;
                    blk.owner = raw;
                    blk.mid = loadMid(0);
                    blk.id = blk.mid->id;
                    I.blocks.push_back(blk);
                }
                else if (le32(m) == 0x44494D4Du)
                {
                    Impl::Block blk;
                    blk.owner = raw;
                    blk.multi = true;
                    blk.id = le32(m + 8);
                    const int count = s8(m[7]);
                    std::vector<Impl::Block> subsBlocks;
                    for (int i = 0; i < count; ++i)
                    {
                        Impl::Midi *sub = loadMid(le32(m + 0x10 + 4u * i));
                        blk.subs.push_back(sub);
                        Impl::Block sb;
                        sb.owner = raw;
                        sb.mid = sub;
                        sb.id = sub->id;
                        subsBlocks.push_back(sb);
                    }
                    I.blocks.push_back(blk);
                    for (auto &sb : subsBlocks)
                        I.blocks.push_back(sb);
                }
            }
        }
    }

    void Engine::bankUnloaded(uint32_t address)
    {
        Impl &I = *m_impl;
        for (auto it = I.banks.begin(); it != I.banks.end(); ++it)
            if ((*it)->address == address)
            {
                if ((*it)->spu)
                    I.spuFree((*it)->spu, (*it)->spuBytes);
                Impl::Bank *b = it->get();
                std::erase_if(I.blocks, [&](const Impl::Block &k) { return k.owner == b; });
                I.banks.erase(it);
                return;
            }
    }

    void Engine::runTick(uint64_t service)
    {
        static FILE *trace = std::getenv("BOZO_SND989_TRACE") ? std::fopen(std::getenv("BOZO_SND989_TRACE"), "w") : nullptr;
        if (trace)
            std::fprintf(trace, "T %llu %llu %x %x%c", static_cast<unsigned long long>(m_ticks), static_cast<unsigned long long>(service),
                         m_impl->pendingKon[0], m_impl->pendingKon[1], 10);
        ++m_ticks;
        m_now = service;
        m_mixer->advanceTo(service);
        m_impl->traceCalls = trace != nullptr;
        m_impl->tickService = service;
        m_impl->callLog.clear();
        m_clock = m_impl->tick(service);
        m_impl->traceCalls = false;
        if (trace)
        {
            std::fprintf(trace, "C %u", m_impl->now);
            for (const auto &c : m_impl->callLog)
                std::fprintf(trace, " %u:%x:%llu:%x", c.ordinal, c.entry, static_cast<unsigned long long>(c.offset), c.result);
            std::fputc(10, trace);
            // A debugging aid: each voice's state, owner handle, priority and silent count after the tick.
            const Impl &I = *m_impl;
            std::fprintf(trace, "S %u", I.now);
            for (int v = 0; v < kVoices; ++v)
                std::fprintf(trace, " %d:%x:%u:%u", I.V[v].state, I.V[v].owner ? I.V[v].owner->handle : 0u, I.V[v].prio, I.silent[v]);
            std::fputc('\n', trace);
        }
        m_nextDue = service + kTick;
    }

    void Engine::advance(uint64_t P)
    {
        Impl &I = *m_impl;
        // Ticks and the stream threads' key-ons in time order, up to P (a tick first when both are due together).
        for (;;)
        {
            size_t k = I.keyOns.size();
            for (size_t i = 0; i < I.keyOns.size(); ++i)
                if (k == I.keyOns.size() || I.keyOns[i].at < I.keyOns[k].at)
                    k = i;
            const bool haveKey = k < I.keyOns.size();
            if (haveKey && I.keyOns[k].at < m_nextDue)
            {
                if (I.keyOns[k].at > P)
                    break;
                m_clock = std::max(m_clock, I.keyOns[k].at);
                m_now = m_clock;
                Impl::Buffer *b = I.keyOns[k].b;
                I.keyOns.erase(I.keyOns.begin() + static_cast<std::ptrdiff_t>(k));
                if (!(b->flags & 1))
                    I.keyOnStream(b);
                m_clock += 400;
                continue;
            }
            if (m_nextDue > P)
                break;
            runTick(std::max(m_nextDue, m_clock));
        }
        m_clock = std::max(m_clock, P);
    }

    bool Engine::command(uint32_t id, const uint32_t a[7], uint64_t ee, uint32_t &result)
    {
        const uint64_t P = ee > timing.eeReset ? (ee - timing.eeReset) / 8 + timing.g : 0;
        if (!m_started)
        {
            m_started = true;
            m_nextDue = P + kTick;
            m_clock = P;
        }
        if (id == 0x2a)
        {
            advance(P);
            m_nextDue = P + timing.streamInitPhase;
        }
        advance(P);
        m_now = m_clock;
        if (static const char *dump = std::getenv("BOZO_SND989_DUMP"); dump && !m_dumped && ee >= std::strtoull(dump, nullptr, 10))
        {
            // A debugging aid: the voice table and the handlers before the first command at or after an EE cycle.
            m_dumped = true;
            Impl &I = *m_impl;
            std::fprintf(stderr, "snd989 dump: command %02x clock %llu now %u\n", id, static_cast<unsigned long long>(m_clock), I.now);
            for (int v = 0; v < kVoices; ++v)
            {
                const auto &x = I.V[v];
                std::fprintf(stderr, "%d st %d own %x prio %u age %u sil %u%s", v, x.state, x.owner ? x.owner->handle : 0u, x.prio,
                             x.age, I.silent[v], v % 4 == 3 ? "\n" : " | ");
            }
        }
        const uint64_t start = m_now;
        const bool returns = m_impl->command(id, a, result);
        static FILE *trace = std::getenv("BOZO_SND989_CMDTRACE") ? std::fopen(std::getenv("BOZO_SND989_CMDTRACE"), "w") : nullptr;
        if (trace)
            std::fprintf(trace, "K %x %lld %x%c", id, static_cast<long long>(m_now - start), a[1], 10);
        m_clock = m_now + commandCost(id);
        if (id == 0x2c)
        {
            for (Impl::Buffer *L : m_impl->pendingStarts)
                m_impl->startPipeline(L, m_clock);
            m_impl->pendingStarts.clear();
        }
        return returns;
    }
}
