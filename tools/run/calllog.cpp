// The call log reader. See calllog.h and docs/specs/calllog.md.
#include "calllog.h"

#include <zlib.h>

#include <cstring>
#include <format>

namespace bozo::calllog
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
        constexpr uint32_t kOutcallHeaderBytes = 36 + 4 * kContextMaskWords; // 80

        bool readRuns(const uint8_t *b, uint32_t &pos, uint32_t end, uint32_t count, std::vector<Run> &out,
                      std::string &error)
        {
            out.clear();
            out.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                if (pos + 8 > end)
                {
                    error = std::format("run {} past the end", i);
                    return false;
                }
                Run r;
                r.address = rd32(b + pos);
                r.length = rd32(b + pos + 4);
                if (pos + 8ull + r.length > end || uint64_t(r.address) + r.length > 0x2005000u)
                {
                    error = std::format("run {} at 0x{:x} ({} bytes) out of bounds", i, r.address, r.length);
                    return false;
                }
                r.data = b + pos + 8;
                out.push_back(r);
                pos += 8 + ((r.length + 3u) & ~3u);
            }
            return true;
        }

        bool readWords(const uint8_t *b, uint32_t maskAt, uint32_t valuesAt, uint32_t count, uint32_t end,
                       std::vector<std::pair<uint32_t, uint32_t>> &out, std::string &error)
        {
            out.clear();
            if (valuesAt + 4ull * count > end)
            {
                error = "context words past the end";
                return false;
            }
            for (uint32_t w = 0; w < kContextWords; ++w)
                if (rd32(b + maskAt + 4 * (w / 32)) >> (w % 32) & 1u)
                {
                    if (out.size() == count)
                        break;
                    out.emplace_back(w, rd32(b + valuesAt + 4 * static_cast<uint32_t>(out.size())));
                }
            if (out.size() != count)
            {
                error = "context mask and count disagree";
                return false;
            }
            return true;
        }

        bool readTraces(const uint8_t *b, uint32_t &pos, uint32_t end, std::vector<Trace> &out, std::string &error)
        {
            out.clear();
            if (pos + 4 > end)
            {
                error = "no traced-call count";
                return false;
            }
            const uint32_t n = rd32(b + pos);
            pos += 4;
            if (pos + 32ull * n > end)
            {
                error = "traced calls past the end";
                return false;
            }
            out.resize(n);
            for (uint32_t i = 0; i < n; ++i, pos += 32)
            {
                Trace &t = out[i];
                t.id = rd32(b + pos);
                t.address = rd32(b + pos + 4);
                for (int k = 0; k < 4; ++k)
                    t.a[k] = rd32(b + pos + 8 + 4 * k);
                t.v0 = rd32(b + pos + 24);
                t.flags = rd32(b + pos + 28);
            }
            return true;
        }
    }

    const char *typeName(uint32_t type)
    {
        static const char *const n[] = {"?", "CALL", "MEM", "CTX", "INVOKE", "INVOKE_END", "SWITCH", "FRAME", "OP", "INIT"};
        return type < 10 ? n[type] : "?";
    }

    const char *kindName(uint32_t kind)
    {
        static const char *const n[] = {"?", "stub", "override", "syscall", "vu0", "io-load", "io-store", "exception"};
        return kind < 8 ? n[kind] : "?";
    }

    uint32_t Record::u32(uint32_t offset) const { return offset + 4 <= bytes ? rd32(p + offset) : 0; }
    uint64_t Record::u64(uint32_t offset) const { return offset + 8 <= bytes ? rd64(p + offset) : 0; }

    bool parse(const Record &r, Call &c, std::string &error)
    {
        if (r.bytes < 96 + kOutcallHeaderBytes + 4)
        {
            error = "CALL too short";
            return false;
        }
        c.ra = r.u32(40);
        c.sp = r.u32(44);
        for (int i = 0; i < 4; ++i)
            c.t[i] = r.u32(48 + 4 * i);
        c.f12 = r.u32(64);
        c.f13 = r.u32(68);
        c.cycleAfter = r.u64(72);
        c.timerBefore = r.u32(80);
        c.timerAfter = r.u32(84);
        c.depth = r.u32(88);
        const uint8_t *b = r.p;
        const uint32_t o = 96;
        c.kind = b[o];
        c.size = b[o + 1];
        c.oflags = static_cast<uint16_t>(b[o + 2] | (b[o + 3] << 8));
        c.target = rd32(b + o + 4);
        for (int i = 0; i < 4; ++i)
            c.value[i] = rd32(b + o + 8 + 4 * i);
        const uint32_t ctxCount = rd32(b + o + 24), runCount = rd32(b + o + 28);
        if (!readWords(b, o + 36, o + kOutcallHeaderBytes, ctxCount, r.bytes, c.words, error))
            return false;
        uint32_t pos = o + kOutcallHeaderBytes + 4 * ctxCount;
        if (!readRuns(b, pos, r.bytes, runCount, c.runs, error))
            return false;
        if (!readTraces(b, pos, r.bytes, c.traces, error))
            return false;
        if (pos != r.bytes)
        {
            error = std::format("{} bytes after the traced calls", r.bytes - pos);
            return false;
        }
        return true;
    }

    bool parse(const Record &r, Mem &m, std::string &error)
    {
        uint32_t pos;
        uint32_t count;
        if (r.type == kInit)
        {
            count = r.u32(40);
            m.originCall = ~0ull;
            pos = 48;
        }
        else
        {
            count = r.u32(40);
            m.originCall = r.u64(48);
            pos = 56;
        }
        return readRuns(r.p, pos, r.bytes, count, m.runs, error);
    }

    bool parse(const Record &r, Ctx &c, std::string &error)
    {
        c.thread = static_cast<int32_t>(r.u32(40));
        c.depth = r.u32(44);
        const uint32_t count = r.u32(48);
        return readWords(r.p, 56, 56 + 4 * kContextMaskWords, count, r.bytes, c.words, error);
    }

    bool parse(const Record &r, Invoke &v, std::string &)
    {
        v.thread = static_cast<int32_t>(r.u32(40));
        v.depth = r.u32(44);
        v.pc = r.u32(48);
        v.flags = r.u32(52);
        v.tag = r.u64(56);
        v.sequence = r.u64(64);
        for (int i = 0; i < 4; ++i)
            v.a[i] = r.u32(72 + 4 * i);
        v.gp = r.u32(88);
        v.sp = r.u32(92);
        v.ra = r.u32(96);
        return true;
    }

    bool parse(const Record &r, InvokeEnd &v, std::string &)
    {
        v.thread = static_cast<int32_t>(r.u32(40));
        v.depth = r.u32(44);
        v.v0 = r.u32(48);
        v.v1 = r.u32(52);
        v.sequence = r.u64(56);
        return true;
    }

    bool parse(const Record &r, Switch &s, std::string &)
    {
        s.from = static_cast<int32_t>(r.u32(40));
        s.fromDepth = r.u32(44);
        s.to = static_cast<int32_t>(r.u32(48));
        s.toDepth = r.u32(52);
        s.waitReason = r.u32(56);
        s.waitObject = r.u32(60);
        s.token = r.u64(64);
        return true;
    }

    bool parse(const Record &r, Frame &f, std::string &error)
    {
        f.ended = r.u32(40);
        const uint32_t runCount = r.u32(44), traceCount = r.u32(48);
        uint32_t pos = 56;
        if (!readRuns(r.p, pos, r.bytes, runCount, f.runs, error))
            return false;
        f.traces.clear();
        if (pos + 32ull * traceCount > r.bytes)
        {
            error = "FRAME traced calls past the end";
            return false;
        }
        for (uint32_t i = 0; i < traceCount; ++i, pos += 32)
        {
            Trace t;
            t.id = rd32(r.p + pos);
            t.address = rd32(r.p + pos + 4);
            for (int k = 0; k < 4; ++k)
                t.a[k] = rd32(r.p + pos + 8 + 4 * k);
            t.v0 = rd32(r.p + pos + 24);
            t.flags = rd32(r.p + pos + 28);
            f.traces.push_back(t);
        }
        return true;
    }

    bool parse(const Record &r, Op &o, std::string &error)
    {
        o.origin = r.u32(40);
        const uint32_t count = r.u32(44);
        o.depth = r.u32(48);
        o.originCall = r.u64(56);
        o.wide = r.u64(64);
        if (72 + 4ull * count > r.bytes)
        {
            error = "OP arguments past the end";
            return false;
        }
        o.args.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            o.args[i] = r.u32(72 + 4 * i);
        return true;
    }

    Reader::~Reader()
    {
        if (f_)
            std::fclose(f_);
    }

    bool Reader::open(const std::string &path)
    {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_)
        {
            error_ = "cannot open " + path;
            return false;
        }
        std::setvbuf(f_, nullptr, _IOFBF, 1 << 22);
        uint8_t h[kHeaderBytes];
        if (std::fread(h, 1, kHeaderBytes, f_) != kHeaderBytes || std::memcmp(h, "PS2XCLG1", 8) != 0)
        {
            error_ = path + ": not a PS2XCLG1 call log";
            return false;
        }
        Header &x = header_;
        x.version = rd32(h + 8);
        x.headerBytes = rd32(h + 12);
        x.contextBytes = rd32(h + 16);
        x.flatBytes = rd32(h + 20);
        x.ramBytes = rd32(h + 24);
        x.scratchpadBytes = rd32(h + 28);
        x.vu0DataBytes = rd32(h + 32);
        x.pageBytes = rd32(h + 36);
        x.frameLimit = rd32(h + 40);
        x.flags = rd32(h + 44);
        x.elfHash = rd64(h + 48);
        x.eeClock = rd32(h + 56);
        x.recordHeaderBytes = rd32(h + 60);
        x.run.assign(reinterpret_cast<const char *>(h + 64), strnlen(reinterpret_cast<const char *>(h + 64), 64));
        if (x.contextBytes != kContextWords * 4 || x.flatBytes != 0x2005000u || x.recordHeaderBytes != kRecordHeaderBytes)
        {
            error_ = path + ": another layout";
            return false;
        }
        if (x.headerBytes != kHeaderBytes)
            std::fseek(f_, x.headerBytes, SEEK_SET);
        return true;
    }

    bool Reader::readChunk(std::string &tag, std::vector<uint8_t> &payload)
    {
        uint8_t h[16];
        if (std::fread(h, 1, 16, f_) != 16)
            return false;
        tag.assign(reinterpret_cast<const char *>(h), 4);
        const uint32_t flags = rd32(h + 4), stored = rd32(h + 8), raw = rd32(h + 12);
        if (flags & 1u)
        {
            raw_.resize(stored);
            if (std::fread(raw_.data(), 1, stored, f_) != stored)
                return false;
            payload.resize(raw);
            uLongf out = raw;
            if (uncompress(payload.data(), &out, raw_.data(), stored) != Z_OK || out != raw)
            {
                error_ = "a chunk does not inflate";
                return false;
            }
        }
        else
        {
            payload.resize(stored);
            if (std::fread(payload.data(), 1, stored, f_) != stored)
                return false;
        }
        return true;
    }

    void Reader::readMeta(const std::string &tag, const std::vector<uint8_t> &p)
    {
        if (tag == "NAME" && p.size() >= 4)
        {
            const uint32_t n = rd32(p.data());
            size_t q = 4;
            for (uint32_t i = 0; i < n && q + 12 <= p.size(); ++i)
            {
                const uint32_t address = rd32(p.data() + q), cls = rd32(p.data() + q + 4), len = rd32(p.data() + q + 8);
                NameInfo info;
                info.name.assign(reinterpret_cast<const char *>(p.data() + q + 12), len);
                info.cls = cls;
                names_[address] = info;
                q += 12 + ((len + 3u) & ~3u);
            }
        }
        else if (tag == "OVRD" && p.size() >= 4)
        {
            const uint32_t n = rd32(p.data());
            for (uint32_t i = 0; i < n && 4 + 4 * (i + 1) <= p.size(); ++i)
                overrides_.insert(rd32(p.data() + 4 + 4 * i));
        }
        else if (tag == "END " && p.size() >= 32)
        {
            endSeen_ = true;
            endRecords_ = rd64(p.data());
            endCalls_ = rd64(p.data() + 8);
        }
    }

    bool Reader::loadChunk()
    {
        std::string tag;
        std::vector<uint8_t> payload;
        for (;;)
        {
            if (eof_ || !readChunk(tag, payload))
            {
                eof_ = true;
                return false;
            }
            if (tag == "RECS")
            {
                prev_.swap(cur_);
                cur_.swap(payload);
                pos_ = 0;
                return true;
            }
            readMeta(tag, payload);
        }
    }

    const Record *Reader::peek()
    {
        if (haveRecord_)
            return &rec_;
        while (pos_ >= cur_.size())
            if (!loadChunk())
                return nullptr;
        if (pos_ + kRecordHeaderBytes > cur_.size())
        {
            error_ = std::format("record {}: header past its chunk", index_);
            return nullptr;
        }
        const uint8_t *b = cur_.data() + pos_;
        Record &r = rec_;
        r.index = index_;
        r.type = b[0];
        r.flags = b[1];
        r.sub = static_cast<uint16_t>(b[2] | (b[3] << 8));
        r.bytes = rd32(b + 4);
        r.cycle = rd64(b + 8);
        r.seq = rd64(b + 16);
        r.frame = rd32(b + 24);
        r.thread = static_cast<int32_t>(rd32(b + 28));
        r.checkpoints = rd32(b + 32);
        r.pc = rd32(b + 36);
        r.p = b;
        if (r.bytes < kRecordHeaderBytes || r.bytes % 4 || pos_ + r.bytes > cur_.size())
        {
            error_ = std::format("record {}: size {}", index_, r.bytes);
            return nullptr;
        }
        haveRecord_ = true;
        return &rec_;
    }

    void Reader::advance()
    {
        if (!haveRecord_ && !peek())
            return;
        pos_ += rec_.bytes;
        ++index_;
        haveRecord_ = false;
    }

    bool readNames(const std::string &path, std::map<uint32_t, NameInfo> &names, std::set<uint32_t> &overrides,
                   std::string &error)
    {
        Reader r;
        if (!r.open(path))
        {
            error = r.error();
            return false;
        }
        r.peek(); // NAME and OVRD come before the first records
        names = r.names();
        overrides = r.overrides();
        return true;
    }
}
