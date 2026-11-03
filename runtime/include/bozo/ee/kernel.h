// BozoEngine's EE kernel: guest threads and their scheduling, semaphores, interrupt and DMA handlers and their
// dispatch queue, the vblank. Native and deterministic; spec: docs/specs/kernel.md (the operations are those of
// calllog.md section 4.8, so a log can be checked against them and a library answered from a log can drive them).
//
// The kernel holds state and makes decisions. It never runs guest code: the dispatcher (ee/dispatcher.h) does, on the
// contexts the kernel keeps for every thread and stacked handler.
#pragma once

#include "bozo/context.h"
#include "bozo/context_words.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace bozo::ee
{
    // The operations on the kernel's state (calllog.md 4.8).
    enum Op : uint16_t
    {
        kOpQueue = 1,
        kOpBlock,
        kOpWake,
        kOpIrq,
        kOpTask,
        kOpEvent,
        kOpCreate,
        kOpStart,
        kOpExit,
        kOpTerminate,
        kOpPriority,
        kOpRotate,
        kOpSuspend,
        kOpResume,
        kOpWakeup,
        kOpCancelWakeup,
        kOpReleaseWait,
        kOpSemaCreate,
        kOpSemaDelete,
        kOpSignal,
        kOpPollSema,
        kOpWaitSema,
        kOpFlagCreate,
        kOpFlagDelete,
        kOpFlagSet,
        kOpFlagClear,
        kOpFlagPoll,
        kOpFlagWait,
        kOpAlarmSet,
        kOpAlarmCancel,
        kOpHandlerAdd,
        kOpHandlerRemove,
        kOpHandlerEnable,
        kOpCauseEnable,
        kOpVsyncFlag,
        kOpGsVsyncCallback,
        kOpFieldRate,
        kOpThreadSetup,
        kOpTransfer,
        kOpCount
    };
    const char *opName(uint32_t op);

    // Who made an operation (calllog.md section 5).
    enum Origin : uint32_t
    {
        kOriginCheckpoint = 0,
        kOriginCall = 1,
        kOriginIop = 2,
        kOriginIopVblank = 3,
        kOriginEvents = 4,
        kOriginResume = 5,
        kOriginHandlerEnd = 6,
        kOriginHandlerStart = 7,
        kOriginThreadEnd = 8,
        kOriginDispatcher = 9,
        kOriginTask = 10,
    };

    // A handler's kind (INVOKE's sub).
    enum InvokeKind : uint16_t
    {
        kInvokeInterrupt = 1,
        kInvokeAlarm = 2,
        kInvokeGsCallback = 3,
        kInvokeRpcCallback = 4,
        kInvokeSyscallOverride = 5,
        kInvokeExitHandler = 6,
        kInvokeLibraryCall = 7,
        kInvokeSifCommand = 8,
    };

    enum WaitReason : uint32_t
    {
        kWaitNone = 0,
        kWaitSleep = 1,
        kWaitSemaphore = 2,
        kWaitEventFlag = 3,
        kWaitVblank = 4,
        kWaitExternal = 5,
        kWaitMpeg = 6,
    };

    // Why the dispatcher left the (thread, depth) that ran last (SWITCH's sub).
    enum SwitchReason : uint16_t
    {
        kSwitchBlocked = 1,
        kSwitchPreempted = 2,
        kSwitchEnded = 3,
        kSwitchSuspended = 4,
        kSwitchDeleted = 5,
        kSwitchStart = 6,
        kSwitchHandlerStart = 7,
        kSwitchHandlerEnd = 8,
        kSwitchOther = 9,
    };

    enum class ThreadState : uint8_t
    {
        Dormant,       // created, not started (or ended)
        Ready,         // in its priority's ready queue
        Running,       // the running thread (also at the head of its ready queue)
        Waiting,
        Suspended,
        WaitSuspended,
    };

    constexpr uint32_t kPriorities = 128;
    constexpr uint32_t kFreshStatus = 0x10001u;      // COP0 Status of a new context (calllog: every first CTX)
    constexpr uint32_t kOne = 0x3F800000u;            // 1.0f: vf0.w and Q of a new context
    constexpr uint32_t kHandlerStackTop = 0x1FFFFF0u; // the stack handlers run on (one 16 KB area per thread)
    constexpr uint32_t kHandlerStackBytes = 0x4000u;
    constexpr uint64_t kNoCall = ~0ull;

    // One guest context the dispatcher can run: a thread's own (depth 0) or a handler stacked on it.
    struct Frame
    {
        R5900Context ctx;
        uint32_t status = kFreshStatus; // COP0 Status of this context
        ContextWords last{};            // the context when guest code last stopped here (calllog.md 4.4)
        bool ran = false;
        // A handler's invocation (depth >= 1).
        uint16_t kind = 0;
        uint32_t pc = 0, a[4] = {}, gp = 0, sp = 0, ra = 0;
        bool completion = false;
        uint64_t sequence = 0, tag = 0;
    };

    struct Thread
    {
        int32_t id = 0;
        ThreadState state = ThreadState::Dormant;
        uint32_t priority = 0, initPriority = 0;
        uint32_t entry = 0, stack = 0, stackSize = 0, gp = 0, attr = 0, option = 0, arg = 0;
        // The wait (Waiting / WaitSuspended).
        uint32_t waitReason = kWaitNone, waitObject = 0, flagBits = 0, flagMode = 0;
        uint64_t waitToken = 0;
        int32_t fixedResult = 0;   // a vblank wait's result (-1: the vblank's parity)
        bool waitCompletion = false; // the library that made the wait completes it as the thread resumes
        bool completionDue = false;  // woken from such a wait: the completion runs before the thread does
        int32_t wakeupCount = 0;
        bool pseudo = false;       // a thread of its own for handlers run while no thread is ready (negative id)
        int32_t handlerSlot = -1;  // its handler stack (kHandlerStackTop - slot * kHandlerStackBytes)
        std::vector<std::unique_ptr<Frame>> frames; // [0] the thread's own context, then the stacked handlers
        uint32_t depth() const { return static_cast<uint32_t>(frames.size() - 1); }
        Frame &top() { return *frames.back(); }
    };

    struct QueuedHandler
    {
        uint16_t kind = 0;
        uint32_t pc = 0, a[4] = {}, gp = 0, sp = 0, ra = 0;
        bool completion = false;
        uint64_t sequence = 0, tag = 0;
    };

    struct InterruptHandler
    {
        int32_t id = 0;
        uint32_t cause = 0, pc = 0, arg = 0, gp = 0, sp = 0;
        bool enabled = true;
    };

    struct Semaphore
    {
        int32_t id = 0;
        int32_t count = 0, max = 0, init = 0;
        uint32_t attr = 0, option = 0;
        std::deque<int32_t> waiters; // in the order they blocked
    };

    // An operation as made (calllog.md 4.8): what an observer (a checker against a log) sees.
    struct OpEvent
    {
        uint32_t op = 0;
        uint32_t origin = 0;
        uint32_t depth = 0;
        uint64_t originCall = kNoCall;
        uint64_t wide = 0;
        uint32_t argc = 0;
        uint32_t args[16] = {};
        int32_t thread = 0; // the running thread when it was made (0: none)
    };

    class KernelObserver
    {
    public:
        virtual ~KernelObserver() = default;
        virtual void op(const OpEvent &) {}
        // EnableDmac on a channel whose interrupt arrived while it was masked: is the channel's D_STAT bit still set?
        // (D_STAT follows the game's hardware stores; the native answer comes with the native DMA replacement.)
        virtual bool dmacStatusSet(uint32_t) { return true; }
    };

    class Kernel
    {
    public:
        Kernel();
        void setObserver(KernelObserver *o) { observer_ = o; }

        // ---- Who is making the next operations (calllog.md section 5) ----
        void setOrigin(uint32_t origin, uint64_t call = kNoCall)
        {
            origin_ = origin;
            originCall_ = call;
        }
        uint32_t origin() const { return origin_; }

        // ---- Threads ----
        Thread *thread(int32_t id);
        int32_t running() const { return running_; }
        // The thread the scheduler runs next: the running thread unless a ready thread has a higher priority, else
        // the head of the highest non-empty ready queue (0: none).
        int32_t best() const;
        int32_t firstReady() const;
        // A ready thread of the running thread's priority or higher: the running thread's time slice can end.
        bool sliceContender() const;
        // The main thread at boot (id 1, running, priority 0), its start context.
        void boot(uint32_t entry, uint32_t sp);
        // The ready queue in run order, the running thread first (the reference record's `threads`).
        void runOrder(std::vector<int32_t> &out) const;

        // ---- Operations (each emitted to the observer before its consequences, which are one depth deeper) ----
        int32_t createThread(uint32_t entry, uint32_t stack, uint32_t stackSize, uint32_t priority, uint32_t gp,
                             uint32_t attr, uint32_t option);
        int32_t startThread(int32_t id, uint32_t arg, bool interruptSafe);
        void exitThread(int32_t id, bool deleted);      // the thread ends (returned from its entry / ExitThread)
        int32_t terminateThread(int32_t id, bool deleted, bool interruptSafe);
        int32_t deleteThread(int32_t id, bool interruptSafe);
        int32_t changePriority(int32_t id, uint32_t priority, bool interruptSafe);
        int32_t rotateReadyQueue(uint32_t priority, bool interruptSafe);
        // The running thread blocks. `token` is the wide argument (vblank count, external token...).
        void block(int32_t id, uint32_t reason, uint32_t object, uint32_t bits, uint32_t mode, bool completion,
                   uint64_t token, int32_t fixedResult);
        // A waiting thread made ready with `result` in its v0.
        void wake(int32_t id, uint32_t result, bool completion, uint32_t reason);
        int32_t createSema(int32_t init, int32_t max, uint32_t attr, uint32_t option);
        int32_t deleteSema(int32_t id, bool interruptSafe);
        int32_t signalSema(int32_t id, bool interruptSafe);
        // WaitSema: returns the id, or blocks the running thread (the dispatcher then stops it).
        int32_t waitSema(int32_t id);
        int32_t pollSema(int32_t id);
        void irq(bool dmac, uint32_t cause);
        int32_t addHandler(bool dmac, uint32_t cause, uint32_t pc, int32_t next, uint32_t arg, uint32_t gp, uint32_t sp);
        int32_t removeHandler(bool dmac, uint32_t cause, int32_t id);
        // EnableIntc / DisableIntc / EnableDmac / DisableDmac (returns 0, as every call in the logs).
        int32_t causeEnable(bool dmac, uint32_t cause, bool on);
        void queue(const QueuedHandler &h);
        void fieldRate(uint32_t numerator, uint32_t denominator);
        uint32_t threadSetup(uint32_t stack, uint32_t stackSize, uint32_t gp);
        // A switch the kernel wants happens now (a non-interrupt kernel call's end, the TMB port's switchThread):
        // returns true when the running thread gives way (it is then no longer running).
        bool transfer(bool interruptSafe);
        // An operation of a library replayed from a log: argument words as logged.
        void replay(uint32_t op, const uint32_t *args, uint32_t argc, uint64_t wide);

        // ---- The dispatcher's side ----
        bool reschedulePending() const { return running_ == 0 || best() != running_; }
        // The running thread stops running and goes back to its ready queue (at its tail when it gives way, at its
        // head when preempted): no thread runs until setRunning.
        void deschedule(bool toTail);
        void setRunning(int32_t id);
        uint64_t picks() const { return picks_; } // how many times the running thread changed
        bool handlerQueued() const { return !queued_.empty(); }
        // The next queued handler, stacked on thread `on` (its depth grows by one). Returns its frame.
        Frame &stackHandler(Thread &on);
        // A frame for a handler stacked by a library call on the running thread (INVOKE inside a call).
        Frame &stackOn(Thread &on, const QueuedHandler &h);
        // A thread for handlers when no thread is ready (negative id, reused when idle).
        Thread &pseudoThread();
        // The handler on top of `t` returned: its frame is dropped.
        void endHandler(Thread &t);
        uint64_t sequence() const { return sequence_; }

        // ---- The vblank ----
        uint32_t vblankCount() const { return vblankCount_; }
        uint64_t nextVblankStart() const { return nextStart_; }
        uint64_t nextVblankEnd() const { return nextEnd_; }
        // The next scheduler event's cycle.
        uint64_t nextEvent() const { return nextStart_ < nextEnd_ ? nextStart_ : nextEnd_; }
        // Processes every event due at or before `cycle`, in order (origin: events).
        void processEvents(uint64_t cycle);

        const std::map<int32_t, std::unique_ptr<Thread>> &threads() const { return threads_; }

    private:
        void emit(uint32_t op, std::initializer_list<uint32_t> args, uint64_t wide = 0);
        struct Nested
        {
            Kernel &k;
            explicit Nested(Kernel &kk) : k(kk) { ++k.depth_; }
            ~Nested() { --k.depth_; }
        };
        void makeReady(Thread &t);
        void removeFromQueue(Thread &t);
        void vblankStart();
        void vblankEnd();
        void raise(bool dmac, uint32_t cause);
        Semaphore *sema(int32_t id);

        KernelObserver *observer_ = nullptr;
        KernelObserver defaultObserver_;
        uint32_t origin_ = kOriginCall;
        uint64_t originCall_ = kNoCall;
        uint32_t depth_ = 0;

        std::map<int32_t, std::unique_ptr<Thread>> threads_;
        std::array<std::deque<int32_t>, kPriorities> ready_;
        int32_t running_ = 0;
        bool switchRequested_ = false;
        uint64_t picks_ = 0; // a rotation of the running thread's queue: the next transfer gives way
        int32_t nextPseudo_ = -1;
        int32_t handlerSlots_ = 0;
        int32_t nextThreadId_ = 2;
        int32_t nextSemaId_ = 1;
        int32_t nextIntcHandler_ = 1, nextDmacHandler_ = 1;

        std::map<int32_t, Semaphore> semas_;
        std::vector<InterruptHandler> intc_[16], dmac_[16];
        int32_t nextHandlerId_ = 1;
        // At boot the vblank (INTC 2, 3) and every DMA channel are enabled (calllog: the first irq records).
        uint32_t intcMask_ = (1u << 2) | (1u << 3), dmacMask_ = 0x3FFu, intcPending_ = 0, dmacPending_ = 0;
        std::deque<QueuedHandler> queued_;
        uint64_t sequence_ = 0;

        uint32_t vblankCount_ = 0;
        uint64_t fieldCycles_ = 0;
        uint64_t nextStart_ = 0, nextEnd_ = 0;
    };
}
