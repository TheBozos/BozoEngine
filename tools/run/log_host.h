// bozo-run's host: everything BozoEngine does not implement natively yet answered from a call log (docs/specs/
// calllog.md section 8), and every decision of the native kernel and dispatcher checked against the log's records in
// lockstep. The first record that differs ends the run with a report.
#pragma once

#include "calllog.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "bozo/reference.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bozo::run
{
    struct Options
    {
        uint32_t frames = 0;           // stop after this many frames (0: the log's frame limit)
        uint64_t traceFrom = ~0ull;    // print every record matched from this record index
        bool traceCheckpoints = false; // and every checkpoint passed from there
        uint32_t maxErrors = 1;        // stop at the n-th divergence (cycle mismatches are resynced to go on)
        bool quiet = false;
        // Pinned mode (kernel.md section 4): the vblank's events are taken at the log's points (out-calls before,
        // checkpoints since the last one, pc); EE time is reported, not checked.
        bool pinned = false;
        // The native libraries answering their calls instead of the log (libs::Library bits), each call checked
        // against its record: result registers and every byte of memory written.
        uint32_t native = libs::kAllLibraries;
        // Pinned mode: whether the 989SND driver takes the decisions the reference's IOP time made from the log's replies
        // (libraries.md section 15.5); off, it answers them from its own models too.
        uint32_t sndFromLog = 0; // 989SND replies taken from the log: bit 0 0x11, 1 0x19, 2 0x21, 3 0x2c (none: the model answers)
        // A port's overrides run natively instead of being answered from the log (phase 4, kernel.md section 3b):
        // each call checked against its record (context words, memory runs, traced calls, operations, frame end).
        const port::Registry *overrides = nullptr;
    };

    // The port's overrides run natively: per override address, calls and divergences.
    struct OverrideStats
    {
        uint64_t calls = 0, nested = 0, divergences = 0, timeDiffers = 0, innerCalls = 0, frames = 0;
        uint64_t bound = 0;                   // override addresses bound (registered and an override of the log)
        std::vector<uint32_t> notInLog;       // registered, but not an override of the reference's run
        std::vector<uint32_t> fromLog;        // the log's overrides with no registration: answered from the log
        std::map<uint32_t, uint64_t> perAddress;
    };

    // Per library: calls answered natively, and the native state checked against calls answered from the log.
    struct NativeStats
    {
        uint64_t calls[16] = {};
        uint64_t timeDiffers = 0; // timed calls whose EE time is not the log's (free mode: reported, the log's kept)
        uint64_t observed = 0; // traced or answered calls the native state followed
        uint64_t notNative = 0; // calls of a native library it has no answer for (left to the log)
        uint64_t divergences = 0;
    };

    // Pinned mode: how far BozoEngine's own clock and checkpoint count are from the log's (reported, not checked).
    struct PinnedStats
    {
        uint64_t events = 0;            // events taken at the log's point
        uint64_t cycleDiffers = 0;      // records whose cycle differs from BozoEngine's clock
        uint64_t checkpointsDiffer = 0; // records whose checkpoint count differs
        uint64_t shifted = 0;           // taken one checkpoint later than the log's count, at its pc
        uint64_t firstCycleRecord = ~0ull, firstCheckpointRecord = ~0ull;
        std::vector<uint64_t> shiftedRecords; // the first 16 of them
    };

    struct FieldStats
    {
        uint32_t frames = 0;
        uint32_t ram = 0, scratchpad = 0, calls = 0, threads = 0, eeCycle = 0, callCount = 0; // frames matching
        int64_t firstRam = -1, firstScratchpad = -1, firstCalls = -1, firstThreads = -1, firstEeCycle = -1;
        uint32_t draws = 0, drawCounts = 0; // frames whose draw-list hash (and batch and vertex counts) match: reported
        int64_t firstDraws = -1;
    };

    class LogHost final : public ee::Host, public ee::KernelObserver, public port::Host
    {
    public:
        LogHost(calllog::Reader &log, const Options &options);

        // Binds every external address of the log (NAME, OVRD) on the dispatcher: the kernel calls natively, the rest
        // to this host. Returns the number of kernel calls bound.
        uint32_t bind(ee::Dispatcher &d);
        // Applies the INIT record to guest memory. False if the log does not start with one.
        bool init(ee::Dispatcher &d);

        void setReference(std::vector<reference::FrameRecord> *ref) { ref_ = ref; }
        void setWriter(reference::Writer *w) { writer_ = w; }

        const FieldStats &stats() const { return stats_; }
        const PinnedStats &pinnedStats() const { return pinned_; }
        const NativeStats &nativeStats() const { return native_; }
        const OverrideStats &overrideStats() const { return overrides_; }
        uint32_t errors() const { return errors_; }
        uint64_t recordsMatched() const { return matched_; }
        uint64_t calls() const { return seq_; }
        uint64_t kernelCalls() const { return kernelCalls_; }
        bool completed() const { return completed_; }
        uint32_t frame() const { return frame_; }

        // ee::Host
        void libraryCall(ee::Dispatcher &, R5900Context *ctx, uint32_t target) override;
        void vu0Microprogram(ee::Dispatcher &, R5900Context *ctx, uint32_t address) override;
        void ioRead(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, uint64_t value[2]) override;
        void ioWrite(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, const uint64_t value[2]) override;
        bool cop0Condition(ee::Dispatcher &, R5900Context *) override;
        void kernelCallBegin(ee::Dispatcher &, R5900Context *, uint32_t target, uint32_t kind) override;
        void kernelCallEnd(ee::Dispatcher &, R5900Context *, uint32_t target, bool stopping) override;
        void invoked(ee::Dispatcher &, const ee::Thread &, const ee::Frame &, bool insideCall) override;
        void invokeEnded(ee::Dispatcher &, const ee::Thread &, const ee::Frame &) override;
        void switched(ee::Dispatcher &, int32_t from, uint32_t fromDepth, int32_t to, uint32_t toDepth, uint16_t reason,
                      uint32_t waitReason, uint32_t waitObject, uint64_t token) override;
        void resuming(ee::Dispatcher &, ee::Thread &, ee::Frame &, uint32_t depth) override;
        void waitCompleted(ee::Dispatcher &, ee::Thread &) override;
        void handlerCompleted(ee::Dispatcher &, ee::Thread &, const ee::Frame &) override;
        void threadEnded(ee::Dispatcher &, ee::Thread &) override;
        void fault(ee::Dispatcher &, R5900Context *, const std::string &what) override;
        bool eventPinned(ee::Dispatcher &, const R5900Context *ctx) override;
        uint64_t outcallDeadline(ee::Dispatcher &) override;
        bool outcallLeave(ee::Dispatcher &, const R5900Context *ctx) override;
        bool eventPinnedSoon(ee::Dispatcher &) override;

        // ee::KernelObserver
        void op(const ee::OpEvent &e) override;
        bool dmacStatusSet(uint32_t cause) override;

        // port::Host
        libs::Libraries &libraries() override { return libs_; }
        void frameEnd(Runtime *rt, R5900Context *ctx) override;
        bool callLibrary(Runtime *rt, R5900Context *ctx, uint32_t address) override;

    private:
        // The next record; ends the run (with a report) when the log has none.
        const calllog::Record &next(const char *wanted);
        // A divergence: reported; the run ends unless `resync` and errors remain.
        void diverge(const std::string &what, const calllog::Record *r, bool resync = false);
        [[noreturn]] void endRun(const std::string &why);
        std::string where() const;
        void checkPoint(const calllog::Record &r, uint64_t cycle, const char *what);
        // The next record, when it is an event's first (an OP of origin events outside any call); else null.
        const calllog::Record *nextEvent();
        // An out-call answered from the log: the CALL is checked, applied, and the records inside it made again.
        const calllog::Record &takeCall(uint8_t kind, uint32_t target, R5900Context *ctx, calllog::Call &c);
        // `native`: the call is made by the native library instead (and checked against the record).
        void answer(R5900Context *ctx, const calllog::Record &r, calllog::Call &c, bool traced,
                    const libs::Function *native = nullptr);
        void runNative(R5900Context *ctx, const calllog::Record &r, const calllog::Call &c, const libs::Function &f);
        // How the memory a native call wrote differs from the record's runs ("" when it does not).
        std::string compareMemory(const std::vector<calllog::Run> &runs);
        // Native state following a call answered from the log, and the calls made inside it.
        void observe(R5900Context *ctx, const calllog::Record &r, const calllog::Call &c);
        void applyWords(R5900Context *ctx, const calllog::Call &c);
        void applyRuns(const std::vector<calllog::Run> &runs);
        // The records made inside a call (OPs of depth 0, INVOKE, FRAME), made again in order.
        bool insideRecords(R5900Context *ctx, uint64_t callSeq, const calllog::Call &c, uint32_t &tracesTaken,
                           bool native = false);
        // `traces` (the frame's traced calls when not the record's) and `apply` (the record's memory applied: calls
        // answered from the log; a native override's memory is compared instead).
        void endFrame(const calllog::Record &r, const calllog::Frame &f, uint32_t openId, const uint32_t *openArgs,
                      const std::vector<calllog::Trace> *traces = nullptr, bool apply = true);
        // ---- The port's overrides, run natively (kernel.md section 3b) ----
        void runOverride(R5900Context *ctx, const calllog::Record &r, const calllog::Call &c, port::Function fn);
        // Out-calls made inside a native override (none is a record of the log): made natively.
        void innerLibraryCall(R5900Context *ctx, uint32_t target);
        void innerIo(R5900Context *ctx, uint32_t address, uint32_t size, bool store, uint64_t value[2]);
        void innerTrace(uint32_t id, const uint32_t a[4], uint32_t v0, bool blocked);
        void continuation(uint32_t origin, uint32_t memSource, const char *what);
        // A native library's continuation (origin resume): made on the thread's context, checked against the log's
        // MEM records of that source (its operations are matched as the kernel makes them).
        void nativeContinuation(ee::Thread &t, std::function<void(libs::Libraries &, libs::Guest &)> run, uint32_t memSource,
                                const char *what);
        void trace(const calllog::Record &r, const char *note = "");
        uint32_t traceId(uint8_t kind, uint32_t target, const R5900Context *ctx) const; // 0: not traced
        void addTraces(const std::vector<calllog::Trace> &t, size_t from);
        void trackDma(uint32_t address, uint32_t value);

        calllog::Reader &log_;
        Options opt_;
        ee::Dispatcher *d_ = nullptr;
        std::vector<reference::FrameRecord> *ref_ = nullptr;
        reference::Writer *writer_ = nullptr;
        std::map<uint32_t, calllog::NameInfo> names_;
        std::map<uint32_t, uint32_t> traceIds_; // address -> FNV id of its name

        uint64_t seq_ = 0;    // out-calls made so far
        uint32_t frame_ = 0;  // frames ended so far
        uint64_t matched_ = 0;
        uint64_t kernelCalls_ = 0;
        uint32_t errors_ = 0;
        bool completed_ = false;
        FieldStats stats_;
        PinnedStats pinned_;
        int64_t callDrift_ = 0;       // BozoEngine's clock minus the log's at the last out-call (pinned mode)
        uint64_t eventIndex_ = ~0ull; // the record nextEvent() last looked at, and whether it starts an event
        bool eventIs_ = false;
        uint64_t armed_ = ~0ull;      // the event record found due (until it is taken)
        reference::CallList calls_;

        // The kernel call in progress.
        struct KernelCallState
        {
            bool active = false;
            bool tail = false;
            uint64_t seq = 0;
            uint32_t target = 0;
            uint32_t kind = 0;
            uint64_t recordIndex = 0;
            calllog::Call call;
            uint8_t flags = 0;
            ContextWords entry{};
            uint32_t args[4] = {};
            bool inner = false; // made inside a native override (no record of its own)
        } kc_;
        // The traced call in progress (its entry is "open" if the frame ends inside it), and whether it blocked.
        uint32_t openId_ = 0;
        uint32_t openArgs_[4] = {};
        bool blockedInCall_ = false;
        // Contexts the log alone knows: a library handler's first context, a library continuation's changes.
        std::set<std::pair<int32_t, uint32_t>> lenient_;
        // The native libraries, the functions bound by address, and the traced calls' names by id.
        libs::Libraries libs_;
        std::map<uint32_t, const libs::Function *> nativeAt_;
        std::map<uint32_t, std::string> idNames_;
        libs::Journal journal_;
        NativeStats native_;
        // CPCOND0 from the game's DMA register stores (replay.md section 3).
        uint32_t dmaDone_ = 0, dmaPcr_ = 0;
        bool dmaPcrKnown_ = false;
        uint32_t vif0Tadr_ = 0;
        // The port's overrides bound natively, and the one running (its call, its traced calls so far).
        std::map<uint32_t, port::Function> portAt_;
        OverrideStats overrides_;
        uint32_t insideDepth_ = 0;
        uint64_t overrideSeq_ = 0;
        calllog::Call overrideCall_;
        uint64_t overrideRecord_ = 0;
        bool overrideTransfer_ = false;
        bool overrideTimeUnknown_ = false; // a timed library call inside took a time the log does not hold
        std::vector<calllog::Trace> innerTraces_;
        size_t innerTracesAdded_ = 0; // those already in a frame's list (a frame ended inside the override)
    };
}
