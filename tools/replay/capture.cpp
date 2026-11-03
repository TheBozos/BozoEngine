// PS2XCAP1 reader (docs/specs/capture.md). See capture.h.
#include "capture.h"

#include "bozo/hash.h"

#include <zlib.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace bozo::capture
{
    namespace
    {
        uint32_t rd32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        uint64_t rd64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }

        // Reads `count` runs from [pos, end). Returns false if they do not fit or leave the flat space.
        bool readRuns(const uint8_t *base, size_t &pos, size_t end, uint32_t count, std::vector<Run> *out,
                      std::string &error, const char *what)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (pos + 8 > end)
                {
                    error = std::format("{} run {} past the end", what, i);
                    return false;
                }
                Run r;
                r.address = rd32(base + pos);
                r.length = rd32(base + pos + 4);
                const size_t padded = (static_cast<size_t>(r.length) + 3) & ~size_t(3);
                if (r.length == 0 || uint64_t(r.address) + r.length > kFlatBytes || pos + 8 + padded > end)
                {
                    error = std::format("{} run {} at 0x{:x} length {} is malformed", what, i, r.address, r.length);
                    return false;
                }
                r.data = base + pos + 8;
                if (out)
                    out->push_back(r);
                pos += 8 + padded;
            }
            return true;
        }

    }

    const char *kindName(uint32_t kind)
    {
        static const char *const names[] = {"?", "stub", "override", "syscall", "vu0", "io-load", "io-store", "exception"};
        return kind < 8 ? names[kind] : "?";
    }

    std::string wordName(uint32_t i)
    {
        static const char *const gpr[] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                          "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                          "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        static const char *const hl[] = {"hi", "lo", "hi1", "lo1"};
        static const char *const tail[] = {"Q",          "I",       "P",        "R",           "vu0_status", "vu0_mac",
                                           "vu0_clip",   "cmsar0",  "cop0_status", "reserved", "reserved", "reserved"};
        if (i < 128)
            return std::format("{}.w{}", gpr[i / 4], i % 4);
        if (i < 136)
            return std::format("{}.w{}", hl[(i - 128) / 2], (i - 128) % 2);
        if (i == kWordPc)
            return "pc";
        if (i == kWordSa)
            return "sa";
        if (i < kWordFcr31)
            return std::format("f{}", i - kWordF0);
        if (i == kWordFcr31)
            return "fcr31";
        if (i == kWordFacc)
            return "facc";
        if (i < kWordAcc)
            return std::format("vf{}.{}", (i - kWordVf0) / 4, "xyzw"[(i - kWordVf0) % 4]);
        if (i < kWordVi0)
            return std::format("acc.{}", "xyzw"[i - kWordAcc]);
        if (i < kWordQ)
            return std::format("vi{}", i - kWordVi0);
        if (i < kContextWords)
            return tail[i - kWordQ];
        return std::format("word{}", i);
    }

    bool Reader::open(const std::string &path)
    {
        namespace fs = std::filesystem;
        path_ = fs::is_directory(path) ? (fs::path(path) / "capture.bin").string() : path;
        std::ifstream in(path_, std::ios::binary);
        if (!in)
        {
            error_ = "cannot read " + path_;
            return false;
        }
        file_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (file_.size() < kHeaderBytes || std::memcmp(file_.data(), "PS2XCAP1", 8) != 0)
        {
            error_ = path_ + ": not a PS2XCAP1 capture";
            return false;
        }
        const uint8_t *h = file_.data();
        Header &x = header_;
        x.version = rd32(h + 8);
        x.headerBytes = rd32(h + 12);
        x.contextBytes = rd32(h + 16);
        x.flatBytes = rd32(h + 20);
        x.ramBytes = rd32(h + 24);
        x.scratchpadBytes = rd32(h + 28);
        x.vu0DataBytes = rd32(h + 32);
        x.samplesPerFunction = rd32(h + 36);
        x.frameFrom = rd32(h + 40);
        x.frameTo = rd32(h + 44);
        x.elfHash = rd64(h + 48);
        x.pageBytes = rd32(h + 56);
        x.run.assign(reinterpret_cast<const char *>(h + 64), strnlen(reinterpret_cast<const char *>(h + 64), 64));
        if (x.version != 1 || x.contextBytes != kContextBytes || x.flatBytes != kFlatBytes || x.headerBytes < kHeaderBytes ||
            x.headerBytes > file_.size())
        {
            error_ = std::format("{}: version {}, context {} bytes, flat {} bytes: not this reader's layout", path_,
                                 x.version, x.contextBytes, x.flatBytes);
            return false;
        }
        pos_ = x.headerBytes;
        return true;
    }

    bool Reader::next(Chunk &chunk)
    {
        if (pos_ + 16 > file_.size())
            return false;
        const uint8_t *c = file_.data() + pos_;
        const uint32_t flags = rd32(c + 4), stored = rd32(c + 8), raw = rd32(c + 12);
        if (pos_ + 16 + uint64_t(stored) > file_.size())
            return false; // cut short: every chunk before is valid
        std::memcpy(chunk.tag, c, 4);
        chunk.tag[4] = 0;
        if (flags & 1u)
        {
            chunk.data.resize(raw);
            uLongf out = raw;
            if (uncompress(chunk.data.data(), &out, c + 16, stored) != Z_OK || out != raw)
            {
                error_ = std::format("{}: chunk at {} does not decompress to {} bytes", path_, pos_, raw);
                return false;
            }
        }
        else
            chunk.data.assign(c + 16, c + 16 + stored);
        pos_ += 16 + static_cast<size_t>(stored);
        return true;
    }

    void sampleId(const std::vector<uint8_t> &p, uint32_t &function, uint32_t &index)
    {
        function = p.size() >= 8 ? rd32(p.data()) : 0;
        index = p.size() >= 8 ? rd32(p.data() + 4) : 0;
    }

    bool applyEntryRuns(const std::vector<uint8_t> &p, uint8_t *flat, std::string &error)
    {
        if (p.size() < 64 + kContextBytes)
        {
            error = "sample shorter than its fixed parts";
            return false;
        }
        const uint32_t count = rd32(p.data() + 32), bytes = rd32(p.data() + 36);
        size_t pos = 64 + kContextBytes;
        const size_t end = pos + bytes;
        if (end > p.size())
        {
            error = "entry runs past the end";
            return false;
        }
        std::vector<Run> runs;
        if (!readRuns(p.data(), pos, end, count, &runs, error, "entry"))
            return false;
        for (const Run &r : runs)
            std::memcpy(flat + r.address, r.data, r.length);
        return true;
    }

    bool parseSample(std::vector<uint8_t> &&payload, Sample &s, std::string &error)
    {
        s = Sample();
        s.payload = std::move(payload);
        const uint8_t *b = s.payload.data();
        const size_t n = s.payload.size();
        if (n < 64 + 2 * kContextBytes)
        {
            error = "sample shorter than its fixed parts";
            return false;
        }
        s.function = rd32(b);
        s.index = rd32(b + 4);
        s.frame = rd32(b + 8);
        s.thread = static_cast<int32_t>(rd32(b + 12));
        s.callIndex = rd64(b + 16);
        s.slot = rd32(b + 24);
        const uint32_t outcallCount = rd32(b + 28), entryRunCount = rd32(b + 32), entryRunBytes = rd32(b + 36),
                       outcallBytes = rd32(b + 40), exitRunCount = rd32(b + 44), exitRunBytes = rd32(b + 48);
        s.returnAddress = rd32(b + 52);
        s.stackPointer = rd32(b + 56);
        if (64ull + 2 * kContextBytes + entryRunBytes + outcallBytes + exitRunBytes != n)
        {
            error = std::format("sample {}: its parts do not add up to the chunk's {} bytes", s.index, n);
            return false;
        }
        size_t pos = 64;
        std::memcpy(s.entry.data(), b + pos, kContextBytes);
        pos += kContextBytes;
        if (!readRuns(b, pos, pos + entryRunBytes, entryRunCount, &s.entryRuns, error, "entry"))
            return false;
        const size_t outEnd = pos + outcallBytes;
        for (uint32_t k = 0; k < outcallCount; ++k)
        {
            if (pos + 80 > outEnd)
            {
                error = std::format("sample {}: out-call {} past the end", s.index, k);
                return false;
            }
            Outcall o;
            o.kind = b[pos];
            o.size = b[pos + 1];
            o.target = rd32(b + pos + 4);
            for (int v = 0; v < 4; ++v)
                o.value[v] = rd32(b + pos + 8 + 4 * v);
            const uint32_t ctxCount = rd32(b + pos + 24), runCount = rd32(b + pos + 28);
            uint32_t mask[kMaskWords];
            for (uint32_t m = 0; m < kMaskWords; ++m)
                mask[m] = rd32(b + pos + 36 + 4 * m);
            pos += 80;
            if (pos + 4ull * ctxCount > outEnd)
            {
                error = std::format("sample {}: out-call {} context words past the end", s.index, k);
                return false;
            }
            for (uint32_t w = 0; w < kContextWords; ++w)
                if (mask[w / 32] >> (w % 32) & 1u)
                {
                    if (o.words.size() == ctxCount)
                        break;
                    o.words.emplace_back(w, rd32(b + pos + 4 * o.words.size()));
                }
            if (o.words.size() != ctxCount)
            {
                error = std::format("sample {}: out-call {} mask and count disagree", s.index, k);
                return false;
            }
            pos += 4ull * ctxCount;
            if (!readRuns(b, pos, outEnd, runCount, &o.runs, error, "out-call"))
                return false;
            s.outcalls.push_back(std::move(o));
        }
        if (pos != outEnd)
        {
            error = std::format("sample {}: out-calls do not fill their bytes", s.index);
            return false;
        }
        std::memcpy(s.exit.data(), b + pos, kContextBytes);
        pos += kContextBytes;
        if (!readRuns(b, pos, pos + exitRunBytes, exitRunCount, &s.exitRuns, error, "exit"))
            return false;
        return true;
    }

    uint64_t xxh64(const uint8_t *p, size_t len, uint64_t seed) { return bozo::hash::xxh64(p, len, seed); }

    bool hashFile(const std::string &path, uint64_t &hash)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        hash = xxh64(bytes.data(), bytes.size(), 0);
        return true;
    }
}
