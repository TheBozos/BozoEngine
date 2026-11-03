// PS2XREF1 records. See bozo/reference.h and docs/specs/harness.md section 6.
#include "bozo/reference.h"

#include "bozo/hash.h"

#include <cstring>

namespace bozo::reference
{
    namespace
    {
        void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
        void put64(std::vector<uint8_t> &b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }
        uint32_t get32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        uint64_t get64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }
        void putText(std::vector<uint8_t> &b, size_t at, const std::string &s)
        {
            std::memcpy(b.data() + at, s.data(), s.size() < 63 ? s.size() : 63);
        }
        std::string getText(const uint8_t *p) { return std::string(reinterpret_cast<const char *>(p), strnlen(reinterpret_cast<const char *>(p), 64)); }
    }

    uint64_t CallList::hash() const { return hash::xxh64(words_.data(), words_.size() * 4, 0); }

    uint32_t CallList::stubId(const std::string &name) { return hash::fnv1a32(name.data(), name.size()) & 0x7FFFFFFFu; }

    uint64_t threadsHash(const std::vector<int32_t> &order) { return hash::xxh64(order.data(), order.size() * 4, 0); }

    Writer::~Writer() { close(); }

    bool Writer::open(const std::string &path, const Header &h)
    {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_)
            return false;
        std::vector<uint8_t> b(kHeaderBytes, 0);
        std::memcpy(b.data(), "PS2XREF1", 8);
        put32(b, 8, h.version);
        put32(b, 12, kHeaderBytes);
        put32(b, 16, kRecordBytes);
        put32(b, 20, h.hash);
        put32(b, 24, h.ramBytes);
        put32(b, 28, h.scratchpadBytes);
        put64(b, 32, h.elfHash);
        put32(b, 40, h.flags);
        put32(b, 44, 0);
        putText(b, 48, h.run);
        putText(b, 112, h.build);
        putText(b, 176, h.elfName);
        std::fwrite(b.data(), 1, b.size(), f_);
        return true;
    }

    void Writer::write(const FrameRecord &r)
    {
        if (!f_)
            return;
        std::vector<uint8_t> b(kRecordBytes, 0);
        put32(b, 0, r.frame);
        put32(b, 4, static_cast<uint32_t>(r.thread));
        put64(b, 8, r.ram);
        put64(b, 16, r.scratchpad);
        put64(b, 24, r.calls);
        put64(b, 32, r.draws);
        put64(b, 40, r.sound);
        put64(b, 48, r.threads);
        put64(b, 56, r.eeCycle);
        put32(b, 64, r.callCount);
        put32(b, 68, r.drawBatches);
        put32(b, 72, r.drawVertices);
        put32(b, 76, r.soundSamples);
        std::fwrite(b.data(), 1, b.size(), f_);
    }

    void Writer::close()
    {
        if (f_)
            std::fclose(f_);
        f_ = nullptr;
    }

    bool read(const std::string &path, Header &h, std::vector<FrameRecord> &frames, std::string &error)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            error = "cannot open " + path;
            return false;
        }
        std::vector<uint8_t> all;
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            all.insert(all.end(), buf, buf + n);
        std::fclose(f);
        if (all.size() < kHeaderBytes || std::memcmp(all.data(), "PS2XREF1", 8) != 0)
        {
            error = path + ": not a PS2XREF1 record";
            return false;
        }
        const uint8_t *p = all.data();
        h.version = get32(p + 8);
        h.headerBytes = get32(p + 12);
        h.recordBytes = get32(p + 16);
        h.hash = get32(p + 20);
        h.ramBytes = get32(p + 24);
        h.scratchpadBytes = get32(p + 28);
        h.elfHash = get64(p + 32);
        h.flags = get32(p + 40);
        h.maskCount = get32(p + 44);
        h.run = getText(p + 48);
        h.build = getText(p + 112);
        h.elfName = getText(p + 176);
        if (h.recordBytes < kRecordBytes)
        {
            error = path + ": records too short";
            return false;
        }
        frames.clear();
        for (size_t at = h.headerBytes; at + h.recordBytes <= all.size(); at += h.recordBytes)
        {
            const uint8_t *r = p + at;
            FrameRecord x;
            x.frame = get32(r);
            x.thread = static_cast<int32_t>(get32(r + 4));
            x.ram = get64(r + 8);
            x.scratchpad = get64(r + 16);
            x.calls = get64(r + 24);
            x.draws = get64(r + 32);
            x.sound = get64(r + 40);
            x.threads = get64(r + 48);
            x.eeCycle = get64(r + 56);
            x.callCount = get32(r + 64);
            x.drawBatches = get32(r + 68);
            x.drawVertices = get32(r + 72);
            x.soundSamples = get32(r + 76);
            frames.push_back(x);
        }
        return true;
    }
}
