// bozo-run's log-backed host. See log_host.h and docs/specs/calllog.md section 8.
#include "log_host.h"

#include "bozo/hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace bozo::run
{
    using namespace calllog;

    namespace
    {
        std::string describe(const Record &r)
        {
            std::string s = std::format("#{} {} f{} c{} s{} t{}{} ckp{} pc 0x{:x}", r.index, typeName(r.type), r.frame,
                                        r.cycle, r.seq, r.thread, r.inside() ? " (inside)" : "", r.checkpoints, r.pc);
            std::string err;
            switch (r.type)
            {
            case kCall:
            {
                Call c;
                if (parse(r, c, err))
                    s += std::format(" {} 0x{:x} ra 0x{:x} value {:x} {:x} {:x} {:x} +{} flags {:x}", kindName(c.kind), c.target,
                                     c.ra, c.value[0], c.value[1], c.value[2], c.value[3], c.cycleAfter - r.cycle, r.flags);
                break;
            }
            case kOp:
            {
                Op o;
                if (parse(r, o, err))
                {
                    s += std::format(" {} origin {} depth {} call {}:", ee::opName(r.sub), o.origin, o.depth,
                                     static_cast<int64_t>(o.originCall));
                    for (uint32_t a : o.args)
                        s += std::format(" {:x}", a);
                    s += std::format(" wide {:x}", o.wide);
                }
                break;
            }
            case kSwitch:
            {
                Switch w;
                parse(r, w, err);
                s += std::format(" {}/{} -> {}/{} reason {} wait {} {} {:x}", w.from, w.fromDepth, w.to, w.toDepth, r.sub,
                                 w.waitReason, w.waitObject, w.token);
                break;
            }
            case kInvoke:
            {
                Invoke v;
                parse(r, v, err);
                s += std::format(" kind {} thread {} depth {} pc 0x{:x} a {:x} {:x} {:x} {:x} gp {:x} sp {:x} ra {:x} seq {} flags {}",
                                 r.sub, v.thread, v.depth, v.pc, v.a[0], v.a[1], v.a[2], v.a[3], v.gp, v.sp, v.ra, v.sequence,
                                 v.flags);
                break;
            }
            case kInvokeEnd:
            {
                InvokeEnd v;
                parse(r, v, err);
                s += std::format(" kind {} thread {} depth {} v0 {:x} v1 {:x} seq {}", r.sub, v.thread, v.depth, v.v0, v.v1,
                                 v.sequence);
                break;
            }
            case kCtx:
            {
                Ctx c;
                if (parse(r, c, err))
                {
                    s += std::format(" thread {} depth {}:", c.thread, c.depth);
                    for (const auto &[w, v] : c.words)
                        s += std::format(" w{}={:x}", w, v);
                }
                break;
            }
            case kMem: s += std::format(" source {}", r.sub); break;
            default: break;
            }
            return s;
        }

        uint32_t gpr(const R5900Context *ctx, int n) { return ctx->r[n].uw[0]; }
    }

    LogHost::LogHost(Reader &log, const Options &options) : log_(log), opt_(options) {}

    uint32_t LogHost::bind(ee::Dispatcher &d)
    {
        d_ = &d;
        d.kernel().setObserver(this);
        libs_.attach(d);
        libs_.printOutput = !opt_.quiet;
        libs_.sndNative = (opt_.native & libs::kSnd) != 0;
        names_ = log_.names();
        uint32_t kernel = 0;
        for (const auto &[address, info] : names_)
        {
            traceIds_[address] = reference::CallList::stubId(info.name);
            const ee::KernelCall k = info.cls == 1 ? ee::kernelCallByName(info.name) : ee::KernelCall::None;
            if (k != ee::KernelCall::None)
            {
                d.bindKernelCall(address, k);
                ++kernel;
            }
            else
            {
                if (info.cls == 1 && !opt_.quiet)
                    std::printf("note: kernel function %s (0x%x) has no native implementation: answered from the log\n",
                                info.name.c_str(), address);
                d.bindLibrary(address);
                if (const libs::Function *f = libs_.find(info.name); f && (f->library & opt_.native))
                    nativeAt_[address] = f;
            }
            idNames_[traceIds_[address]] = info.name.rfind("ps2_", 0) == 0 ? info.name.substr(4) : info.name;
        }
        for (uint32_t address : log_.overrides())
            if (!d.isKernelCall(address))
                d.bindLibrary(address);
        return kernel;
    }

    bool LogHost::init(ee::Dispatcher &d)
    {
        const Record *p = log_.peek();
        if (!p || p->type != kInit)
            return false;
        Mem m;
        std::string err;
        if (!parse(*p, m, err))
            return false;
        uint8_t *flat = d.runtime().memory.flat();
        for (const Run &r : m.runs)
            std::memcpy(flat + r.address, r.data, r.length);
        log_.advance();
        ++matched_;
        return true;
    }

    // ---- Reporting -------------------------------------------------------------------------------------------------

    std::string LogHost::where() const
    {
        if (!d_)
            return "";
        const ee::Thread *t = d_->currentThread();
        const ee::Frame *f = d_->currentFrame();
        return std::format("frame {}, cycle {}, seq {}, thread {} depth {}, pc 0x{:x}, sp 0x{:x}, {} checkpoints since the last "
                           "out-call",
                           frame_, d_->now(), seq_, t ? t->id : d_->kernel().running(), t ? t->depth() : 0,
                           f ? f->ctx.pc : 0, f ? f->ctx.r[29].uw[0] : 0, d_->checkpointsSince());
    }

    void LogHost::endRun(const std::string &why)
    {
        if (!opt_.quiet && !why.empty())
            std::printf("%s\n", why.c_str());
        throw ee::EndRun{};
    }

    void LogHost::diverge(const std::string &what, const Record *r, bool resync)
    {
        ++errors_;
        std::printf("DIVERGENCE %u: %s\n  log:  %s\n  here: %s\n", errors_, what.c_str(),
                    r ? describe(*r).c_str() : "(no record)", where().c_str());
        if (!resync || errors_ >= opt_.maxErrors)
            endRun("");
    }

    const Record &LogHost::next(const char *wanted)
    {
        const Record *p = log_.peek();
        if (!p)
        {
            if (!log_.error().empty())
                endRun("log error: " + log_.error());
            completed_ = true;
            endRun(std::format("the log has no more records (wanted {})", wanted));
        }
        return *p;
    }

    void LogHost::trace(const Record &r, const char *note)
    {
        if (r.index >= opt_.traceFrom)
        {
            std::printf("  ok %s%s\n", describe(r).c_str(), note);
            if (d_)
                d_->traceCheckpoints = opt_.traceCheckpoints;
        }
    }

    void LogHost::checkPoint(const Record &r, uint64_t cycle, const char *what)
    {
        if (opt_.pinned)
        {
            // Pinned mode: EE time and the checkpoint count are reported, not checked; the clock follows the log.
            if (r.checkpoints != d_->checkpointsSince())
            {
                ++pinned_.checkpointsDiffer;
                pinned_.firstCheckpointRecord = std::min(pinned_.firstCheckpointRecord, r.index);
            }
            if (r.cycle != cycle)
            {
                ++pinned_.cycleDiffers;
                pinned_.firstCycleRecord = std::min(pinned_.firstCycleRecord, r.index);
                d_->setNow(nullptr, r.cycle);
            }
            return;
        }
        if (r.cycle != cycle)
        {
            diverge(std::format("{}: cycle {} (log {}, {:+}); checkpoints since the last out-call: {} (log {})", what, cycle,
                                r.cycle, static_cast<int64_t>(cycle) - static_cast<int64_t>(r.cycle), d_->checkpointsSince(),
                                r.checkpoints),
                    &r, true);
            d_->setNow(nullptr, r.cycle);
        }
    }

    // ---- Out-calls -----------------------------------------------------------------------------------------------

    uint32_t LogHost::traceId(uint8_t kind, uint32_t target, const R5900Context *ctx) const
    {
        if (kind == kSyscall)
            return reference::CallList::syscallId(gpr(ctx, 3));
        if (kind != kStub && kind != kOverride)
            return 0;
        auto it = traceIds_.find(target);
        return it == traceIds_.end() ? 0 : it->second;
    }

    void LogHost::addTraces(const std::vector<Trace> &t, size_t from)
    {
        for (size_t i = from; i < t.size(); ++i)
            calls_.add(t[i].id, t[i].a, t[i].v0, t[i].flags & 1u);
    }

    const Record &LogHost::takeCall(uint8_t kind, uint32_t target, R5900Context *ctx, Call &c)
    {
        const Record &r = next("CALL");
        if (r.type != kCall)
            diverge(std::format("{} 0x{:x}: the log has a {} record", kindName(kind), target, typeName(r.type)), &r);
        std::string err;
        if (!parse(r, c, err))
            endRun(std::format("record {}: {}", r.index, err));
        const bool kindOk = c.kind == kind || ((kind == kStub || kind == kOverride) && (c.kind == kStub || c.kind == kOverride));
        if (!kindOk || c.target != target)
            diverge(std::format("{} 0x{:x}: the log has {} 0x{:x}", kindName(kind), target, kindName(c.kind), c.target), &r);
        if (r.seq != seq_)
            diverge(std::format("{} 0x{:x}: seq {}, the log has {}", kindName(kind), target, seq_, r.seq), &r);
        const ee::Thread *t = d_->currentThread();
        if (r.thread != (t ? t->id : 0))
            diverge(std::format("{} 0x{:x}: on thread {}, the log has {}", kindName(kind), target, t ? t->id : 0, r.thread), &r);
        if (c.depth != d_->currentDepth())
            diverge(std::format("{} 0x{:x}: at depth {}, the log has {}", kindName(kind), target, d_->currentDepth(), c.depth), &r);
        callDrift_ = static_cast<int64_t>(d_->now(ctx)) - static_cast<int64_t>(r.cycle);
        checkPoint(r, d_->now(ctx), std::format("{} 0x{:x}", kindName(kind), target).c_str());
        if (c.timerBefore != d_->timerPending())
            diverge(std::format("{} 0x{:x}: timer-poll cycles pending {}, the log has {}", kindName(kind), target,
                                d_->timerPending(), c.timerBefore),
                    &r);
        return r;
    }

    void LogHost::applyWords(R5900Context *ctx, const Call &c)
    {
        uint32_t &status = d_->runtime().cop0Status;
        for (const auto &[w, v] : c.words)
            if (w != kWordPc)
                setContextWord(*ctx, status, w, v);
    }

    void LogHost::applyRuns(const std::vector<Run> &runs)
    {
        uint8_t *flat = d_->runtime().memory.flat();
        for (const Run &r : runs)
            std::memcpy(flat + r.address, r.data, r.length);
    }

    bool LogHost::insideRecords(R5900Context *ctx, uint64_t callSeq, const Call &c, uint32_t &tracesTaken, bool native)
    {
        bool invoked = false;
        std::string err;
        for (;;)
        {
            const Record *p = log_.peek();
            if (!p || !p->inside() || p->seq != callSeq)
                break;
            const Record r = *p;
            d_->setNow(ctx, r.cycle); // the library's own time inside the call
            if (native && r.type != kFrame)
            {
                // A native call makes its own operations (matched as they are made): anything left is one it missed.
                diverge(std::format("the native call did not make the log's {} inside it", typeName(r.type)), &r, true);
                log_.advance();
                continue;
            }
            switch (r.type)
            {
            case kOp:
            {
                Op o;
                if (!parse(r, o, err))
                    endRun(err);
                if (o.depth != 0)
                    diverge(std::format("the log has a {} of depth {} the kernel did not make", ee::opName(r.sub), o.depth), &r);
                d_->kernel().replay(r.sub, o.args.data(), static_cast<uint32_t>(o.args.size()), o.wide);
                const Record *q = log_.peek();
                if (q && q->index == r.index)
                    diverge(std::format("the kernel did not make the log's {}", ee::opName(r.sub)), &r);
                break;
            }
            case kInvoke:
            {
                Invoke v;
                parse(r, v, err);
                ee::QueuedHandler h;
                h.kind = r.sub;
                h.pc = v.pc;
                for (int i = 0; i < 4; ++i)
                    h.a[i] = v.a[i];
                h.gp = v.gp;
                h.sp = v.sp;
                h.ra = v.ra;
                h.completion = (v.flags & 1u) != 0;
                h.tag = v.tag;
                d_->stackOnCurrent(h);
                invoked = true;
                break;
            }
            case kFrame:
            {
                Frame f;
                if (!parse(r, f, err))
                    endRun(err);
                log_.advance();
                ++matched_;
                trace(r);
                endFrame(r, f, openId_, openArgs_);
                tracesTaken = static_cast<uint32_t>(f.traces.size());
                (void)c;
                break;
            }
            default:
                diverge(std::format("a {} record inside a call", typeName(r.type)), &r);
                log_.advance();
                break;
            }
        }
        return invoked;
    }

    void LogHost::answer(R5900Context *ctx, const Record &r, Call &c, bool traced, const libs::Function *native)
    {
        const uint64_t callSeq = seq_++;
        ++matched_;
        trace(r);
        const bool tail = d_->runtime().tailCall;
        d_->runtime().tailCall = false;
        const uint32_t id = traced && !tail ? traceId(c.kind, c.target, ctx) : 0;
        const uint32_t args[4] = {gpr(ctx, 4), gpr(ctx, 5), gpr(ctx, 6), gpr(ctx, 7)};
        openId_ = id;
        std::memcpy(openArgs_, args, sizeof(args));
        blockedInCall_ = false;
        const bool frameInside = (r.flags & kFrameInside) != 0;
        if (!native)
        {
            applyWords(ctx, c);
            if (!frameInside)
                applyRuns(c.runs);
        }
        d_->kernel().setOrigin(ee::kOriginCall, callSeq);
        if (native)
            runNative(ctx, r, c, *native);
        uint32_t tracesTaken = 0;
        const uint32_t frameBefore = frame_;
        const bool invoked = insideRecords(ctx, callSeq, c, tracesTaken, native != nullptr);
        if (!native)
        {
            applyWords(ctx, c);
            if (frameInside)
                applyRuns(c.runs);
            observe(ctx, r, c);
            if (frameInside)
                libs_.io.t0Rebase = true; // the port's frame boundary restarts timer 0 (libraries.md section 11)
        }
        d_->setNow(ctx, c.cycleAfter);
        d_->setTimerPending(c.timerAfter);
        if (id && frame_ == frameBefore)
            calls_.add(id, args, blockedInCall_ ? 0 : gpr(ctx, 2), blockedInCall_ ? reference::CallList::kBlocked : 0);
        addTraces(c.traces, tracesTaken);
        openId_ = 0;

        const ee::Thread *t = d_->currentThread();
        const bool mine = invoked || !t || d_->kernel().running() != t->id;
        const bool theirs = (r.flags & kUnwound) != 0;
        if (mine != theirs)
            diverge(std::format("{} 0x{:x}: the call {} here, the log's {}", kindName(c.kind), c.target,
                                mine ? "left to the dispatcher" : "returned", theirs ? "left" : "returned"),
                    &r);
        d_->markOutcallEnd(ctx); // also when the call leaves to the dispatcher (the count restarts here)
        uint32_t pc = gpr(ctx, 31);
        bool hasPc = false;
        for (const auto &[w, v] : c.words)
            if (w == kWordPc)
            {
                pc = v;
                hasPc = true;
            }
        if (mine)
            d_->stop(ctx, pc);
        if (r.flags & kTransfer)
        {
            if (!hasPc)
                diverge("a transfer without a pc", &r);
            d_->stop(ctx, pc);
        }
        // A call that returned with the context somewhere else than its return address (an override that ran guest
        // code and left it elsewhere): the guest code goes back to the dispatcher, which runs it on from there.
        if ((c.kind == kStub || c.kind == kOverride) && hasPc && pc != c.ra)
            d_->stop(ctx, pc);
    }

    void LogHost::libraryCall(ee::Dispatcher &, R5900Context *ctx, uint32_t target)
    {
        Call c;
        const Record r = takeCall(kStub, target, ctx, c);
        for (int k = 0; k < 4; ++k)
            if (gpr(ctx, 4 + k) != c.value[k])
            {
                diverge(std::format("{} 0x{:x}: a0-a3 {:x} {:x} {:x} {:x}, the log has {:x} {:x} {:x} {:x}", kindName(c.kind),
                                    target, gpr(ctx, 4), gpr(ctx, 5), gpr(ctx, 6), gpr(ctx, 7), c.value[0], c.value[1],
                                    c.value[2], c.value[3]),
                        &r, true);
                break;
            }
        if (gpr(ctx, 31) != c.ra || gpr(ctx, 29) != c.sp)
            diverge(std::format("0x{:x}: ra {:x} sp {:x}, the log has ra {:x} sp {:x}", target, gpr(ctx, 31), gpr(ctx, 29),
                                c.ra, c.sp),
                    &r, true);
        log_.advance();
        // A call the run was cut inside (its end not recorded) stays the log's.
        auto n = (r.flags & kCut) ? nativeAt_.end() : nativeAt_.find(target);
        answer(ctx, r, c, true, n != nativeAt_.end() ? n->second : nullptr);
    }

    void LogHost::vu0Microprogram(ee::Dispatcher &, R5900Context *ctx, uint32_t address)
    {
        Call c;
        const Record r = takeCall(kVu0, address, ctx, c);
        log_.advance();
        const libs::Function *native = (opt_.native & libs::kVu0) ? libs_.vu0Program(address) : nullptr;
        if ((opt_.native & libs::kVu0) && !native)
            ++native_.notNative;
        answer(ctx, r, c, false, native);
    }

    void LogHost::ioRead(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, uint64_t value[2])
    {
        Call c;
        const Record r = takeCall(kIoLoad, address, ctx, c);
        if (c.size != size)
            diverge(std::format("io-load 0x{:x}: size {}, the log has {}", address, size, c.size), &r, true);
        value[0] = c.value[0] | (static_cast<uint64_t>(c.value[1]) << 32);
        value[1] = c.value[2] | (static_cast<uint64_t>(c.value[3]) << 32);
        log_.advance();
        if ((opt_.native & libs::kIo) && libs::ioKnown(address, size, false))
        {
            if ((address & 0x1FFFFFFFu) == 0x10000000u && libs_.io.t0Rebase)
            {
                // The timer's count restarted inside the port's frame boundary (answered from the log): it goes on
                // from the log's value here. The override's own last read of the count is not in the log either: the
                // log's pending poll cycles say whether it read this same value.
                libs_.io.t0Rebase = false;
                libs_.io.t0Start = c.value[0] & 0xFFFFu;
                libs_.io.t0Base = d_->now(ctx);
                libs_.io.t0LastRead = c.timerAfter != c.timerBefore ? libs_.io.t0Start : ~0u;
                ++native_.observed;
            }
            libs_.ioAccess = libs::IoAccess{address, size, false, 0, c.timerBefore};
            answer(ctx, r, c, false, &libs::Libraries::ioFunction());
            value[0] = libs_.ioAccess.value; // equal to the log's when no divergence was reported
            value[1] = 0;
            return;
        }
        if (opt_.native & libs::kIo)
            ++native_.notNative;
        answer(ctx, r, c, false);
    }

    void LogHost::ioWrite(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, const uint64_t value[2])
    {
        Call c;
        const Record r = takeCall(kIoStore, address, ctx, c);
        uint32_t got[4] = {static_cast<uint32_t>(value[0]), static_cast<uint32_t>(value[0] >> 32),
                           static_cast<uint32_t>(value[1]), static_cast<uint32_t>(value[1] >> 32)};
        const uint32_t words = size >= 16 ? 4 : size == 8 ? 2 : 1;
        if (size < 4)
            got[0] &= (1u << (8 * size)) - 1u;
        bool same = c.size == size;
        for (uint32_t k = 0; k < words; ++k)
            same = same && got[k] == c.value[k];
        if (!same)
            diverge(std::format("io-store 0x{:x}: size {} value {:x}, the log has size {} value {:x}", address, size, got[0],
                                c.size, c.value[0]),
                    &r, true);
        log_.advance();
        if ((opt_.native & libs::kIo) && libs::ioKnown(address, size, true))
        {
            libs_.ioAccess = libs::IoAccess{address, size, true, value[0], c.timerBefore};
            answer(ctx, r, c, false, &libs::Libraries::ioFunction());
            return;
        }
        if (opt_.native & libs::kIo)
            ++native_.notNative;
        // Answered from the log: CPCOND0 and the VU0 image follow the game's DMA stores.
        trackDma(address, got[0]);
        if ((address & 0x1FFFFFFFu) == 0x10008030u)
            vif0Tadr_ = got[0];
        else if ((address & 0x1FFFFFFFu) == 0x10008000u && (got[0] & 0x100u))
            libs_.vu0.kick(got[0], vif0Tadr_);
        answer(ctx, r, c, false);
    }

    void LogHost::trackDma(uint32_t address, uint32_t value)
    {
        static const uint32_t chcr[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                          0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};
        const uint32_t a = address & 0x1FFFFFFFu;
        if (a == 0x1000E010u)
            dmaDone_ &= ~(value & 0x3FFu);
        else if (a == 0x1000E020u)
        {
            dmaPcr_ = value & 0x3FFu;
            dmaPcrKnown_ = true;
        }
        else
            for (uint32_t ch = 0; ch < 10; ++ch)
                if (a == chcr[ch] && (value & 0x100u))
                    dmaDone_ |= 1u << ch;
    }

    bool LogHost::cop0Condition(ee::Dispatcher &, R5900Context *)
    {
        if (opt_.native & libs::kIo)
            return libs_.io.cpcond0();
        if (dmaPcrKnown_)
            return dmaPcr_ != 0 && (dmaDone_ & dmaPcr_) == dmaPcr_;
        return dmaDone_ != 0;
    }

    // ---- Native library calls ----------------------------------------------------------------------------------

    void LogHost::runNative(R5900Context *ctx, const Record &r, const Call &c, const libs::Function &f)
    {
        uint32_t &status = d_->runtime().cop0Status;
        ContextWords entry;
        toWords(*ctx, status, entry);
        journal_.clear();
        libs::Guest g(d_->runtime(), ctx, &journal_);
        std::string diff;
        // A timed call in a pinned run decides about time as the reference did (kernel.md section 4): the EE cycles
        // the log's call took, and whether its thread waited and until when (the log's block, the first operation
        // inside it).
        const uint64_t logCycles = c.cycleAfter - r.cycle;
        libs_.pin = libs::Pin{};
        if (f.timed && opt_.pinned)
        {
            libs_.pin.active = true;
            libs_.pin.cycles = logCycles;
            for (const Run &run : c.runs)
                libs_.pin.writtenEnd = std::max(libs_.pin.writtenEnd, run.address + run.length);
            if (const Record *p = log_.peek(); p && p->inside() && p->seq == r.seq && p->type == kOp && p->sub == ee::kOpBlock)
            {
                Op o;
                std::string err;
                if (parse(*p, o, err))
                {
                    libs_.pin.waits = true;
                    libs_.pin.token = o.wide;
                }
            }
        }
        libs_.snd.fromLog = opt_.sndFromLog;
        if (f.logReply && opt_.pinned)
        {
            // The reference's reply words, for what its IOP time decided (libraries.md section 15.5): the log's runs,
            // else the memory as it was (the reference's call left those bytes as they were).
            const std::vector<Run> *runs = &c.runs;
            const uint8_t *flat = d_->runtime().memory.flat();
            libs_.pin.logWord = [runs, flat](uint32_t address, uint32_t &value) {
                const int64_t o = GuestMemory::flatOffset(address, 4);
                if (o < 0)
                    return false;
                uint8_t b[4];
                for (uint32_t i = 0; i < 4; ++i)
                {
                    const uint32_t at = static_cast<uint32_t>(o) + i;
                    b[i] = flat[at];
                    for (const Run &run : *runs)
                        if (at >= run.address && at < run.address + run.length)
                            b[i] = run.data[at - run.address];
                }
                std::memcpy(&value, b, 4);
                return true;
            };
        }
        try
        {
            f.handler(libs_, g);
        }
        catch (const libs::BadAccess &b)
        {
            diff += std::format(" an access to 0x{:x} ({} bytes) outside guest memory;", b.address, b.bytes);
        }
        ContextWords now;
        toWords(*ctx, status, now);
        std::map<uint32_t, uint32_t> logged(c.words.begin(), c.words.end());
        for (uint32_t w = 0; w < kContextWords; ++w)
        {
            if (w == kWordPc)
                continue;
            auto it = logged.find(w);
            const uint32_t want = it != logged.end() ? it->second : entry[w];
            if (now[w] != want)
                diff += std::format(" w{} {:x} (log {:x})", w, now[w], want);
        }
        if (auto it = logged.find(kWordPc); it != logged.end() && it->second != c.ra)
            diff += std::format(" the log's call returns to 0x{:x}, not its ra 0x{:x};", it->second, c.ra);
        diff += compareMemory(c.runs);
        libs_.pin = libs::Pin{};
        if (f.timed)
        {
            // Free mode: the library's own drive time, reported when it is not the reference's (the log's is kept).
            if (g.cycles != logCycles)
                ++native_.timeDiffers;
        }
        else if (logCycles != 0 && !f.frameBoundary)
            diff += std::format(" the log's call takes {} EE cycles (native: 0);", logCycles);
        if (&f == &libs::Libraries::ioFunction())
        {
            // A hardware register: a load's value, and the timer-poll cycles a poll leaves pending.
            const libs::IoAccess &a = libs_.ioAccess;
            const uint64_t logged = c.value[0] | (static_cast<uint64_t>(c.value[1]) << 32);
            if (!a.store && a.value != logged)
                diff += std::format(" loads {:x} (log {:x});", a.value, logged);
            if (a.timerPending != c.timerAfter)
                diff += std::format(" leaves {} timer-poll cycles pending (log {});", a.timerPending, c.timerAfter);
        }
        else if (c.timerAfter != c.timerBefore)
            diff += " the log's call changes the pending timer-poll cycles;";
        if (!c.traces.empty())
            diff += std::format(" the log's call made {} traced calls;", c.traces.size());
        unsigned bit = 0;
        while (bit < 15 && !(f.library & (1u << bit)))
            ++bit;
        ++native_.calls[bit];
        if (!diff.empty())
        {
            ++native_.divergences;
            diverge(std::format("native {} ({}) differs from the log:{}", f.name, libs::libraryName(f.library), diff), &r, true);
            // Going on (--max-errors): the log's effect replaces the native one.
            uint8_t *flat = d_->runtime().memory.flat();
            for (size_t i = journal_.entries.size(); i-- > 0;)
            {
                const libs::Journal::Entry &e = journal_.entries[i];
                std::memcpy(flat + e.flat, journal_.old.data() + e.oldAt, e.length);
            }
            for (uint32_t w = 0; w < kContextWords; ++w)
                if (w != kWordPc)
                    setContextWord(*ctx, status, w, entry[w]);
            applyWords(ctx, c);
            applyRuns(c.runs);
        }
    }

    std::string LogHost::compareMemory(const std::vector<Run> &runs)
    {
        const uint8_t *flat = d_->runtime().memory.flat();
        for (const Run &run : runs)
            if (std::memcmp(flat + run.address, run.data, run.length) != 0)
            {
                uint32_t i = 0;
                while (flat[run.address + i] == run.data[i])
                    ++i;
                return std::format(" memory 0x{:x}: {:02x} (log {:02x}, run 0x{:x} +{});", run.address + i, flat[run.address + i],
                                   run.data[i], run.address, run.length);
            }
        // Bytes the native call changed that the log's call did not: each written range's bytes as they were before
        // the call (an earlier write to the same bytes holds their older value) against now, outside the log's runs.
        // The runs as sorted spans, searched by address (a picture's write meets thousands of runs); runs of one
        // record never overlap.
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        spans.reserve(runs.size());
        for (const Run &run : runs)
            spans.emplace_back(run.address, run.address + run.length);
        std::sort(spans.begin(), spans.end());
        auto covered = [&](uint32_t a) {
            auto it = std::upper_bound(spans.begin(), spans.end(), std::make_pair(a, ~0u));
            return it != spans.begin() && a < std::prev(it)->second;
        };
        const auto &entries = journal_.entries;
        std::vector<uint8_t> before;
        for (size_t k = 0; k < entries.size(); ++k)
        {
            const libs::Journal::Entry &e = entries[k];
            before.assign(journal_.old.begin() + static_cast<std::ptrdiff_t>(e.oldAt),
                          journal_.old.begin() + static_cast<std::ptrdiff_t>(e.oldAt + e.length));
            for (size_t j = 0; j < k; ++j)
            {
                const libs::Journal::Entry &p = entries[j];
                const uint32_t lo = std::max(p.flat, e.flat), hi = std::min(p.flat + p.length, e.flat + e.length);
                for (uint32_t a = lo; a < hi; ++a)
                    before[a - e.flat] = journal_.old[p.oldAt + (a - p.flat)];
            }
            for (uint32_t i = 0; i < e.length; ++i)
                if (flat[e.flat + i] != before[i] && !covered(e.flat + i))
                    return std::format(" memory 0x{:x}: {:02x} (log unchanged {:02x});", e.flat + i, flat[e.flat + i], before[i]);
        }
        return "";
    }

    void LogHost::observe(R5900Context *ctx, const Record &r, const Call &c)
    {
        std::string why;
        for (const Trace &t : c.traces)
        {
            auto it = idNames_.find(t.id);
            if (it == idNames_.end() || !libs::Libraries::follows(it->second))
                continue;
            const libs::Function *f = libs_.find(it->second);
            if (!f || !(f->library & opt_.native))
                continue;
            ++native_.observed;
            if (!libs_.observeTraced(it->second, t.a, t.v0, why))
            {
                ++native_.divergences;
                diverge("native state: " + why, &r, true);
            }
        }
        // The 989SND driver follows the RPC calls made inside a call answered from the log (port overrides: the movie's
        // sound, the menus and TheGame::Update), and the SIF DMA ids those calls took (libraries.md section 15.7).
        if (opt_.native & (libs::kSnd | libs::kSif))
        {
            std::vector<std::array<uint32_t, 4>> rpc;
            for (const Trace &t : c.traces)
            {
                auto it = idNames_.find(t.id);
                if (it == idNames_.end())
                    continue;
                if (it->second == "sceSifCallRpc")
                    rpc.push_back({t.a[0], t.a[1], t.a[2], t.a[3]});
                else if (it->second == "sceSifSetDma" && (opt_.native & libs::kSif))
                    libs_.sif.dmaId = t.v0;
            }
            if (!rpc.empty() && (opt_.native & libs::kSnd))
            {
                libs::Guest g(d_->runtime(), ctx);
                libs_.snd.followTraced(libs_, g, rpc);
                ++native_.observed;
            }
            // sceSifCallRpc itself answered from the log (the SIF layer not native): the driver follows it.
            if (c.kind == kStub && (opt_.native & libs::kSnd) && !(opt_.native & libs::kSif))
                if (auto n = names_.find(c.target); n != names_.end() && n->second.name == "sceSifCallRpc" &&
                                                    c.value[0] && libs_.sndNative)
                {
                    libs::Guest g(d_->runtime(), ctx);
                    const uint32_t send = c.value[3], ssize = c.t[0], recv = c.t[1], rsize = c.t[2];
                    libs_.snd.noteBuffers(send, recv);
                    std::vector<uint32_t> request((ssize + 3) / 4), reply;
                    for (size_t i = 0; send && i < request.size(); ++i)
                        request[i] = g.read32(send + static_cast<uint32_t>(4 * i));
                    libs::SndDriver::LogWord memory = [&g](uint32_t a, uint32_t &v) {
                        v = g.read32(a);
                        return true;
                    };
                    libs_.snd.call(libs_, g, c.value[1], request, recv, rsize, reply, &memory, true);
                    ++native_.observed;
                }
        }
        // A library answered from the log that takes from the native heap (libraries.md section 2.2).
        if (c.kind == kStub && (opt_.native & libs::kLibc))
            if (auto n = names_.find(c.target); n != names_.end() && n->second.name == "sceMpegCreate")
            {
                libs::Guest g(d_->runtime(), ctx);
                ++native_.observed;
                if (!libs_.observeAnswered(n->second.name, g, why))
                {
                    ++native_.divergences;
                    diverge("native state: " + why, &r, true);
                }
            }
    }

    // ---- Kernel calls ------------------------------------------------------------------------------------------

    void LogHost::kernelCallBegin(ee::Dispatcher &d, R5900Context *ctx, uint32_t target, uint32_t kind)
    {
        Call &c = kc_.call;
        const Record r = takeCall(static_cast<uint8_t>(kind), target, ctx, c);
        uint32_t want[4];
        if (kind == kSyscall)
        {
            want[0] = gpr(ctx, 3);
            want[1] = gpr(ctx, 4);
            want[2] = gpr(ctx, 5);
            want[3] = gpr(ctx, 6);
        }
        else
            for (int k = 0; k < 4; ++k)
                want[k] = gpr(ctx, 4 + k);
        for (int k = 0; k < 4; ++k)
            if (want[k] != c.value[k])
            {
                diverge(std::format("{} 0x{:x}: values {:x} {:x} {:x} {:x}, the log has {:x} {:x} {:x} {:x}", kindName(kind),
                                    target, want[0], want[1], want[2], want[3], c.value[0], c.value[1], c.value[2], c.value[3]),
                        &r, true);
                break;
            }
        log_.advance();
        ++matched_;
        trace(r);
        kc_.active = true;
        kc_.tail = d.runtime().tailCall;
        d.runtime().tailCall = false;
        kc_.seq = seq_++;
        kc_.target = target;
        kc_.kind = kind;
        kc_.flags = r.flags;
        kc_.recordIndex = r.index;
        std::memcpy(kc_.args, want, sizeof(want));
        toWords(*ctx, d.runtime().cop0Status, kc_.entry);
        blockedInCall_ = false;
        ++kernelCalls_;
        d.kernel().setOrigin(ee::kOriginCall, kc_.seq);
    }

    void LogHost::kernelCallEnd(ee::Dispatcher &d, R5900Context *ctx, uint32_t target, bool stopping)
    {
        const Call &c = kc_.call;
        if (const Record *p = log_.peek(); p && p->inside() && p->seq == kc_.seq)
            diverge(std::format("kernel call 0x{:x}: the log has more inside it", target), p);
        ContextWords now;
        toWords(*ctx, d.runtime().cop0Status, now);
        std::map<uint32_t, uint32_t> logged(c.words.begin(), c.words.end());
        std::string diff;
        for (uint32_t w = 0; w < kContextWords; ++w)
        {
            if (w == kWordPc)
                continue;
            auto it = logged.find(w);
            const uint32_t want = it != logged.end() ? it->second : kc_.entry[w];
            if (now[w] != want)
                diff += std::format(" w{} {:x} (log {:x})", w, now[w], want);
        }
        Record fake;
        fake.index = kc_.recordIndex;
        if (!diff.empty())
        {
            diverge(std::format("kernel call 0x{:x} ({}): context differs:{}", target, names_.count(target) ? names_[target].name : "syscall",
                                diff),
                    nullptr, true);
            for (const auto &[w, v] : c.words)
                if (w != kWordPc)
                    setContextWord(*ctx, d.runtime().cop0Status, w, v);
        }
        if (!c.runs.empty())
            diverge(std::format("kernel call 0x{:x}: the log's call wrote memory ({} runs)", target, c.runs.size()), nullptr, true);
        if (c.cycleAfter != d.now(ctx))
            diverge(std::format("kernel call 0x{:x}: ends at cycle {}, the log's at {}", target, d.now(ctx), c.cycleAfter),
                    nullptr, true);
        const bool unwound = (kc_.flags & kUnwound) != 0;
        if (unwound != stopping)
            diverge(std::format("kernel call 0x{:x}: {} here, the log's {}", target, stopping ? "left to the dispatcher" : "returned",
                                unwound ? "left" : "returned"),
                    nullptr);
        const uint32_t id = kc_.tail ? 0 : traceId(static_cast<uint8_t>(kc_.kind == kSyscall ? kSyscall : kStub), target, ctx);
        if (id)
        {
            uint32_t args4[4];
            for (int k = 0; k < 4; ++k)
                args4[k] = kc_.entry[gprWord(4 + k)];
            calls_.add(id, args4, blockedInCall_ ? 0 : gpr(ctx, 2), blockedInCall_ ? reference::CallList::kBlocked : 0);
        }
        kc_.active = false;
    }

    // ---- The dispatcher's records ------------------------------------------------------------------------------

    void LogHost::invoked(ee::Dispatcher &d, const ee::Thread &t, const ee::Frame &f, bool insideCall)
    {
        const Record &r = next("INVOKE");
        std::string err;
        if (r.type != kInvoke)
            diverge(std::format("a handler 0x{:x} (kind {}) starts on thread {}; the log has a {} record", f.pc, f.kind, t.id,
                                typeName(r.type)),
                    &r);
        Invoke v;
        parse(r, v, err);
        std::string diff;
        if (r.sub != f.kind) diff += std::format(" kind {} (log {})", f.kind, r.sub);
        if (v.thread != t.id) diff += std::format(" thread {} (log {})", t.id, v.thread);
        if (v.depth != t.depth()) diff += std::format(" depth {} (log {})", t.depth(), v.depth);
        if (v.pc != f.pc) diff += std::format(" pc {:x} (log {:x})", f.pc, v.pc);
        if ((v.flags & 1u) != (f.completion ? 1u : 0u)) diff += " completion";
        if (v.tag != f.tag) diff += std::format(" tag {:x} (log {:x})", f.tag, v.tag);
        if (v.sequence != f.sequence) diff += std::format(" sequence {} (log {})", f.sequence, v.sequence);
        for (int i = 0; i < 4; ++i)
            if (v.a[i] != f.a[i]) diff += std::format(" a{} {:x} (log {:x})", i, f.a[i], v.a[i]);
        if (v.gp != f.gp) diff += std::format(" gp {:x} (log {:x})", f.gp, v.gp);
        if (v.sp != f.sp) diff += std::format(" sp {:x} (log {:x})", f.sp, v.sp);
        if (v.ra != f.ra) diff += std::format(" ra {:x} (log {:x})", f.ra, v.ra);
        if (r.inside() != insideCall) diff += insideCall ? " (inside a call here)" : " (outside a call here)";
        if (!diff.empty())
            diverge("INVOKE differs:" + diff, &r);
        checkPoint(r, d.now(), "INVOKE");
        log_.advance();
        ++matched_;
        trace(r);
        if (f.kind != ee::kInvokeInterrupt)
            lenient_.insert({t.id, t.depth()});
    }

    void LogHost::invokeEnded(ee::Dispatcher &d, const ee::Thread &t, const ee::Frame &f)
    {
        const Record &r = next("INVOKE_END");
        if (r.type != kInvokeEnd)
            diverge(std::format("the handler 0x{:x} on thread {} returned; the log has a {} record", f.pc, t.id, typeName(r.type)),
                    &r);
        InvokeEnd v;
        std::string err;
        parse(r, v, err);
        const uint32_t v0 = f.ctx.r[2].uw[0], v1 = f.ctx.r[3].uw[0];
        if (r.sub != f.kind || v.thread != t.id || v.depth != t.depth() || v.v0 != v0 || v.v1 != v1 || v.sequence != f.sequence)
            diverge(std::format("INVOKE_END differs: kind {} thread {} depth {} v0 {:x} v1 {:x} sequence {}", f.kind, t.id,
                                t.depth(), v0, v1, f.sequence),
                    &r, true);
        checkPoint(r, d.now(), "INVOKE_END");
        log_.advance();
        ++matched_;
        trace(r);
    }

    void LogHost::switched(ee::Dispatcher &d, int32_t from, uint32_t fromDepth, int32_t to, uint32_t toDepth, uint16_t reason,
                           uint32_t waitReason, uint32_t waitObject, uint64_t token)
    {
        const Record &r = next("SWITCH");
        if (r.type != kSwitch)
            diverge(std::format("switch {}/{} -> {}/{} (reason {}); the log has a {} record", from, fromDepth, to, toDepth,
                                reason, typeName(r.type)),
                    &r);
        Switch s;
        std::string err;
        parse(r, s, err);
        bool same = s.to == to && s.toDepth == toDepth && r.sub == reason;
        if (reason != ee::kSwitchStart)
            same = same && s.from == from && s.fromDepth == fromDepth;
        if (reason == ee::kSwitchBlocked)
            same = same && s.waitReason == waitReason && s.waitObject == waitObject && s.token == token;
        if (!same)
            diverge(std::format("SWITCH differs: {}/{} -> {}/{} reason {} wait {} {} {:x}", from, fromDepth, to, toDepth, reason,
                                waitReason, waitObject, token),
                    &r);
        checkPoint(r, d.now(), "SWITCH");
        log_.advance();
        ++matched_;
        trace(r);
    }

    void LogHost::resuming(ee::Dispatcher &d, ee::Thread &t, ee::Frame &f, uint32_t depth)
    {
        ContextWords cur;
        toWords(f.ctx, f.status, cur);
        const Record *p = log_.peek();
        Ctx c;
        std::string err;
        bool have = false;
        Record r;
        if (p && p->type == kCtx && !p->inside())
        {
            r = *p;
            if (!parse(r, c, err))
                endRun(err);
            if (c.thread != t.id || c.depth != depth)
                diverge(std::format("guest code runs on {}/{}; the log's CTX is for {}/{}", t.id, depth, c.thread, c.depth), &r);
            have = true;
        }
        const bool lenient = lenient_.erase({t.id, depth}) != 0;
        std::map<uint32_t, uint32_t> logged(c.words.begin(), c.words.end());
        std::string diff;
        for (uint32_t w = 0; w < kContextWords; ++w)
        {
            const bool mine = cur[w] != f.last[w];
            auto it = logged.find(w);
            if (it != logged.end())
            {
                if (cur[w] != it->second && !lenient)
                    diff += std::format(" w{} {:x} (log {:x})", w, cur[w], it->second);
            }
            else if (mine)
                diff += std::format(" w{} {:x} (log unchanged {:x})", w, cur[w], f.last[w]);
        }
        if (!diff.empty())
            diverge(std::format("context of {}/{} as it resumes differs:{}", t.id, depth, diff), have ? &r : nullptr, true);
        if (have)
        {
            for (const auto &[w, v] : c.words)
                setContextWord(f.ctx, f.status, w, v);
            checkPoint(r, d.now(), "CTX");
            log_.advance();
            ++matched_;
            trace(r);
        }
    }

    void LogHost::continuation(uint32_t origin, uint32_t memSource, const char *what)
    {
        std::string err;
        for (;;)
        {
            const Record *p = log_.peek();
            if (!p || p->inside())
                return;
            const Record r = *p;
            if (r.type == kMem && r.sub == memSource)
            {
                Mem m;
                if (!parse(r, m, err))
                    endRun(err);
                checkPoint(r, d_->now(), what);
                applyRuns(m.runs);
                log_.advance();
                ++matched_;
                trace(r);
                continue;
            }
            if (r.type == kOp)
            {
                Op o;
                if (!parse(r, o, err))
                    endRun(err);
                if (o.origin != origin || o.depth != 0)
                    return;
                d_->kernel().replay(r.sub, o.args.data(), static_cast<uint32_t>(o.args.size()), o.wide);
                const Record *q = log_.peek();
                if (q && q->index == r.index)
                    diverge(std::format("the kernel did not make the log's {}", ee::opName(r.sub)), &r);
                continue;
            }
            return;
        }
    }

    void LogHost::waitCompleted(ee::Dispatcher &, ee::Thread &t)
    {
        if (auto run = libs_.takeContinuation(t.id))
        {
            nativeContinuation(t, std::move(run), ee::kOriginResume, "resume");
            return;
        }
        continuation(ee::kOriginResume, ee::kOriginResume, "resume");
        lenient_.insert({t.id, t.depth()});
    }

    void LogHost::nativeContinuation(ee::Thread &t, std::function<void(libs::Libraries &, libs::Guest &)> run,
                                     uint32_t memSource, const char *what)
    {
        // The log's memory changes of this continuation: the MEM records of its source at this point (before the
        // operations it makes, and after them), kept as owned copies.
        struct Owned
        {
            uint32_t address;
            std::vector<uint8_t> data;
        };
        std::vector<Owned> logged;
        auto take = [&] {
            std::string err;
            for (;;)
            {
                const Record *p = log_.peek();
                if (!p || p->inside() || p->type != kMem || p->sub != memSource)
                    return;
                const Record r = *p;
                Mem m;
                if (!parse(r, m, err))
                    endRun(err);
                checkPoint(r, d_->now(), what);
                for (const Run &run : m.runs)
                    logged.push_back({run.address, std::vector<uint8_t>(run.data, run.data + run.length)});
                log_.advance();
                ++matched_;
                trace(r);
            }
        };
        take();
        journal_.clear();
        libs::Guest g(d_->runtime(), &t.top().ctx, &journal_);
        std::string diff;
        try
        {
            run(libs_, g);
        }
        catch (const libs::BadAccess &b)
        {
            diff += std::format(" an access to 0x{:x} ({} bytes) outside guest memory;", b.address, b.bytes);
        }
        take();
        std::vector<Run> runs;
        for (const Owned &o : logged)
            runs.push_back({o.address, static_cast<uint32_t>(o.data.size()), o.data.data()});
        diff += compareMemory(runs);
        if (!diff.empty())
        {
            ++native_.divergences;
            diverge(std::format("a native continuation ({}) on thread {} differs from the log:{}", what, t.id, diff), nullptr, true);
            applyRuns(runs);
        }
    }

    void LogHost::handlerCompleted(ee::Dispatcher &, ee::Thread &t, const ee::Frame &f)
    {
        if (auto run = libs_.takeHandlerEnd(f.sequence))
        {
            nativeContinuation(t, std::move(run), ee::kOriginHandlerEnd, "handler-end");
            return;
        }
        continuation(ee::kOriginHandlerEnd, ee::kOriginHandlerEnd, "handler-end");
    }

    void LogHost::threadEnded(ee::Dispatcher &, ee::Thread &)
    {
        continuation(ee::kOriginThreadEnd, ee::kOriginThreadEnd, "thread-end");
    }

    void LogHost::fault(ee::Dispatcher &, R5900Context *, const std::string &what)
    {
        ++errors_;
        endRun("FAULT: " + what + "\n  here: " + where());
    }

    // ---- Pinned mode -------------------------------------------------------------------------------------------

    const Record *LogHost::nextEvent()
    {
        const Record *p = log_.peek();
        if (!p || p->type != kOp || p->inside())
            return nullptr;
        if (p->index != eventIndex_)
        {
            Op o;
            std::string err;
            eventIndex_ = p->index;
            eventIs_ = parse(*p, o, err) && o.origin == ee::kOriginEvents;
        }
        return eventIs_ ? p : nullptr;
    }

    bool LogHost::eventPinnedSoon(ee::Dispatcher &)
    {
        const Record *p = nextEvent();
        return p && p->seq == seq_;
    }

    bool LogHost::eventPinned(ee::Dispatcher &d, const R5900Context *ctx)
    {
        const Record *p = nextEvent();
        if (!p || p->seq != seq_)
            return false;
        if (p->index == armed_)
            return true; // already found due (guest code stopped for it; the dispatcher now takes it)
        const uint64_t n = d.checkpointsSince();
        if (n < p->checkpoints)
            return false;
        // The record carries the reference's context pc, which inside inner calls lags behind: it is the pc of a
        // checkpoint passed lately in the same inner calls (kernel.md section 4: match1p #18297743 at bspPlaneTest2Vec's
        // call records 0x201418; #18359662 at the inner call records 0x201660; #31601175 at bspPlaneTest2Vec's call,
        // after an inner call returning to 0x201490, records 0x201418).
        const bool pcOk = !ctx || ctx->pc == p->pc || (d.insideInnerCall(ctx) && d.passedLately(p->pc));
        if (n == p->checkpoints && pcOk)
        {
            ++pinned_.events;
            armed_ = p->index;
            return true;
        }
        // BozoEngine's count one ahead of the reference's in this stretch (a checkpoint the reference does not count,
        // kernel.md section 4): the event is taken at the next checkpoint, if it goes on at the logged pc.
        if (n == p->checkpoints + 1 && ctx && pcOk)
        {
            ++pinned_.events;
            ++pinned_.shifted;
            if (pinned_.shiftedRecords.size() < 16)
                pinned_.shiftedRecords.push_back(p->index);
            armed_ = p->index;
            return true;
        }
        if (n == p->checkpoints && ctx)
            return false; // wait one checkpoint: the pc may come there
        diverge(std::format("the log's event ({}) comes {} checkpoints after the last out-call at pc 0x{:x}; here {} have "
                            "passed, at pc 0x{:x}",
                            ee::opName(p->sub), p->checkpoints, p->pc, n, ctx ? ctx->pc : 0),
                p);
        return false;
    }

    // ---- Kernel operations -------------------------------------------------------------------------------------

    void LogHost::op(const ee::OpEvent &e)
    {
        const Record &r = next("OP");
        auto mine = [&] {
            std::string s = std::format("{} origin {} depth {} call {}:", ee::opName(e.op), e.origin, e.depth,
                                        static_cast<int64_t>(e.originCall));
            for (uint32_t i = 0; i < e.argc; ++i)
                s += std::format(" {:x}", e.args[i]);
            return s + std::format(" wide {:x} thread {}", e.wide, e.thread);
        };
        if (r.type != kOp)
            diverge(std::format("the kernel made {}; the log has a {} record", mine(), typeName(r.type)), &r);
        Op o;
        std::string err;
        if (!parse(r, o, err))
            endRun(err);
        bool same = r.sub == e.op && o.origin == e.origin && o.depth == e.depth && o.wide == e.wide &&
                    o.args.size() == e.argc && r.thread == e.thread;
        if (e.origin == ee::kOriginCall)
            same = same && o.originCall == e.originCall;
        for (uint32_t i = 0; same && i < e.argc; ++i)
            same = o.args[i] == e.args[i];
        if (!same)
            diverge("OP differs: the kernel made " + mine(), &r);
        checkPoint(r, d_->now(), "OP");
        // libmpeg (answered from the log) queueing the game's nodata callback: its decoder took the IPU's input.
        if (!(opt_.native & libs::kMpeg) && e.op == ee::kOpQueue && e.argc > 0 && e.args[0] == ee::kInvokeLibraryCall)
            libs_.io.ipuDemand = true;
        if (e.op == ee::kOpBlock && d_->currentThread() && static_cast<int32_t>(e.args[0]) == d_->currentThread()->id)
            blockedInCall_ = true;
        log_.advance();
        ++matched_;
        trace(r);
    }

    bool LogHost::dmacStatusSet(uint32_t cause)
    {
        if (opt_.native & libs::kIo)
            return cause < 10 && (libs_.io.stat & (1u << cause)) != 0;
        const Record *p = log_.peek();
        if (!p || p->type != kOp || p->sub != ee::kOpIrq)
            return false;
        Op o;
        std::string err;
        if (!parse(*p, o, err))
            return false;
        return o.depth == 1 && o.args.size() >= 2 && o.args[0] == 1 && o.args[1] == cause;
    }

    // ---- Frames ------------------------------------------------------------------------------------------------

    void LogHost::endFrame(const Record &r, const Frame &f, uint32_t openId, const uint32_t *openArgs)
    {
        if (f.ended != frame_)
            diverge(std::format("frame {} ends here; the log's FRAME is {}", frame_, f.ended), &r);
        d_->setNow(nullptr, r.cycle);
        applyRuns(f.runs);
        if (openId)
            calls_.add(openId, openArgs, 0, reference::CallList::kOpenAtEnd);
        addTraces(f.traces, 0);

        uint8_t *flat = d_->runtime().memory.flat();
        reference::FrameRecord fr;
        fr.frame = frame_;
        fr.thread = d_->kernel().running();
        fr.ram = hash::xxh64(flat, 0x2000000, 0);
        fr.scratchpad = hash::xxh64(flat + 0x2000000, 0x4000, 0);
        fr.calls = calls_.hash();
        fr.callCount = calls_.count();
        std::vector<int32_t> order;
        d_->kernel().runOrder(order);
        fr.threads = reference::threadsHash(order);
        // The frame ends inside an out-call, at the log's time there: BozoEngine's own is that plus how far its clock
        // was from the log's when the call was made (0 in free mode, where a difference ends the run).
        fr.eeCycle = static_cast<uint64_t>(static_cast<int64_t>(r.cycle) + callDrift_);
        if (writer_)
            writer_->write(fr);
        ++stats_.frames;
        if (ref_ && frame_ < ref_->size())
        {
            const reference::FrameRecord &x = (*ref_)[frame_];
            auto field = [&](bool same, uint32_t &count, int64_t &first, const char *name, uint64_t mine, uint64_t theirs) {
                if (same)
                    ++count;
                else if (first < 0)
                {
                    first = frame_;
                    if (!opt_.quiet)
                        std::printf("frame %u: %s differs (%016llx, the reference's %016llx)\n", frame_, name,
                                    static_cast<unsigned long long>(mine), static_cast<unsigned long long>(theirs));
                }
            };
            field(fr.ram == x.ram, stats_.ram, stats_.firstRam, "ram", fr.ram, x.ram);
            field(fr.scratchpad == x.scratchpad, stats_.scratchpad, stats_.firstScratchpad, "scratchpad", fr.scratchpad,
                  x.scratchpad);
            field(fr.calls == x.calls, stats_.calls, stats_.firstCalls, "calls", fr.calls, x.calls);
            if (fr.calls != x.calls && stats_.firstCalls == frame_ && !opt_.quiet)
                std::printf("  calls: %u here, %u in the reference\n", fr.callCount, x.callCount);
            field(fr.threads == x.threads, stats_.threads, stats_.firstThreads, "threads", fr.threads, x.threads);
            field(fr.eeCycle == x.eeCycle, stats_.eeCycle, stats_.firstEeCycle, "eeCycle", fr.eeCycle, x.eeCycle);
            if (fr.callCount == x.callCount)
                ++stats_.callCount;
        }
        calls_.clear();
        ++frame_;
        libs_.framesEnded = frame_;
        const uint32_t limit = opt_.frames ? opt_.frames : log_.header().frameLimit;
        if (limit && frame_ >= limit)
        {
            completed_ = true;
            endRun("");
        }
    }
}
