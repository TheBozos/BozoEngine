// A reader for PS2XCAP1 per-function captures (docs/specs/capture.md): the header, the chunks (zlib payloads
// decompressed) and the samples with their out-calls and memory runs.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bozo::capture
{
    constexpr uint32_t kHeaderBytes = 128;
    constexpr uint32_t kContextWords = 332;
    constexpr uint32_t kContextBytes = kContextWords * 4;
    constexpr uint32_t kMaskWords = (kContextWords + 31) / 32; // 11
    constexpr uint32_t kFlatBytes = 0x2005000u;

    // Context word indices (capture.md section 5).
    constexpr uint32_t kWordHi = 128, kWordLo = 130, kWordHi1 = 132, kWordLo1 = 134;
    constexpr uint32_t kWordPc = 136, kWordSa = 137, kWordF0 = 138, kWordFcr31 = 170, kWordFacc = 171;
    constexpr uint32_t kWordVf0 = 172, kWordAcc = 300, kWordVi0 = 304, kWordQ = 320, kWordI = 321, kWordP = 322,
                       kWordR = 323, kWordVuStatus = 324, kWordVuMac = 325, kWordVuClip = 326, kWordCmsar0 = 327,
                       kWordCop0Status = 328;

    enum OutcallKind : uint8_t
    {
        kStub = 1,
        kOverride = 2,
        kSyscall = 3,
        kVu0 = 4,
        kIoLoad = 5,
        kIoStore = 6,
        kException = 7,
    };
    const char *kindName(uint32_t kind);
    // "v0.w0", "f12", "vf3.y", "cop0_status"...
    std::string wordName(uint32_t word);

    struct Header
    {
        uint32_t version = 0, headerBytes = 0, contextBytes = 0, flatBytes = 0, ramBytes = 0, scratchpadBytes = 0,
                 vu0DataBytes = 0, samplesPerFunction = 0, frameFrom = 0, frameTo = 0;
        uint64_t elfHash = 0;
        uint32_t pageBytes = 0;
        std::string run;
    };

    // A memory run: `length` bytes at flat address `address`; `data` points into the sample's payload.
    struct Run
    {
        uint32_t address = 0;
        uint32_t length = 0;
        const uint8_t *data = nullptr;
    };

    struct Outcall
    {
        uint8_t kind = 0;
        uint8_t size = 0;
        uint32_t target = 0;
        uint32_t value[4] = {};
        std::vector<std::pair<uint32_t, uint32_t>> words; // (context word, new value), in word order
        std::vector<Run> runs;
    };

    struct Sample
    {
        uint32_t function = 0, index = 0, frame = 0;
        int32_t thread = 0;
        uint64_t callIndex = 0;
        uint32_t slot = 0, returnAddress = 0, stackPointer = 0;
        std::array<uint32_t, kContextWords> entry{};
        std::array<uint32_t, kContextWords> exit{};
        std::vector<Run> entryRuns;
        std::vector<Outcall> outcalls;
        std::vector<Run> exitRuns;
        std::vector<uint8_t> payload; // owns the bytes the runs point into
    };

    struct Chunk
    {
        char tag[5] = {};
        std::vector<uint8_t> data; // decompressed
    };

    class Reader
    {
    public:
        // Reads the whole file (a capture.bin, or a folder holding one). Returns false with error() set.
        bool open(const std::string &path);
        const Header &header() const { return header_; }
        const std::string &error() const { return error_; }
        const std::string &path() const { return path_; }
        // The next chunk; false at the end of the file (or on an error: error() non-empty; a truncated last chunk is
        // the end, not an error, as capture.md section 6 allows).
        bool next(Chunk &chunk);
        void rewind() { pos_ = header_.headerBytes; }

    private:
        std::string path_;
        std::vector<uint8_t> file_;
        size_t pos_ = 0;
        Header header_;
        std::string error_;
    };

    // Parses a SAMP payload (moved into the sample). Returns false with a message on a malformed sample.
    bool parseSample(std::vector<uint8_t> &&payload, Sample &s, std::string &error);
    // Reads a SAMP payload's function and index without parsing it.
    void sampleId(const std::vector<uint8_t> &payload, uint32_t &function, uint32_t &index);
    // Applies the entry runs of a SAMP payload to flat memory without parsing the rest (for chaining quickly).
    bool applyEntryRuns(const std::vector<uint8_t> &payload, uint8_t *flat, std::string &error);

    // XXH64 (seed 0) of a file's bytes; false if it cannot be read.
    bool hashFile(const std::string &path, uint64_t &hash);
    uint64_t xxh64(const uint8_t *data, size_t length, uint64_t seed);
}
