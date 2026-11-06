// BozoEngine's dispatcher: runs guest code on the kernel's threads and handlers, charges EE time (the reference's
// model, calllog.md section 2), processes the scheduler's events at checkpoints, and answers the kernel calls natively.
// Spec: docs/specs/kernel.md.
//
// Guest code stops (a thread switch, a handler, a blocking call) by unwinding to the dispatcher with the context's pc
// at the point it goes on from; the dispatcher later runs it on from there through the generated resume entries. So a
// thread's C++ frames never outlive a stop, exactly as the reference's runtime behaves (the cycle model depends on it:
// a return into a function whose frame is gone costs a dispatcher start).
//
// Everything the runtime does not implement natively yet (the libraries, the port's overrides, VU0 microprograms, I/O)
// goes to a Host. bozo-run's host answers them from a call log; the real runtime's will be the native libraries.
#pragma once

#include "bozo/ee/kernel.h"
#include "bozo/runtime.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bozo::ee
{
    class Dispatcher;

    // Thrown to leave guest code (caught by the dispatcher; never escapes it).
    struct StopGuest
    {
    };
    // Thrown by a host to end the run (a divergence, the end of a log).
    struct EndRun
    {
    };

    class Host
    {
    public:
        virtual ~Host() = default;
        // An out-call the runtime does not implement: a library function or a port override at ctx->pc (`target`),
        // reached by a call (or a jump) from guest code, or run by the dispatcher at the bottom of a thread.
        virtual void libraryCall(Dispatcher &, R5900Context *ctx, uint32_t target) = 0;
        virtual void vu0Microprogram(Dispatcher &, R5900Context *ctx, uint32_t address) = 0;
        virtual void ioRead(Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, uint64_t value[2]) = 0;
        virtual void ioWrite(Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, const uint64_t value[2]) = 0;
        virtual bool cop0Condition(Dispatcher &, R5900Context *) { return true; }
        // A syscall whose service is a native library function (`name`, kernel.md section 2): false when the host
        // has none (the dispatcher then faults).
        virtual bool librarySyscall(Dispatcher &, R5900Context *, const char * /*name*/) { return false; }
        // Around a native kernel call (kind 1: a kernel function at `target`; kind 3: a syscall at `target`).
        virtual void kernelCallBegin(Dispatcher &, R5900Context *, uint32_t /*target*/, uint32_t /*kind*/) {}
        virtual void kernelCallEnd(Dispatcher &, R5900Context *, uint32_t /*target*/, bool /*stopping*/) {}
        // The dispatcher's own decisions, as they are made.
        virtual void invoked(Dispatcher &, const Thread &, const Frame &, bool /*insideCall*/) {}
        virtual void invokeEnded(Dispatcher &, const Thread &, const Frame &) {}
        virtual void switched(Dispatcher &, int32_t /*from*/, uint32_t /*fromDepth*/, int32_t /*to*/, uint32_t /*toDepth*/,
                              uint16_t /*reason*/, uint32_t /*waitReason*/, uint32_t /*waitObject*/, uint64_t /*token*/) {}
        // Guest code is about to run on (thread, depth): `first` when it never ran there (or a new handler).
        virtual void resuming(Dispatcher &, Thread &, Frame &, uint32_t /*depth*/) {}
        // Library continuations the dispatcher runs: a wait's completion as its thread is picked (origin 5), a
        // handler's completion as it returns (origin 6), a thread's end (origin 8).
        virtual void waitCompleted(Dispatcher &, Thread &) {}
        virtual void handlerCompleted(Dispatcher &, Thread &, const Frame &) {}
        virtual void threadEnded(Dispatcher &, Thread &) {}
        // The dispatcher idles from `from` to `to` (every thread waiting).
        virtual void idle(Dispatcher &, uint64_t /*from*/, uint64_t /*to*/) {}
        // Pinned mode, inside a port's native override (kernel.md section 3b): the EE cycle from which guest code may
        // leave it (~0: it does not), and whether it leaves at this checkpoint (the reference's override ended there).
        virtual uint64_t outcallDeadline(Dispatcher &) { return ~0ull; }
        virtual bool outcallLeave(Dispatcher &, const R5900Context *) { return false; }
        // Pinned mode (Dispatcher::pinned, kernel.md section 4): whether the next asynchronous event (the vblank's
        // start or end) is due right here, at this checkpoint (`ctx`: guest code's context, null in the dispatcher's
        // loop); and whether one is due before the next out-call (every checkpoint then asks).
        virtual bool eventPinned(Dispatcher &, const R5900Context * /*ctx*/) { return false; }
        virtual bool eventPinnedSoon(Dispatcher &) { return false; }
        // A pc with neither code nor an override, a break, a trap...: report and end the run.
        virtual void fault(Dispatcher &, R5900Context *, const std::string &what) = 0;
    };

    // The kernel functions BozoEngine implements natively, by the game's symbol name (calllog.md section 6).
    enum class KernelCall : uint16_t
    {
        None,
        InitThread, CreateThread, StartThread, ExitThread, ExitDeleteThread, TerminateThread, iTerminateThread,
        DeleteThread, ChangeThreadPriority, iChangeThreadPriority, RotateThreadReadyQueue, iRotateThreadReadyQueue,
        ReleaseWaitThread, iReleaseWaitThread, GetThreadId, ReferThreadStatus, iReferThreadStatus, SleepThread,
        WakeupThread, iWakeupThread, CancelWakeupThread, iCancelWakeupThread, SuspendThread, iSuspendThread,
        ResumeThread, iResumeThread,
        CreateSema, DeleteSema, iDeleteSema, SignalSema, iSignalSema, WaitSema, PollSema, iPollSema, ReferSemaStatus,
        iReferSemaStatus,
        AddIntcHandler, AddIntcHandler2, RemoveIntcHandler, AddDmacHandler, AddDmacHandler2, RemoveDmacHandler,
        EnableIntc, DisableIntc, EnableDmac, DisableDmac, iEnableIntc, iDisableIntc, iEnableDmac, iDisableDmac,
        FlushCache, iFlushCache, EnableCache, DisableCache, EndOfHeap, GetMemorySize, GsGetIMR, GsPutIMR, iGsGetIMR,
        iGsPutIMR, SetGsCrt, ResetEE, GetOsdConfigParam, SetOsdConfigParam, GetOsdConfigParam2, SetOsdConfigParam2,
        SetVSyncFlag, SetAlarm, iSetAlarm, ReleaseAlarm, iReleaseAlarm, InitSys,
    };
    // The kernel call a symbol name stands for (a leading "ps2_" and leading "_" are ignored), None if it is not one.
    KernelCall kernelCallByName(const std::string &name);
    // The kernel call an EE syscall number stands for (v1 at the `syscall`, negative for the interrupt-safe entries:
    // kernel.md section 2), None if it is not one.
    KernelCall kernelCallBySyscall(int32_t number);
    // The native library function an EE syscall number stands for (the SIF DMA calls), null if none.
    const char *librarySyscall(int32_t number);

    class Dispatcher final : public RuntimeServices
    {
    public:
        explicit Dispatcher(Host &host);

        Runtime &runtime() { return rt_; }
        Kernel &kernel() { return kernel_; }
        Host &host() { return host_; }

        void setFunctions(const BozoFunctionEntry *table, size_t count, const BozoResumeEntry *resume, size_t resumeCount);
        // Binds a native kernel call at a guest address (the game's kernel stub there is not run).
        void bindKernelCall(uint32_t address, KernelCall call);
        // Binds the host's out-call handler at a guest address (a library function or a port override).
        void bindLibrary(uint32_t address);
        bool isKernelCall(uint32_t address) const { return kernelCalls_.count(address) != 0; }
        // The heap's start as SetupHeap (syscall 61) returned it (0 before): where the game's malloc starts.
        uint32_t heapStart() const { return heapStart_; }
        // The console's OSD settings GetOsdConfigParam answers (kernel.md section 2): English, 4:3, configuration
        // version 2 (a zeroed second block) until the game or the port sets them.
        uint32_t osdConfig = 0x00014010u;
        std::vector<uint8_t> osdConfig2 = std::vector<uint8_t>(0x100, 0);

        // The main thread starts at `entry` with sp `sp`; the dispatcher then runs until the host ends the run.
        void boot(uint32_t entry, uint32_t sp);
        void run();

        // ---- The native frame boundary (kernel.md section 2) ----
        // A game frame ended: SetVSyncFlag's words are written and the sync-V callback is queued. Nothing here follows
        // the field clock.
        void frameCompleted();
        // sceGsSyncVCallback: returns the callback it replaces (0: none; `pc` 0 removes it).
        uint32_t setSyncVCallback(uint32_t pc, uint32_t gp);

        // ---- EE time ----
        uint64_t cycle() const { return cycle_; }
        // The running guest code's clock (ctx->cycles while guest code runs; the dispatcher's otherwise).
        uint64_t now(const R5900Context *ctx) const { return inGuest_ ? ctx->cycles : cycle_; }
        uint64_t now() const { return inGuest_ && currentFrame_ ? currentFrame_->ctx.cycles : cycle_; }
        bool inGuest() const { return inGuest_; }
        void setNow(R5900Context *ctx, uint64_t cycle);
        uint32_t timerPending() const { return timerPending_; }
        void setTimerPending(uint32_t cycles) { timerPending_ = cycles; }
        uint64_t checkpointsSince() const; // checkpoints since the last out-call ended
        void markOutcallEnd(const R5900Context *ctx);

        // ---- What out-calls use ----
        Thread *currentThread() { return current_; }
        Frame *currentFrame() { return currentFrame_; }
        uint32_t currentDepth() const { return current_ ? current_->depth() : 0; }
        // Inside inner calls (kernel.md section 3): whether guest code is in one, and whether it passed a checkpoint
        // going on at `pc` lately (among the last 64 that asked the runtime: every one when traced, or in pinned mode
        // near an event).
        bool insideInnerCall(const R5900Context *ctx) const { return ctx && (ctx->noStop || ctx->inPlace); }
        bool passedLately(uint32_t pc) const
        {
            for (uint32_t p : recentPcs_)
                if (p == pc)
                    return true;
            return false;
        }
        // Leave guest code now: the current frame resumes at `pc` later.
        [[noreturn]] void stop(R5900Context *ctx, uint32_t pc);
        // After an out-call: the kernel's state may have changed (a handler queued, a thread readied).
        void updateLimit(R5900Context *ctx);

        // ---- Native out-calls that run guest code (a port's overrides: kernel.md section 3b) ----
        // Between beginOutcall and endOutcall the guest code an override calls charges EE time as usual, but no
        // checkpoint stops it and no event is taken (the reference: an override is one out-call, nothing happens inside
        // it but what it does). A stop it calls for through an out-call (a call that blocks, a kernel call that gives
        // way) is deferred: the context's pc is set to where the thread goes on, and guest code returns up to the
        // override (after a call, a pc other than the return address returns); the host stops the thread once the
        // override has returned. Overrides nest.
        void beginOutcall(R5900Context *ctx);
        void endOutcall(R5900Context *ctx);
        bool insideOutcall() const { return outcallDepth_ != 0; }
        // Guest code an override runs on a context of its own (a copy: TMB's TMB_KILL_ENEMIES calls the game's
        // vehicleTakeHit on one) is not the thread's: it cannot stop or leave (nothing would run it on from its pc),
        // so it runs to its return, no event taken and no stop made there (kernel.md section 3b).
        bool ownContext(const R5900Context *ctx) const
        {
            return outcallDepth_ && inGuest_ && currentFrame_ && ctx != &currentFrame_->ctx;
        }
        // Leave guest code at `pc`: inside an out-call deferred (returns), else stop (does not return).
        void leave(R5900Context *ctx, uint32_t pc);
        // The deferred stop, if any (taken: cleared).
        bool takeDeferredStop(R5900Context *ctx, uint32_t &pc);
        bool stopDeferred() const { return rt_.stopDeferred; }
        // A handler stacked by a library call on the current thread (the call is unwound right after).
        Frame &stackOnCurrent(const QueuedHandler &h);

        // ---- RuntimeServices ----
        void unknownTarget(uint8_t *, R5900Context *, Runtime *, uint32_t target) override;
        void resumeFailed(uint8_t *, R5900Context *, Runtime *, uint32_t pc) override;
        void yield(uint8_t *, R5900Context *, Runtime *) override;
        // A recursive call's or an inner call's return inside an override: guest code leaves there, but not on a
        // context of the override's own (kernel.md section 3b, "Guest code on an override's own context").
        void leaveAt(uint8_t *, R5900Context *ctx, Runtime *, uint32_t pc) override
        {
            if (!ownContext(ctx))
                leave(ctx, pc);
        }
        bool jump(uint8_t *, R5900Context *, Runtime *, uint32_t target) override;
        void syscall(uint8_t *, R5900Context *, Runtime *, uint32_t code) override;
        void breakpoint(uint8_t *, R5900Context *, Runtime *, uint32_t code) override;
        void trap(uint8_t *, R5900Context *, Runtime *) override;
        void unimplemented(uint8_t *, R5900Context *, Runtime *, uint32_t word) override;
        void eret(uint8_t *, R5900Context *, Runtime *) override;
        uint32_t cop0Read(R5900Context *, Runtime *, uint32_t reg) override;
        void cop0Write(R5900Context *, Runtime *, uint32_t reg, uint32_t value) override;
        bool cop0Condition(R5900Context *, Runtime *) override;
        void vu0CallMicroprogram(uint8_t *, R5900Context *, Runtime *, uint32_t address) override;
        uint32_t vu0ControlRead(R5900Context *, Runtime *, uint32_t reg) override;
        void vu0ControlWrite(R5900Context *, Runtime *, uint32_t reg, uint32_t value) override;
        void ioRead(R5900Context *, Runtime *, uint32_t address, uint32_t size, uint64_t value[2]) override;
        void ioWrite(R5900Context *, Runtime *, uint32_t address, uint32_t size, const uint64_t value[2]) override;

        // Debugging: every checkpoint calls yield and is printed (pc it goes on to, cycle).
        bool traceCheckpoints = false;
        // Pinned mode (kernel.md section 4): asynchronous events are taken where the host says (the reference's
        // points), not when the EE clock reaches them; everything else is decided as in free mode.
        bool pinned = false;
        // How a stop leaves guest code. Off: by throwing StopGuest, which unwinds every C++ frame between the stop and
        // the dispatcher (each one's destructors run). On: by a jump straight back to the dispatcher (__builtin_longjmp),
        // no frame unwound: a host may set it only when nothing between holds anything to destroy (the generated code,
        // the runtime's entries and its own out-call paths; a port's overrides never see a stop, it is deferred there:
        // kernel.md section 3b). The free host does: an exception's unwinding cost ~20 us a stop (the 1P match's
        // jumps through registers, dbsTraverse's table, stop a few hundred times a second).
        bool jumpStops = false;
        // Whether each frame keeps the context it stopped with (Frame::last, the CTX record's words: calllog.md 4.4).
        // Only the call-log host reads them.
        bool keepStopWords = true;

        // Statistics.
        uint64_t dispatches = 0, stops = 0, handlersRun = 0, jumps = 0;

    private:
        static void kernelEntry(uint8_t *rdram, R5900Context *ctx, Runtime *rt);
        static void libraryEntry(uint8_t *rdram, R5900Context *ctx, Runtime *rt);
        // Runs the native kernel call; returns true when the caller must stop (it blocked or gave way).
        bool kernelCall(KernelCall call, R5900Context *ctx);
        Thread *choose();
        void runFrame(Thread &t, Frame &f);
        // Runs guest code at ctx->pc (enter) with a stop's jump target set (jumpStops): true when the bottom function
        // returned, false when guest code stopped.
        bool enterCatchingStops(R5900Context *ctx);
        // Runs guest code at ctx->pc until the bottom function returns.
        void enter(R5900Context *ctx);
        bool mustStop(const R5900Context *ctx, uint64_t cycle);
        void idleUntilEvent();
        // Whether an event is due (free mode: by the clock; pinned mode: the host's point), and taking it (free mode:
        // every event due by `cycle`; pinned mode: the next one).
        bool eventDue(const R5900Context *ctx, uint64_t cycle);
        void takeEvents(uint64_t cycle);

        Host &host_;
        Runtime rt_;
        Kernel kernel_;
        std::map<uint32_t, KernelCall> kernelCalls_;
        uint64_t cycle_ = 0;
        uint32_t heapStart_ = 0;
        int32_t syscall_ = 0; // the syscall being answered (0: none; 0 is no service BozoEngine answers)
        int32_t heapSize_ = -1; // SetupHeap's size (negative: up to the main thread's stack)
        uint64_t gsImr_ = 0x7F00; // GsPutIMR's mask (no GS behind it): every GS interrupt masked
        uint32_t vsyncFlag_ = 0, vsyncCsr_ = 0; // SetVSyncFlag's addresses
        uint32_t syncVCallback_ = 0, syncVGp_ = 0;
        uint64_t framesCompleted_ = 0;
        uint64_t checkpoints_ = 0; // checkpoints passed (while guest code runs: its context's count)
        uint64_t sliceStart_ = 0;  // the running thread's time slice began here
        uint32_t timerPending_ = 0;
        bool inGuest_ = false;
        Thread *current_ = nullptr;
        Frame *currentFrame_ = nullptr;
        uint64_t outcallCheckpoints_ = 0; // ctx->checkpoints when the last out-call ended
        uint32_t recentPcs_[64] = {}; // the pcs of the last checkpoints that asked the runtime (a ring)
        uint32_t recentNext_ = 0;
        // The (thread, depth) guest code last ran on, and how it was left.
        int32_t lastThread_ = 0;
        uint32_t lastDepth_ = 0;
        bool lastHandlerEnded_ = false;
        bool started_ = false;
        uint32_t outcallDepth_ = 0;
        uint32_t deferredPc_ = 0;
        void **stopJump_ = nullptr; // enterCatchingStops' __builtin_setjmp buffer while guest code runs (jumpStops)
    };
}
