// The replay of captured samples against BozoEngine's generated code. See replay.h and docs/specs/replay.md.
#include "replay.h"

#include <cstdio>
#include <cstring>
#include <format>

namespace bozo::replay
{
    using namespace capture;

    namespace
    {
        constexpr size_t kMaxRanges = 64;
        constexpr uint32_t kPage = 4096;

        uint32_t lo32(uint64_t v) { return static_cast<uint32_t>(v); }
        uint32_t hi32(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
        void setLo(uint64_t &v, uint32_t x) { v = (v & 0xFFFFFFFF00000000ull) | x; }
        void setHi(uint64_t &v, uint32_t x) { v = (v & 0xFFFFFFFFull) | (static_cast<uint64_t>(x) << 32); }
    }

    std::string statusText(uint32_t s)
    {
        static const char *const names[] = {"context", "memory",  "diverged",  "outcalls-left", "arguments",
                                            "runaway", "no-entry", "exception", "crashed"};
        if (s == 0)
            return "match";
        std::string t;
        for (int i = 0; i < 9; ++i)
            if (s >> i & 1u)
                t += (t.empty() ? "" : ",") + std::string(names[i]);
        return t;
    }

    uint32_t guestAddress(uint32_t flat)
    {
        if (flat >= kFlatVu0Offset)
            return kVu0DataBase + (flat - kFlatVu0Offset);
        if (flat >= kFlatScratchpadOffset)
            return kScratchpadBase + (flat - kFlatScratchpadOffset);
        return flat;
    }

    uint32_t getWord(const R5900Context &c, const Runtime &rt, uint32_t i)
    {
        if (i < 128)
            return c.r[i / 4].uw[i % 4];
        switch (i)
        {
        case kWordHi: return lo32(c.hi);
        case kWordHi + 1: return hi32(c.hi);
        case kWordLo: return lo32(c.lo);
        case kWordLo + 1: return hi32(c.lo);
        case kWordHi1: return lo32(c.hi1);
        case kWordHi1 + 1: return hi32(c.hi1);
        case kWordLo1: return lo32(c.lo1);
        case kWordLo1 + 1: return hi32(c.lo1);
        case kWordPc: return c.pc;
        case kWordSa: return c.sa;
        case kWordFcr31: return c.fcr31;
        case kWordFacc: return c.facc;
        case kWordQ: return c.vu0_q_bits;
        case kWordI: return c.vu0_i_bits;
        case kWordP: return c.vu0_p_bits;
        case kWordR: return c.vu0_r;
        case kWordVuStatus: return c.vu0_status;
        case kWordVuMac: return c.vu0_mac;
        case kWordVuClip: return c.vu0_clip_flags;
        case kWordCmsar0: return c.vu0_cmsar0;
        case kWordCop0Status: return rt.cop0Status;
        default: break;
        }
        if (i >= kWordF0 && i < kWordFcr31)
            return c.fu[i - kWordF0];
        if (i >= kWordVf0 && i < kWordAcc)
            return c.vu0_vf[(i - kWordVf0) / 4].u[(i - kWordVf0) % 4];
        if (i >= kWordAcc && i < kWordVi0)
            return c.vu0_acc.u[i - kWordAcc];
        if (i >= kWordVi0 && i < kWordQ)
            return c.vi[i - kWordVi0];
        return 0; // reserved
    }

    void setWord(R5900Context &c, Runtime &rt, uint32_t i, uint32_t v)
    {
        if (i < 128)
        {
            c.r[i / 4].uw[i % 4] = v;
            return;
        }
        switch (i)
        {
        case kWordHi: setLo(c.hi, v); return;
        case kWordHi + 1: setHi(c.hi, v); return;
        case kWordLo: setLo(c.lo, v); return;
        case kWordLo + 1: setHi(c.lo, v); return;
        case kWordHi1: setLo(c.hi1, v); return;
        case kWordHi1 + 1: setHi(c.hi1, v); return;
        case kWordLo1: setLo(c.lo1, v); return;
        case kWordLo1 + 1: setHi(c.lo1, v); return;
        case kWordPc: c.pc = v; return;
        case kWordSa: c.sa = v; return;
        case kWordFcr31: c.fcr31 = v; return;
        case kWordFacc: c.facc = v; return;
        case kWordQ: c.vu0_q_bits = v; return;
        case kWordI: c.vu0_i_bits = v; return;
        case kWordP: c.vu0_p_bits = v; return;
        case kWordR: c.vu0_r = v; return;
        case kWordVuStatus: c.vu0_status = v; return;
        case kWordVuMac: c.vu0_mac = v; return;
        case kWordVuClip: c.vu0_clip_flags = v; return;
        case kWordCmsar0: c.vu0_cmsar0 = v; return;
        case kWordCop0Status: rt.cop0Status = v; return;
        default: break;
        }
        if (i >= kWordF0 && i < kWordFcr31)
            c.fu[i - kWordF0] = v;
        else if (i >= kWordVf0 && i < kWordAcc)
            c.vu0_vf[(i - kWordVf0) / 4].u[(i - kWordVf0) % 4] = v;
        else if (i >= kWordAcc && i < kWordVi0)
            c.vu0_acc.u[i - kWordAcc] = v;
        else if (i >= kWordVi0 && i < kWordQ)
            c.vi[i - kWordVi0] = v;
    }

    Replayer::Replayer(const BozoFunctionEntry *table, size_t count, const std::map<uint32_t, uint8_t> &externals)
        : chain_(bozo::kFlatBytes, 0), externals_(externals)
    {
        rt_.services = this;
        rt_.functions.setGenerated(table, count);
        for (const auto &[address, kind] : externals_)
            rt_.functions.setOverride(address, &Replayer::external);
    }

    bool Replayer::chainEntry(const std::vector<uint8_t> &payload, std::string &error)
    {
        // The same runs into both copies keeps them equal.
        return applyEntryRuns(payload, chain_.data(), error) && applyEntryRuns(payload, rt_.memory.flat(), error);
    }

    void Replayer::fail(uint32_t status, std::string text)
    {
        result_->status |= status;
        if (trace_)
            std::printf("    !! %s\n", text.c_str());
        if (result_->outcallIssues.size() < 16)
            result_->outcallIssues.push_back(std::move(text));
    }

    const Outcall &Replayer::expect(uint8_t kind, uint32_t target, const char *what)
    {
        if (next_ >= sample_->outcalls.size())
        {
            fail(kDiverged, std::format("#{} {} 0x{:x}: the log has no more out-calls", next_, what, target));
            throw Abort{};
        }
        const Outcall &o = sample_->outcalls[next_];
        const bool kindOk = o.kind == kind || ((kind == kStub || kind == kOverride) && (o.kind == kStub || o.kind == kOverride));
        if (!kindOk || o.target != target)
        {
            fail(kDiverged, std::format("#{} {} 0x{:x}: the log has {} 0x{:x}", next_, what, target, kindName(o.kind), o.target));
            throw Abort{};
        }
        if (trace_)
            std::printf("    #%zu %s 0x%x (%zu context words, %zu runs)\n", next_, kindName(o.kind), o.target, o.words.size(),
                        o.runs.size());
        ++next_;
        result_->outcallsUsed = static_cast<uint32_t>(next_);
        return o;
    }

    void Replayer::checkArgs(const Outcall &o, const uint32_t got[4], int n, const char *what)
    {
        for (int k = 0; k < n; ++k)
            if (got[k] != o.value[k])
            {
                std::string t = std::format("#{} {} 0x{:x}: values", next_ - 1, what, o.target);
                for (int j = 0; j < n; ++j)
                    t += std::format(" {:08x}", got[j]);
                t += ", the log has";
                for (int j = 0; j < n; ++j)
                    t += std::format(" {:08x}", o.value[j]);
                fail(kArguments, std::move(t));
                return;
            }
    }

    void Replayer::apply(const Outcall &o, R5900Context *ctx)
    {
        for (const auto &[w, v] : o.words)
            if (w != kWordPc) // the reference's bookkeeping (capture.md 7.3)
                setWord(*ctx, rt_, w, v);
        uint8_t *flat = rt_.memory.flat();
        for (const Run &r : o.runs)
            std::memcpy(flat + r.address, r.data, r.length);
    }

    void Replayer::external(uint8_t *, R5900Context *ctx, Runtime *rt)
    {
        Replayer &self = *static_cast<Replayer *>(rt->services);
        const uint32_t target = ctx->pc;
        const auto it = self.externals_.find(target);
        const uint8_t kind = it != self.externals_.end() ? it->second : static_cast<uint8_t>(kStub);
        const Outcall &o = self.expect(kind, target, kind == kOverride ? "override" : "stub");
        const uint32_t args[4] = {ctx->r[4].uw[0], ctx->r[5].uw[0], ctx->r[6].uw[0], ctx->r[7].uw[0]};
        self.checkArgs(o, args, 4, kindName(o.kind));
        self.apply(o, ctx);
    }

    void Replayer::syscall(uint8_t *, R5900Context *ctx, Runtime *, uint32_t)
    {
        const Outcall &o = expect(kSyscall, ctx->pc, "syscall");
        const uint32_t args[4] = {ctx->r[3].uw[0], ctx->r[4].uw[0], ctx->r[5].uw[0], ctx->r[6].uw[0]};
        checkArgs(o, args, 4, "syscall");
        apply(o, ctx);
    }

    void Replayer::vu0CallMicroprogram(uint8_t *, R5900Context *ctx, Runtime *, uint32_t address)
    {
        const Outcall &o = expect(kVu0, address, "vu0");
        apply(o, ctx);
    }

    void Replayer::ioRead(R5900Context *ctx, Runtime *, uint32_t address, uint32_t size, uint64_t value[2])
    {
        const Outcall &o = expect(kIoLoad, address, "io-load");
        if (o.size != size)
            fail(kArguments, std::format("#{} io-load 0x{:x}: size {}, the log has {}", next_ - 1, address, size, o.size));
        value[0] = o.value[0] | (static_cast<uint64_t>(o.value[1]) << 32);
        value[1] = o.value[2] | (static_cast<uint64_t>(o.value[3]) << 32);
        apply(o, ctx);
    }

    void Replayer::ioWrite(R5900Context *ctx, Runtime *, uint32_t address, uint32_t size, const uint64_t value[2])
    {
        const Outcall &o = expect(kIoStore, address, "io-store");
        if (o.size != size)
            fail(kArguments, std::format("#{} io-store 0x{:x}: size {}, the log has {}", next_ - 1, address, size, o.size));
        uint32_t got[4] = {lo32(value[0]), hi32(value[0]), lo32(value[1]), hi32(value[1])};
        const uint32_t words = size >= 16 ? 4 : size == 8 ? 2 : 1;
        if (size < 4)
            got[0] &= (1u << (8 * size)) - 1u;
        checkArgs(o, got, static_cast<int>(words), "io-store");
        apply(o, ctx);
    }

    void Replayer::unknownTarget(uint8_t *, R5900Context *, Runtime *, uint32_t target)
    {
        fail(kException, std::format("jump to 0x{:x}: no generated code and no override there", target));
        throw Abort{};
    }

    void Replayer::yield(uint8_t *, R5900Context *ctx, Runtime *)
    {
        if (ctx->cycles >= cycleBudget)
        {
            fail(kRunaway, std::format("ran {} cycles without returning", ctx->cycles));
            throw Abort{};
        }
        ctx->cycleLimit = cycleBudget;
    }

    void Replayer::breakpoint(uint8_t *, R5900Context *ctx, Runtime *, uint32_t code)
    {
        fail(kException, std::format("break 0x{:x} at 0x{:x}", code, ctx->pc));
        throw Abort{};
    }
    void Replayer::trap(uint8_t *, R5900Context *ctx, Runtime *)
    {
        fail(kException, std::format("trap at 0x{:x}", ctx->pc));
        throw Abort{};
    }
    void Replayer::unimplemented(uint8_t *, R5900Context *ctx, Runtime *, uint32_t word)
    {
        fail(kException, std::format("untranslated instruction 0x{:08x} at 0x{:x}", word, ctx->pc));
        throw Abort{};
    }
    void Replayer::eret(uint8_t *, R5900Context *ctx, Runtime *)
    {
        fail(kException, std::format("eret at 0x{:x}", ctx->pc));
        throw Abort{};
    }

    // Requests no out-call kind covers: answered with a default and noted (the open questions of replay.md).
    uint32_t Replayer::cop0Read(R5900Context *ctx, Runtime *, uint32_t reg)
    {
        result_->notes.push_back(std::format("mfc0 {} at 0x{:x} read as 0", reg, ctx->pc));
        return 0;
    }
    void Replayer::cop0Write(R5900Context *ctx, Runtime *, uint32_t reg, uint32_t value)
    {
        result_->notes.push_back(std::format("mtc0 {} = 0x{:x} at 0x{:x} ignored", reg, value, ctx->pc));
    }
    bool Replayer::cop0Condition(R5900Context *ctx, Runtime *)
    {
        result_->notes.push_back(std::format("bc0 condition at 0x{:x} read as true", ctx->pc));
        return true;
    }
    uint32_t Replayer::vu0ControlRead(R5900Context *ctx, Runtime *, uint32_t reg)
    {
        result_->notes.push_back(std::format("cfc2 {} at 0x{:x} read as 0", reg, ctx->pc));
        return 0;
    }
    void Replayer::vu0ControlWrite(R5900Context *ctx, Runtime *, uint32_t reg, uint32_t value)
    {
        result_->notes.push_back(std::format("ctc2 {} = 0x{:x} at 0x{:x} ignored", reg, value, ctx->pc));
    }

    SampleResult Replayer::run(const Sample &s, bool trace)
    {
        SampleResult res;
        sample_ = &s;
        result_ = &res;
        next_ = 0;
        trace_ = trace;

        R5900Context ctx;
        for (uint32_t w = 0; w < kContextWords; ++w)
            setWord(ctx, rt_, w, s.entry[w]);
        ctx.cycles = 0;
        ctx.cycleLimit = cycleBudget;
        uint8_t *flat = rt_.memory.flat();

        RecompiledFunction fn = rt_.functions.generated(s.function);
        bool returned = false;
        if (!fn)
            res.status |= kNoEntry;
        else
        {
            try
            {
                fn(flat, &ctx, &rt_);
                returned = true;
            }
            catch (const Abort &)
            {
            }
        }
        res.cycles = ctx.cycles;
        if (returned)
        {
            ctx.pc = s.returnAddress; // the call ended at its return address (capture.md section 5)
            if (next_ < s.outcalls.size())
            {
                const Outcall &o = s.outcalls[next_];
                res.status |= kOutcallsLeft;
                res.outcallIssues.push_back(std::format("#{}: {} 0x{:x} and {} more out-calls never made", next_,
                                                        kindName(o.kind), o.target, s.outcalls.size() - next_ - 1));
            }
            for (uint32_t w = 0; w < kContextWords; ++w)
            {
                res.got[w] = getWord(ctx, rt_, w);
                res.want[w] = s.exit[w];
                if (res.got[w] != res.want[w])
                    res.words.push_back(w);
            }
            if (!res.words.empty())
                res.status |= kContext;
        }

        // Memory: the chain copy becomes the expected exit memory (entry + exit runs); the working copy is compared
        // with it page by page and repaired; then both go back to the entry state.
        std::vector<std::vector<uint8_t>> saved;
        saved.reserve(s.exitRuns.size());
        uint8_t *chain = chain_.data();
        for (const Run &r : s.exitRuns)
        {
            saved.emplace_back(chain + r.address, chain + r.address + r.length);
            std::memcpy(chain + r.address, r.data, r.length);
        }
        for (uint32_t p = 0; p < bozo::kFlatBytes; p += kPage)
        {
            if (std::memcmp(flat + p, chain + p, kPage) == 0)
                continue;
            if (returned)
                for (uint32_t b = p; b < p + kPage; ++b)
                {
                    if (flat[b] == chain[b])
                        continue;
                    ++res.differingBytes;
                    if (!res.ranges.empty() && b - (res.ranges.back().flat + res.ranges.back().length) < 16)
                        res.ranges.back().length = b + 1 - res.ranges.back().flat;
                    else if (res.ranges.size() < kMaxRanges)
                        res.ranges.push_back({b, 1});
                }
            std::memcpy(flat + p, chain + p, kPage);
        }
        if (res.differingBytes)
            res.status |= kMemory;
        for (size_t k = s.exitRuns.size(); k-- > 0;)
        {
            const Run &r = s.exitRuns[k];
            std::memcpy(chain + r.address, saved[k].data(), r.length);
            std::memcpy(flat + r.address, saved[k].data(), r.length);
        }
        sample_ = nullptr;
        result_ = nullptr;
        return res;
    }
}
