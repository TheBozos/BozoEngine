// BozoEngine's dispatcher. See bozo/ee/dispatcher.h and docs/specs/kernel.md.
#include "bozo/ee/dispatcher.h"

#include <cstdio>
#include <cstring>
#include <format>

namespace bozo::ee
{
    namespace
    {
        constexpr uint64_t kDispatchCycles = 8; // each time the dispatcher starts guest code (calllog.md section 2)
        constexpr uint64_t kTimeSlice = 65536;  // a thread's time slice against threads of its priority

        Dispatcher &self(Runtime *rt) { return *static_cast<Dispatcher *>(rt->services); }
        uint32_t arg(const R5900Context *ctx, int i) { return ctx->r[4 + i].uw[0]; }
        // A kernel call's result: v0 as a whole 128-bit register (its upper half cleared, as the reference's).
        void ret(R5900Context *ctx, int32_t v)
        {
            ctx->r[2].sd[0] = v;
            ctx->r[2].ud[1] = 0;
        }
    }

    KernelCall kernelCallByName(const std::string &full)
    {
        static const std::map<std::string, KernelCall> names = {
            {"InitThread", KernelCall::InitThread},
            {"CreateThread", KernelCall::CreateThread},
            {"StartThread", KernelCall::StartThread},
            {"ExitThread", KernelCall::ExitThread},
            {"ExitDeleteThread", KernelCall::ExitDeleteThread},
            {"TerminateThread", KernelCall::TerminateThread},
            {"iTerminateThread", KernelCall::iTerminateThread},
            {"DeleteThread", KernelCall::DeleteThread},
            {"ChangeThreadPriority", KernelCall::ChangeThreadPriority},
            {"iChangeThreadPriority", KernelCall::iChangeThreadPriority},
            {"RotateThreadReadyQueue", KernelCall::RotateThreadReadyQueue},
            {"iRotateThreadReadyQueue", KernelCall::iRotateThreadReadyQueue},
            {"ReleaseWaitThread", KernelCall::ReleaseWaitThread},
            {"iReleaseWaitThread", KernelCall::iReleaseWaitThread},
            {"GetThreadId", KernelCall::GetThreadId},
            {"ReferThreadStatus", KernelCall::ReferThreadStatus},
            {"iReferThreadStatus", KernelCall::iReferThreadStatus},
            {"SleepThread", KernelCall::SleepThread},
            {"WakeupThread", KernelCall::WakeupThread},
            {"iWakeupThread", KernelCall::iWakeupThread},
            {"CancelWakeupThread", KernelCall::CancelWakeupThread},
            {"iCancelWakeupThread", KernelCall::iCancelWakeupThread},
            {"SuspendThread", KernelCall::SuspendThread},
            {"iSuspendThread", KernelCall::iSuspendThread},
            {"ResumeThread", KernelCall::ResumeThread},
            {"iResumeThread", KernelCall::iResumeThread},
            {"CreateSema", KernelCall::CreateSema},
            {"DeleteSema", KernelCall::DeleteSema},
            {"iDeleteSema", KernelCall::iDeleteSema},
            {"SignalSema", KernelCall::SignalSema},
            {"iSignalSema", KernelCall::iSignalSema},
            {"WaitSema", KernelCall::WaitSema},
            {"PollSema", KernelCall::PollSema},
            {"iPollSema", KernelCall::iPollSema},
            {"ReferSemaStatus", KernelCall::ReferSemaStatus},
            {"iReferSemaStatus", KernelCall::iReferSemaStatus},
            {"AddIntcHandler", KernelCall::AddIntcHandler},
            {"AddIntcHandler2", KernelCall::AddIntcHandler2},
            {"RemoveIntcHandler", KernelCall::RemoveIntcHandler},
            {"AddDmacHandler", KernelCall::AddDmacHandler},
            {"AddDmacHandler2", KernelCall::AddDmacHandler2},
            {"RemoveDmacHandler", KernelCall::RemoveDmacHandler},
            {"EnableIntc", KernelCall::EnableIntc},
            {"DisableIntc", KernelCall::DisableIntc},
            {"EnableDmac", KernelCall::EnableDmac},
            {"DisableDmac", KernelCall::DisableDmac},
            {"iEnableIntc", KernelCall::iEnableIntc},
            {"iDisableIntc", KernelCall::iDisableIntc},
            {"iEnableDmac", KernelCall::iEnableDmac},
            {"iDisableDmac", KernelCall::iDisableDmac},
            {"FlushCache", KernelCall::FlushCache},
            {"iFlushCache", KernelCall::iFlushCache},
            {"EnableCache", KernelCall::EnableCache},
            {"DisableCache", KernelCall::DisableCache},
            {"EndOfHeap", KernelCall::EndOfHeap},
            {"GetMemorySize", KernelCall::GetMemorySize},
            {"GsGetIMR", KernelCall::GsGetIMR},
            {"GsPutIMR", KernelCall::GsPutIMR},
            {"iGsGetIMR", KernelCall::iGsGetIMR},
            {"iGsPutIMR", KernelCall::iGsPutIMR},
            {"SetGsCrt", KernelCall::SetGsCrt},
            {"ResetEE", KernelCall::ResetEE},
            {"GetOsdConfigParam", KernelCall::GetOsdConfigParam},
            {"SetOsdConfigParam", KernelCall::SetOsdConfigParam},
            {"GetOsdConfigParam2", KernelCall::GetOsdConfigParam2},
            {"SetOsdConfigParam2", KernelCall::SetOsdConfigParam2},
            {"SetVSyncFlag", KernelCall::SetVSyncFlag},
            {"SetAlarm", KernelCall::SetAlarm},
            {"iSetAlarm", KernelCall::iSetAlarm},
            {"ReleaseAlarm", KernelCall::ReleaseAlarm},
            {"iReleaseAlarm", KernelCall::iReleaseAlarm},
        };
        std::string name = full;
        if (name.rfind("ps2_", 0) == 0) // the reference's names for the kernel's own wrappers ("ps2__EnableIntc")
            name.erase(0, 4);
        while (!name.empty() && name[0] == '_')
            name.erase(0, 1);
        auto it = names.find(name);
        return it == names.end() ? KernelCall::None : it->second;
    }

    Dispatcher::Dispatcher(Host &host) : host_(host) { rt_.services = this; }

    void Dispatcher::setFunctions(const BozoFunctionEntry *table, size_t count, const BozoResumeEntry *resume,
                                  size_t resumeCount)
    {
        rt_.functions.setGenerated(table, count);
        rt_.functions.setResume(resume, resumeCount);
    }

    void Dispatcher::bindKernelCall(uint32_t address, KernelCall call)
    {
        kernelCalls_[address] = call;
        rt_.functions.setOverride(address, &Dispatcher::kernelEntry);
    }

    void Dispatcher::bindLibrary(uint32_t address) { rt_.functions.setOverride(address, &Dispatcher::libraryEntry); }

    void Dispatcher::boot(uint32_t entry, uint32_t sp) { kernel_.boot(entry, sp); }

    void Dispatcher::setNow(R5900Context *ctx, uint64_t cycle)
    {
        if (inGuest_ && (ctx || currentFrame_))
            (ctx ? ctx : &currentFrame_->ctx)->cycles = cycle;
        else
            cycle_ = cycle;
    }

    uint64_t Dispatcher::checkpointsSince() const
    {
        const uint64_t now = inGuest_ && currentFrame_ ? currentFrame_->ctx.checkpoints : checkpoints_;
        return now - outcallCheckpoints_;
    }

    void Dispatcher::markOutcallEnd(const R5900Context *ctx)
    {
        outcallCheckpoints_ = inGuest_ ? ctx->checkpoints : checkpoints_;
    }

    void Dispatcher::stop(R5900Context *ctx, uint32_t pc)
    {
        ctx->pc = pc;
        ++stops;
        throw StopGuest{};
    }

    bool Dispatcher::mustStop(uint64_t cycle) const
    {
        if (cycle >= kernel_.nextEvent())
            return true;
        if (current_ && current_->depth() == 0 && cycle >= sliceStart_ + kTimeSlice && kernel_.sliceContender())
            return true;
        // A queued handler or a thread switch waits while a handler runs (it starts when the handler has returned).
        return current_ && current_->depth() == 0 && (kernel_.handlerQueued() || kernel_.reschedulePending());
    }

    void Dispatcher::updateLimit(R5900Context *ctx)
    {
        if (traceCheckpoints || timerPending_ ||
            (current_ && current_->depth() == 0 && (kernel_.handlerQueued() || kernel_.reschedulePending())))
            ctx->cycleLimit = 0;
        else
        {
            ctx->cycleLimit = kernel_.nextEvent();
            if (current_ && current_->depth() == 0 && kernel_.sliceContender() && sliceStart_ + kTimeSlice < ctx->cycleLimit)
                ctx->cycleLimit = sliceStart_ + kTimeSlice;
        }
    }

    Frame &Dispatcher::stackOnCurrent(const QueuedHandler &h)
    {
        QueuedHandler q = h;
        q.sequence = kernel_.sequence();
        Frame &f = kernel_.stackOn(*current_, q);
        host_.invoked(*this, *current_, f, true);
        return f;
    }

    // ---- Out-calls ---------------------------------------------------------------------------------------------

    void Dispatcher::kernelEntry(uint8_t *, R5900Context *ctx, Runtime *rt)
    {
        Dispatcher &d = self(rt);
        const uint32_t target = ctx->pc;
        const KernelCall call = d.kernelCalls_[target];
        d.host_.kernelCallBegin(d, ctx, target, 1);
        const bool stopping = d.kernelCall(call, ctx);
        d.host_.kernelCallEnd(d, ctx, target, stopping);
        d.markOutcallEnd(ctx);
        if (stopping)
            d.stop(ctx, ctx->r[31].uw[0]);
        d.updateLimit(ctx);
    }

    void Dispatcher::libraryEntry(uint8_t *, R5900Context *ctx, Runtime *rt)
    {
        Dispatcher &d = self(rt);
        d.host_.libraryCall(d, ctx, ctx->pc);
        d.markOutcallEnd(ctx);
        d.updateLimit(ctx);
    }

    void Dispatcher::syscall(uint8_t *, R5900Context *ctx, Runtime *, uint32_t)
    {
        const uint32_t number = ctx->r[3].uw[0];
        const uint32_t at = ctx->pc + 4; // a syscall is known by its return address (the log's target)
        if (number != 60 && number != 61)
        {
            host_.fault(*this, ctx, std::format("syscall {} at 0x{:x}: not implemented", static_cast<int32_t>(number), at));
            return;
        }
        host_.kernelCallBegin(*this, ctx, at, 3);
        if (number == 60) // SetupThread(gp, stack, stack size, args, root): the main thread's stack
            ret(ctx, static_cast<int32_t>(kernel_.threadSetup(arg(ctx, 1), arg(ctx, 2), arg(ctx, 0))));
        else // SetupHeap(start, size): the heap starts at the end of the program, aligned to 16 bytes
            ret(ctx, static_cast<int32_t>((arg(ctx, 0) + 15u) & ~15u));
        host_.kernelCallEnd(*this, ctx, at, false);
        markOutcallEnd(ctx);
        updateLimit(ctx);
    }

    void Dispatcher::vu0CallMicroprogram(uint8_t *, R5900Context *ctx, Runtime *, uint32_t address)
    {
        host_.vu0Microprogram(*this, ctx, address);
        markOutcallEnd(ctx);
        updateLimit(ctx);
    }

    void Dispatcher::ioRead(R5900Context *ctx, Runtime *, uint32_t address, uint32_t size, uint64_t value[2])
    {
        host_.ioRead(*this, ctx, address, size, value);
        markOutcallEnd(ctx);
        updateLimit(ctx);
    }

    void Dispatcher::ioWrite(R5900Context *ctx, Runtime *, uint32_t address, uint32_t size, const uint64_t value[2])
    {
        host_.ioWrite(*this, ctx, address, size, value);
        markOutcallEnd(ctx);
        updateLimit(ctx);
    }

    bool Dispatcher::cop0Condition(R5900Context *ctx, Runtime *) { return host_.cop0Condition(*this, ctx); }

    void Dispatcher::unknownTarget(uint8_t *, R5900Context *ctx, Runtime *, uint32_t target)
    {
        host_.fault(*this, ctx, std::format("jump to 0x{:x}: no generated code and no override there", target));
    }
    void Dispatcher::resumeFailed(uint8_t *, R5900Context *ctx, Runtime *, uint32_t pc)
    {
        host_.fault(*this, ctx, std::format("resume at 0x{:x}: not a label of its function", pc));
    }
    void Dispatcher::breakpoint(uint8_t *, R5900Context *ctx, Runtime *, uint32_t code)
    {
        host_.fault(*this, ctx, std::format("break 0x{:x} at 0x{:x}", code, ctx->pc));
    }
    void Dispatcher::trap(uint8_t *, R5900Context *ctx, Runtime *)
    {
        host_.fault(*this, ctx, std::format("trap at 0x{:x}", ctx->pc));
    }
    void Dispatcher::unimplemented(uint8_t *, R5900Context *ctx, Runtime *, uint32_t word)
    {
        host_.fault(*this, ctx, std::format("untranslated instruction 0x{:08x} at 0x{:x}", word, ctx->pc));
    }
    void Dispatcher::eret(uint8_t *, R5900Context *ctx, Runtime *)
    {
        host_.fault(*this, ctx, std::format("eret at 0x{:x}", ctx->pc));
    }
    uint32_t Dispatcher::cop0Read(R5900Context *ctx, Runtime *, uint32_t reg)
    {
        host_.fault(*this, ctx, std::format("mfc0 {} at 0x{:x}", reg, ctx->pc));
        return 0;
    }
    void Dispatcher::cop0Write(R5900Context *ctx, Runtime *, uint32_t reg, uint32_t)
    {
        host_.fault(*this, ctx, std::format("mtc0 {} at 0x{:x}", reg, ctx->pc));
    }
    uint32_t Dispatcher::vu0ControlRead(R5900Context *ctx, Runtime *, uint32_t reg)
    {
        host_.fault(*this, ctx, std::format("cfc2 {} at 0x{:x}", reg, ctx->pc));
        return 0;
    }
    void Dispatcher::vu0ControlWrite(R5900Context *ctx, Runtime *, uint32_t reg, uint32_t)
    {
        host_.fault(*this, ctx, std::format("ctc2 {} at 0x{:x}", reg, ctx->pc));
    }

    // ---- Checkpoints ---------------------------------------------------------------------------------------------

    void Dispatcher::yield(uint8_t *, R5900Context *ctx, Runtime *)
    {
        if (traceCheckpoints)
        {
            static uint64_t last = 0;
            std::printf("    checkpoint -> 0x%x at cycle %llu (#%llu) +%lld\n", ctx->pc,
                        static_cast<unsigned long long>(ctx->cycles), static_cast<unsigned long long>(ctx->checkpoints),
                        static_cast<long long>(ctx->cycles - last));
            last = ctx->cycles;
        }
        if (timerPending_)
        {
            ctx->cycles += timerPending_;
            timerPending_ = 0;
        }
        if (mustStop(ctx->cycles))
        {
            // The end of a time slice: the thread gives way to the next one of its priority (no operation recorded).
            if (ctx->cycles < kernel_.nextEvent() && current_ && current_->depth() == 0 &&
                ctx->cycles >= sliceStart_ + kTimeSlice && kernel_.sliceContender())
                kernel_.deschedule(true);
            stop(ctx, ctx->pc);
        }
        updateLimit(ctx);
    }

    // A jump through a register goes back to the dispatcher, which runs on from the target (the jumping function's
    // frame and every frame below it are gone: their returns come back to the dispatcher too).
    bool Dispatcher::jump(uint8_t *, R5900Context *ctx, Runtime *, uint32_t target)
    {
        ++jumps;
        stop(ctx, target);
    }

    // ---- The dispatcher's loop -----------------------------------------------------------------------------------

    Thread *Dispatcher::choose()
    {
        for (;;)
        {
            // A handler interrupted on a thread of its own goes on first.
            for (auto &[id, t] : kernel_.threads())
                if (t->pseudo && t->frames.size() > 1)
                {
                    kernel_.setRunning(id);
                    return t.get();
                }
            // The thread to run: the scheduler's choice. A library's completion of its wait runs first (it may block
            // it again); then a queued handler is stacked on it (on a thread of its own if no thread is ready).
            const int32_t b = kernel_.best();
            Thread *t = b ? kernel_.thread(b) : nullptr;
            if (t)
            {
                kernel_.setRunning(b);
                if (t->completionDue)
                {
                    t->completionDue = false;
                    kernel_.setOrigin(kOriginResume);
                    host_.waitCompleted(*this, *t);
                    continue;
                }
            }
            if (kernel_.handlerQueued())
            {
                Thread *on = t ? t : &kernel_.pseudoThread();
                kernel_.setRunning(on->id);
                Frame &h = kernel_.stackHandler(*on);
                ++handlersRun;
                host_.invoked(*this, *on, h, false);
                return on;
            }
            return t;
        }
    }

    void Dispatcher::idleUntilEvent()
    {
        const uint64_t to = kernel_.nextEvent();
        host_.idle(*this, cycle_, to);
        if (to > cycle_)
            cycle_ = to;
    }

    void Dispatcher::run()
    {
        for (;;)
        {
            kernel_.setOrigin(kOriginEvents);
            kernel_.processEvents(cycle_);
            const uint64_t picks = kernel_.picks();
            Thread *t = choose();
            // A time slice runs from the decision that picked the thread out of the ready queue (a handler stacked on
            // the running thread does not start a new one).
            if (kernel_.picks() != picks)
                sliceStart_ = cycle_;
            if (!t)
            {
                idleUntilEvent();
                continue;
            }
            const uint64_t decided = cycle_;
            cycle_ += kDispatchCycles;
            ++checkpoints_;
            ++dispatches;
            // Starting guest code is a checkpoint: an event due by now is processed first, and the choice made again.
            if (kernel_.nextEvent() <= cycle_)
                continue;
            const uint32_t depth = t->depth();
            if (!started_ || t->id != lastThread_ || depth != lastDepth_)
            {

                uint16_t reason = kSwitchOther;
                uint32_t waitReason = 0, waitObject = 0;
                uint64_t token = 0;
                Thread *from = kernel_.thread(lastThread_);
                if (!started_)
                    reason = kSwitchStart;
                else if (lastHandlerEnded_ && t->id == lastThread_ && depth + 1 == lastDepth_)
                    reason = kSwitchHandlerEnd;
                else if (t->id == lastThread_ && depth == lastDepth_ + 1)
                    reason = kSwitchHandlerStart;
                else if (lastHandlerEnded_)
                    reason = kSwitchHandlerEnd;
                else if (!from)
                    reason = kSwitchDeleted;
                else
                    switch (from->state)
                    {
                    case ThreadState::Waiting:
                        reason = kSwitchBlocked;
                        waitReason = from->waitReason;
                        waitObject = from->waitObject;
                        token = from->waitToken;
                        break;
                    case ThreadState::Ready:
                    case ThreadState::Running: reason = kSwitchPreempted; break;
                    case ThreadState::Dormant: reason = kSwitchEnded; break;
                    case ThreadState::Suspended:
                    case ThreadState::WaitSuspended: reason = kSwitchSuspended; break;
                    }
                host_.switched(*this, started_ ? lastThread_ : 0, started_ ? lastDepth_ : 0, t->id, depth, reason,
                               waitReason, waitObject, token);
            }
            started_ = true;
            Frame &f = t->top();
            if (traceCheckpoints)
                std::printf("    dispatch -> thread %d depth %u pc 0x%x at cycle %llu\n", t->id, depth, f.ctx.pc,
                            static_cast<unsigned long long>(cycle_));
            host_.resuming(*this, *t, f, depth);
            runFrame(*t, f);
        }
    }

    void Dispatcher::enter(R5900Context *ctx)
    {
        const uint32_t pc = ctx->pc;
        if (RecompiledFunction o = rt_.functions.override(pc))
        {
            rt_.tailCall = true; // not a call made by guest code (the dispatcher runs the thread on at an out-call)
            o(rt_.memory.flat(), ctx, &rt_);
            rt_.tailCall = false;
            ctx->pc = ctx->r[31].uw[0]; // an out-call at the bottom returns to its caller
            return;
        }
        if (const BozoResumeEntry *e = rt_.functions.resumeEntry(pc))
        {
            e->resume(rt_.memory.flat(), ctx, &rt_);
            return;
        }
        host_.fault(*this, ctx, std::format("no code at 0x{:x}", pc));
    }

    void Dispatcher::runFrame(Thread &t, Frame &f)
    {
        current_ = &t;
        currentFrame_ = &f;
        R5900Context *ctx = &f.ctx;
        rt_.cop0Status = f.status;
        ctx->cycles = cycle_;
        ctx->checkpoints = checkpoints_;
        inGuest_ = true;
        lastHandlerEnded_ = false;
        bool ended = false;
        try
        {
            updateLimit(ctx);
            // Guest code runs until it stops or the bottom function returns: then into a function whose frame is gone
            // (the dispatcher goes on there, at ctx->pc, as after any stop), or to 0 (the thread or handler ends).
            enter(ctx);
            ended = ctx->pc == 0;
        }
        catch (const StopGuest &)
        {
        }
        inGuest_ = false;
        cycle_ = ctx->cycles;
        checkpoints_ = ctx->checkpoints;
        f.status = rt_.cop0Status;
        f.ran = true;
        toWords(f.ctx, f.status, f.last);
        lastThread_ = t.id;
        lastDepth_ = t.depth();
        current_ = nullptr;
        currentFrame_ = nullptr;
        if (!ended)
            return;
        if (t.depth() > 0)
        {
            host_.invokeEnded(*this, t, f);
            if (f.completion)
            {
                kernel_.setOrigin(kOriginHandlerEnd);
                host_.handlerCompleted(*this, t, f);
            }
            kernel_.endHandler(t);
            lastHandlerEnded_ = true;
        }
        else
        {
            kernel_.setOrigin(kOriginDispatcher);
            kernel_.exitThread(t.id, false);
            kernel_.setOrigin(kOriginThreadEnd);
            host_.threadEnded(*this, t);
        }
    }

    // ---- Kernel calls ------------------------------------------------------------------------------------------

    bool Dispatcher::kernelCall(KernelCall call, R5900Context *ctx)
    {
        Kernel &k = kernel_;
        uint8_t *ram = rt_.memory.ram();
        auto word = [&](uint32_t address) {
            uint32_t v = 0;
            if (uint8_t *p = rt_.memory.hostPointer(address, 4))
                std::memcpy(&v, p, 4);
            return v;
        };
        (void)ram;
        const int32_t a0 = static_cast<int32_t>(arg(ctx, 0));
        switch (call)
        {
        case KernelCall::InitThread: ret(ctx, 1); return false;
        case KernelCall::CreateThread:
        {
            // ee_thread_t: status, func, stack, stack_size, gp_reg, initial_priority, current_priority, attr, option.
            const uint32_t p = arg(ctx, 0);
            ret(ctx, k.createThread(word(p + 4), word(p + 8), word(p + 12), word(p + 20), word(p + 16), word(p + 28),
                                    word(p + 32)));
            return false;
        }
        case KernelCall::StartThread:
            ret(ctx, k.startThread(a0, arg(ctx, 1), false));
            return k.transfer(false);
        case KernelCall::TerminateThread: ret(ctx, k.terminateThread(a0, false, false)); return false;
        case KernelCall::iTerminateThread: ret(ctx, k.terminateThread(a0, false, true)); return false;
        case KernelCall::DeleteThread: ret(ctx, k.deleteThread(a0, false)); return false;
        case KernelCall::ExitThread:
        case KernelCall::ExitDeleteThread:
            k.exitThread(k.running(), call == KernelCall::ExitDeleteThread);
            return true;
        case KernelCall::ChangeThreadPriority:
            ret(ctx, k.changePriority(a0, arg(ctx, 1), false));
            return k.transfer(false);
        case KernelCall::iChangeThreadPriority: ret(ctx, k.changePriority(a0, arg(ctx, 1), true)); return false;
        case KernelCall::RotateThreadReadyQueue:
            ret(ctx, k.rotateReadyQueue(arg(ctx, 0), false));
            return k.transfer(false);
        case KernelCall::iRotateThreadReadyQueue: ret(ctx, k.rotateReadyQueue(arg(ctx, 0), true)); return false;
        case KernelCall::GetThreadId: ret(ctx, k.running()); return false;
        case KernelCall::CreateSema:
        {
            // ee_sema_t: count, max_count, init_count, wait_threads, attr, option.
            const uint32_t p = arg(ctx, 0);
            ret(ctx, k.createSema(static_cast<int32_t>(word(p + 8)), static_cast<int32_t>(word(p + 4)), word(p + 16),
                                  word(p + 20)));
            return false;
        }
        case KernelCall::DeleteSema:
            ret(ctx, k.deleteSema(a0, false));
            return k.transfer(false);
        case KernelCall::iDeleteSema: ret(ctx, k.deleteSema(a0, true)); return false;
        case KernelCall::SignalSema:
            ret(ctx, k.signalSema(a0, false));
            return k.transfer(false);
        case KernelCall::iSignalSema: ret(ctx, k.signalSema(a0, true)); return false;
        case KernelCall::WaitSema:
        {
            const int32_t id = k.thread(k.running()) ? a0 : a0;
            ret(ctx, k.waitSema(id));
            return k.running() == 0;
        }
        case KernelCall::PollSema:
        case KernelCall::iPollSema: ret(ctx, k.pollSema(a0)); return false;
        case KernelCall::AddIntcHandler:
        case KernelCall::AddIntcHandler2:
            ret(ctx, k.addHandler(false, arg(ctx, 0), arg(ctx, 1), static_cast<int32_t>(arg(ctx, 2)), arg(ctx, 3),
                                  ctx->r[28].uw[0], ctx->r[29].uw[0]));
            return false;
        case KernelCall::AddDmacHandler:
        case KernelCall::AddDmacHandler2:
            ret(ctx, k.addHandler(true, arg(ctx, 0), arg(ctx, 1), static_cast<int32_t>(arg(ctx, 2)), arg(ctx, 3),
                                  ctx->r[28].uw[0], ctx->r[29].uw[0]));
            return false;
        case KernelCall::RemoveIntcHandler: ret(ctx, k.removeHandler(false, arg(ctx, 0), static_cast<int32_t>(arg(ctx, 1)))); return false;
        case KernelCall::RemoveDmacHandler: ret(ctx, k.removeHandler(true, arg(ctx, 0), static_cast<int32_t>(arg(ctx, 1)))); return false;
        case KernelCall::EnableIntc:
        case KernelCall::iEnableIntc: ret(ctx, k.causeEnable(false, arg(ctx, 0), true)); return false;
        case KernelCall::DisableIntc:
        case KernelCall::iDisableIntc: ret(ctx, k.causeEnable(false, arg(ctx, 0), false)); return false;
        case KernelCall::EnableDmac:
        case KernelCall::iEnableDmac: ret(ctx, k.causeEnable(true, arg(ctx, 0), true)); return false;
        case KernelCall::DisableDmac:
        case KernelCall::iDisableDmac: ret(ctx, k.causeEnable(true, arg(ctx, 0), false)); return false;
        case KernelCall::FlushCache:
        case KernelCall::iFlushCache: ret(ctx, 0); return false;
        case KernelCall::EnableCache:
        case KernelCall::DisableCache: ret(ctx, 0); return false;
        case KernelCall::GetMemorySize: ret(ctx, 0x2000000); return false;
        default:
            host_.fault(*this, ctx, std::format("kernel call {} at 0x{:x}: not implemented yet", static_cast<int>(call), ctx->pc));
            return false;
        }
    }
}
