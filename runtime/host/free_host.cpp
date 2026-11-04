// The free host. See bozo/host/free_host.h.
#include "bozo/host/free_host.h"

#include "bozo/app/determinism.h"
#include "bozo/hash.h"
#include "bozo/ops_mem.h"
#include "bozo/render/renderer.h"

#include "elf.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace bozo::host
{
    namespace
    {
        uint32_t gpr(const R5900Context *ctx, int n) { return ctx->r[n].uw[0]; }
    }

    FreeHost::FreeHost(const FreeOptions &options) : opt_(options) {}

    bool FreeHost::setUp(ee::Dispatcher &d, const BozoExternalEntry *externals, size_t count, const port::Registry *registry,
                         std::string &error)
    {
        d_ = &d;
        // Stops jump back to the dispatcher (nothing on the way holds anything to destroy: dispatcher.h), and no frame
        // keeps the context words only the call-log host compares.
        d.jumpStops = true;
        d.keepStopWords = false;
        d.kernel().setObserver(this);
        d.runtime().port = this;
        libs_.attach(d);
        libs_.printOutput = opt_.printGameOutput;
        libs_.sndNative = true;
        // Free mode's IOP time (libraries.md 15.13c): the IOP reset where the game makes it, nothing pinned.
        libs_.snd.freeTiming = true;
        libs_.snd.timing = libs::snd989::Timing::freeRun(0);
        libs_.cd.imagePath = opt_.iso;
        libs_.mc.hostDirectory = opt_.cardDir;
        for (int player = 0; player < 4; ++player)
        {
            const std::string &path = player == 0 ? opt_.padScript : opt_.playerScripts[static_cast<size_t>(player - 1)];
            if (path.empty())
                continue;
            std::ifstream in(path, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!in || !padScript_.load(player, text, error))
            {
                error = "pad script " + path + ": " + (error.empty() ? "cannot read it" : error);
                return false;
            }
            libs_.pad.input = &padScript_;
        }

        ElfFile elf;
        if (!elf.load(opt_.elf))
        {
            error = opt_.elf + ": " + elf.error();
            return false;
        }
        // Guest memory as the reference's boot leaves it (the call logs' INIT): the ELF's segments, and two words of
        // the boot's own at 0x2f0 and 0x2f8.
        uint8_t *ram = d.runtime().memory.ram();
        const std::vector<uint8_t> &image = elf.image();
        if (elf.imageBase() + image.size() > kRamSize)
        {
            error = "the ELF's image does not fit in guest RAM";
            return false;
        }
        std::memcpy(ram + elf.imageBase(), image.data(), image.size());
        const uint32_t bootWords[2] = {0x01800000u, 0x00001F80u};
        std::memcpy(ram + 0x2F0, &bootWords[0], 4);
        std::memcpy(ram + 0x2F8, &bootWords[1], 4);

        for (const ElfSymbol &s : elf.symbols())
            if (s.type == 2 && s.value != 0 && !s.name.empty())
                names_.emplace(s.value, s.name);
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t a = externals[i].address;
            if (a == 0)
                continue;
            if (registry)
                if (port::Function fn = registry->find(a))
                {
                    portAt_[a] = fn;
                    d.bindLibrary(a);
                    continue;
                }
            auto n = names_.find(a);
            const std::string name = n != names_.end() ? n->second : std::string();
            if (const ee::KernelCall k = ee::kernelCallByName(name); k != ee::KernelCall::None)
            {
                d.bindKernelCall(a, k);
                continue;
            }
            // A library BozoEngine implements; anything else (an override address the port registered nothing at, a
            // stub nothing native answers) stays the game's own code there.
            if (const libs::Function *f = libs_.find(name))
            {
                d.bindLibrary(a);
                nativeAt_[a] = f;
            }
        }
        uint32_t entry = 0;
        std::memcpy(&entry, elf.bytes().data() + 24, 4);
        d.boot(entry, 0x1FFFFF0u);
        if (!opt_.out.empty())
        {
            reference::Header h;
            h.elfHash = hash::xxh64(elf.bytes().data(), elf.bytes().size(), 0);
            h.run = "free";
            h.build = "BozoEngine free host";
            h.elfName = "SCUS_971.01";
            writing_ = writer_.open(opt_.out, h);
        }
        // The draw lists' digest is the record's `draws`: not computed when no record is written.
        render::Renderer::instance().setDrawDigest(writing_);
        return true;
    }

    uint64_t FreeHost::run()
    {
        try
        {
            d_->run();
        }
        catch (const ee::EndRun &)
        {
        }
        if (writing_)
            writer_.close();
        return stats_.frames;
    }

    void FreeHost::fault(ee::Dispatcher &, R5900Context *ctx, const std::string &what)
    {
        endReason_ = std::format("FAULT: {} (frame {}, pc 0x{:x})", what, stats_.frames, ctx ? ctx->pc : 0);
        throw ee::EndRun{};
    }

    // ---- Out-calls ---------------------------------------------------------------------------------------------

    void FreeHost::runOverride(R5900Context *ctx, uint32_t target, port::Function fn)
    {
        ++stats_.overrideCalls;
        ++perOverride_[target];
        const uint32_t ra = gpr(ctx, 31);
        const ee::Thread *self = d_->currentThread();
        const int32_t selfId = self ? self->id : 0;
        const bool outer = insideDepth_ == 0;
        ++insideDepth_;
        d_->beginOutcall(ctx);
        ctx->pc = target;
        fn(d_->runtime().memory.flat(), ctx, &d_->runtime());
        d_->endOutcall(ctx);
        --insideDepth_;
        if (!outer)
            return; // a stop deferred inside reaches the outermost override
        uint32_t pc = ctx->pc;
        const bool deferred = d_->takeDeferredStop(ctx, pc);
        d_->markOutcallEnd(ctx);
        // It leaves to the dispatcher when it stopped, when its thread no longer runs, or when it goes on elsewhere than
        // its return address (abi.md section 3: the dispatcher runs the thread on from there).
        if (deferred || d_->kernel().running() != selfId || pc != ra)
            d_->stop(ctx, pc);
    }

    void FreeHost::libraryCall(ee::Dispatcher &, R5900Context *ctx, uint32_t target)
    {
        watchdog(ctx);
        d_->runtime().tailCall = false;
        if (auto p = portAt_.find(target); p != portAt_.end())
        {
            runOverride(ctx, target, p->second);
            return;
        }
        auto n = nativeAt_.find(target);
        if (n == nativeAt_.end())
        {
            auto name = names_.find(target);
            ++stats_.unanswered[name != names_.end() ? name->second : std::format("0x{:x}", target)];
            fault(*d_, ctx, std::format("a call to 0x{:x} ({}) has no native answer", target,
                                        name != names_.end() ? name->second : "no symbol"));
            return;
        }
        ++stats_.libraryCalls;
        const libs::Function &f = *n->second;
        const ee::Thread *self = d_->currentThread();
        const int32_t selfId = self ? self->id : 0;
        const uint32_t ra = gpr(ctx, 31);
        libs_.pin = libs::Pin{};
        libs::Guest g(d_->runtime(), ctx);
        try
        {
            f.handler(libs_, g);
        }
        catch (const libs::BadAccess &b)
        {
            fault(*d_, ctx, std::format("native {}: an access to 0x{:x} ({} bytes) outside guest memory", f.name, b.address, b.bytes));
        }
        if (g.cycles)
            d_->setNow(ctx, d_->now(ctx) + g.cycles);
        if (f.frameBoundary)
            endFrame(ctx);
        const bool blocked = d_->kernel().running() != selfId;
        if (insideDepth_)
        {
            // Inside an override: guest code leaves it right there, the thread going on after the call once woken
            // (kernel.md section 3b).
            if (blocked)
                d_->leave(ctx, ra);
            return;
        }
        d_->markOutcallEnd(ctx);
        if (blocked)
            d_->stop(ctx, ra);
    }

    void FreeHost::vu0Microprogram(ee::Dispatcher &, R5900Context *ctx, uint32_t address)
    {
        ++stats_.vu0Calls;
        const libs::Function *f = libs_.vu0Program(address);
        if (!f)
        {
            fault(*d_, ctx, std::format("VU0 microprogram 0x{:x}: no native program", address));
            return;
        }
        libs::Guest g(d_->runtime(), ctx);
        f->handler(libs_, g);
    }

    void FreeHost::ioRead(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, uint64_t value[2])
    {
        ++stats_.ioAccesses;
        watchdog(ctx);
        value[0] = value[1] = 0;
        if (!libs::ioKnown(address, size, false))
        {
            ++stats_.ioUnknown;
            return;
        }
        libs_.ioAccess = libs::IoAccess{address, size, false, 0, d_->timerPending()};
        libs::Guest g(d_->runtime(), ctx);
        libs::Libraries::ioFunction().handler(libs_, g);
        d_->setTimerPending(libs_.ioAccess.timerPending);
        value[0] = libs_.ioAccess.value;
    }

    void FreeHost::ioWrite(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, const uint64_t value[2])
    {
        ++stats_.ioAccesses;
        if (!libs::ioKnown(address, size, true))
        {
            ++stats_.ioUnknown;
            return;
        }
        libs_.ioAccess = libs::IoAccess{address, size, true, value[0], d_->timerPending(), ctx->pc};
        libs::Guest g(d_->runtime(), ctx);
        libs::Libraries::ioFunction().handler(libs_, g);
        d_->setTimerPending(libs_.ioAccess.timerPending);
    }

    bool FreeHost::cop0Condition(ee::Dispatcher &, R5900Context *ctx)
    {
        watchdog(ctx);
        return libs_.io.cpcond0();
    }

    void FreeHost::watchdog(R5900Context *ctx)
    {
        if (stopRequested_.load(std::memory_order_relaxed))
        {
            endReason_ = "stopped";
            throw ee::EndRun{};
        }
        if (!opt_.stallFields)
            return;
        const uint64_t now = d_->now(ctx);
        if (now - lastFrameCycle_ > static_cast<uint64_t>(opt_.stallFields) * d_->kernel().field())
            fault(*d_, ctx, std::format("no game frame ended for {} fields: the main loop waits at 0x{:x} ({}), ra 0x{:x} ({})",
                                        opt_.stallFields, ctx ? ctx->pc : 0, ctx ? functionAt(ctx->pc) : "?",
                                        ctx ? ctx->r[31].uw[0] : 0, ctx ? functionAt(ctx->r[31].uw[0]) : "?"));
    }

    void FreeHost::kernelCallEnd(ee::Dispatcher &, R5900Context *, uint32_t target, bool)
    {
        ++stats_.kernelCalls;
        if (auto n = names_.find(target); n != names_.end() && n->second.find("FlushCache") != std::string::npos)
            libs_.io.flushCache();
    }

    bool FreeHost::dmacStatusSet(uint32_t cause) { return cause < 10 && (libs_.io.stat & (1u << cause)) != 0; }

    void FreeHost::waitCompleted(ee::Dispatcher &, ee::Thread &t)
    {
        if (auto run = libs_.takeContinuation(t.id))
        {
            libs::Guest g(d_->runtime(), &t.top().ctx);
            run(libs_, g);
        }
    }

    void FreeHost::handlerCompleted(ee::Dispatcher &, ee::Thread &t, const ee::Frame &f)
    {
        if (auto run = libs_.takeHandlerEnd(f.sequence))
        {
            libs::Guest g(d_->runtime(), &t.top().ctx);
            run(libs_, g);
        }
    }

    // ---- Frames ------------------------------------------------------------------------------------------------

    void FreeHost::frameEnd(Runtime *, R5900Context *ctx) { endFrame(ctx); }

    bool FreeHost::callLibrary(Runtime *, R5900Context *ctx, uint32_t address)
    {
        auto n = names_.find(address);
        const libs::Function *f = n != names_.end() ? libs_.find(n->second) : nullptr;
        if (!f)
            return false;
        ++stats_.libraryCalls;
        libs_.pin = libs::Pin{};
        libs::Guest g(d_->runtime(), ctx);
        f->handler(libs_, g);
        if (g.cycles)
            d_->setNow(ctx, d_->now(ctx) + g.cycles);
        ctx->pc = ctx->r[31].uw[0]; // it returned, as the function would have
        return true;
    }

    std::string FreeHost::functionAt(uint32_t pc) const
    {
        auto it = names_.upper_bound(pc);
        if (it == names_.begin())
            return "?";
        --it;
        return std::format("{}+0x{:x}", it->second, pc - it->first);
    }

    uint32_t FreeHost::symbol(const std::string &name) const
    {
        for (const auto &[a, n] : names_)
            if (n == name)
                return a;
        return 0;
    }

    // The frame's sound: the engine brought up to the frame's end on the EE clock, its samples since the last frame to
    // onAudio, their digest for the record.
    void FreeHost::frameSound(uint64_t &hash, uint32_t &count)
    {
        hash = 0;
        count = 0;
        if (!opt_.sound)
            return;
        libs_.snd.advanceAudio(libs_, d_->now());
        audio::Mixer *mixer = libs_.snd.mixer();
        if (!mixer)
            return;
        mixer->takeMixed(hash, count);
        audio_.resize(2u * audio::Mixer::kQueueFrames);
        const size_t n = mixer->take(audio_.data(), audio::Mixer::kQueueFrames);
        if (onAudio && n)
            onAudio(stats_.frames, audio_.data(), n);
    }

    void FreeHost::endFrame(const R5900Context *ctx)
    {
        const uint64_t frame = stats_.frames;
        lastFrameCycle_ = d_->now();
        if (writing_)
        {
            uint8_t *flat = d_->runtime().memory.flat();
            reference::FrameRecord fr;
            fr.frame = static_cast<uint32_t>(frame);
            fr.thread = d_->kernel().running();
            fr.ram = hash::xxh64(flat, 0x2000000, 0);
            fr.scratchpad = hash::xxh64(flat + 0x2000000, 0x4000, 0);
            std::vector<int32_t> order;
            d_->kernel().runOrder(order);
            fr.threads = reference::threadsHash(order);
            fr.eeCycle = d_->now();
            const render::FrameDraws draws = render::Renderer::instance().takeFrameDraws();
            fr.draws = draws.hash;
            fr.drawBatches = draws.batches;
            fr.drawVertices = draws.vertices;
            frameSound(fr.sound, fr.soundSamples);
            writer_.write(fr);
        }
        else
        {
            render::Renderer::instance().takeFrameDraws();
            uint64_t hash = 0;
            uint32_t count = 0;
            frameSound(hash, count);
        }
        ++stats_.frames;
        framesEnded_.store(stats_.frames, std::memory_order_relaxed);
        libs_.framesEnded = static_cast<uint32_t>(stats_.frames);
        det::frameCounter().store(stats_.frames, std::memory_order_relaxed);
        if (onFrame)
            onFrame(frame, ctx);
        if (opt_.frames && stats_.frames >= opt_.frames)
        {
            endReason_ = std::format("frame limit {} reached", opt_.frames);
            throw ee::EndRun{};
        }
        if (stopRequested_.load(std::memory_order_relaxed))
        {
            endReason_ = "stopped";
            throw ee::EndRun{};
        }
    }
}
