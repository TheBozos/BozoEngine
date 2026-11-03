// The per-frame reference record (PS2XREF1, docs/specs/harness.md section 6): what BozoEngine writes for every game
// frame so a run can be compared with the reference build's, field by field. Also a reader for comparing.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bozo::reference
{
    constexpr uint32_t kHeaderBytes = 240;
    constexpr uint32_t kRecordBytes = 80;

    struct FrameRecord
    {
        uint32_t frame = 0;
        int32_t thread = 0;       // the guest thread running at the frame end
        uint64_t ram = 0;         // XXH64 of the 32 MB of RAM
        uint64_t scratchpad = 0;  // XXH64 of the 16 KB scratchpad
        uint64_t calls = 0;       // XXH64 of the frame's calls (CallList)
        uint64_t draws = 0;       // the renderer's draw lists (0 until BozoEngine has its renderer)
        uint64_t sound = 0;       // FNV-1a 64 of the frame's samples (0 until it has its mixer)
        uint64_t threads = 0;     // XXH64 of i32 {running thread, the ready threads in run order}
        uint64_t eeCycle = 0;     // the EE cycle count at the frame end
        uint32_t callCount = 0, drawBatches = 0, drawVertices = 0, soundSamples = 0;
    };

    struct Header
    {
        uint32_t version = 1, headerBytes = kHeaderBytes, recordBytes = kRecordBytes, hash = 1;
        uint32_t ramBytes = 0x2000000, scratchpadBytes = 0x4000;
        uint64_t elfHash = 0;
        uint32_t flags = 1, maskCount = 0;
        std::string run, build, elfName;
    };

    // The frame's calls: each 7 u32 {id, a0, a1, a2, a3, v0, flags}, in call order.
    class CallList
    {
    public:
        enum Flags : uint32_t
        {
            kBlocked = 1,     // the call blocked (its result comes later)
            kOpenAtEnd = 2,   // the frame ended inside it
        };
        void add(uint32_t id, const uint32_t a[4], uint32_t v0, uint32_t flags)
        {
            words_.insert(words_.end(), {id, a[0], a[1], a[2], a[3], v0, flags});
        }
        uint32_t count() const { return static_cast<uint32_t>(words_.size() / 7); }
        uint64_t hash() const;
        void clear() { words_.clear(); }
        const std::vector<uint32_t> &words() const { return words_; }
        // The id of a stub (FNV-1a 32 of its name, top bit clear) or of a syscall (0x80000000 | number).
        static uint32_t stubId(const std::string &name);
        static uint32_t syscallId(uint32_t number) { return 0x80000000u | number; }

    private:
        std::vector<uint32_t> words_;
    };

    uint64_t threadsHash(const std::vector<int32_t> &order);

    class Writer
    {
    public:
        ~Writer();
        bool open(const std::string &path, const Header &h);
        void write(const FrameRecord &r);
        void close();

    private:
        std::FILE *f_ = nullptr;
    };

    bool read(const std::string &path, Header &h, std::vector<FrameRecord> &frames, std::string &error);
}
