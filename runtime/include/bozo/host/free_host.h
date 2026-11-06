// The free host (phase 4, milestone M0): the game on BozoEngine with no call log. Every out-call is answered by
// BozoEngine itself: the kernel calls by the dispatcher, the library calls by the native libraries (bound by the
// ELF's symbol names), the port's overrides natively (bozo/port.h), the VU0 microprograms and the hardware registers by
// their native models. Time is BozoEngine's own (free mode, kernel.md section 4): the vblank's events happen when the
// EE clock reaches them. Headless, the renderer records what it is given (bozo/render/renderer.h); in real time (a
// window: bozo/app/window.h), the port's frames are held to their rate on the host's clock and presented.
#pragma once

#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "bozo/reference.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bozo::host
{
    struct FreeOptions
    {
        std::string elf;     // the game's executable (its symbols bind the libraries)
        std::string iso;     // the disc image (the CD library reads it; 989SND's banks)
        uint32_t frames = 0; // stop after this many game frames (0: never)
        std::string padScript; // a frame-keyed pad script for player 1 (libraries.md section 6), empty: no input
        std::array<std::string, 3> playerScripts; // players 2-4's scripts (with a multitap: 3 and 4), empty: none
        std::string cardDir;   // the memory cards' folder (empty: fresh formatted cards in memory)
        std::string out;       // a PS2XREF1 record of the run (empty: none)
        bool quiet = false;
        bool printGameOutput = false; // the game's printf
        bool profileIo = false; // count remaining device reads/writes by guest function and address
        uint32_t stallFields = 600;   // end the run when no game frame ended for this many fields of EE time (0: never)
        // The game on the host's clock (a window): port::Host::realTime() true, deterministic() false (the port holds
        // its frames to their rate: FramePacer). Off: as fast as the host makes frames, nothing host-timed.
        bool realTime = false;
        // The sound: 989SND's engine and its mixer brought up to each frame's end on the EE clock (libraries.md 15.13c,
        // free mode), the frame's samples to onAudio and to the record's sound field. Off: the mixer runs only as the
        // driver's commands bring it up (nothing is output).
        bool sound = true;
        // Optional native game-frame duration. Ports with host-paced frames can charge their logical
        // clock here instead of running a console timer/device loop. Zero keeps explicit port accounting.
        uint64_t framePeriodCycles = 0;
    };

    struct FreeStats
    {
        uint64_t libraryCalls = 0, kernelCalls = 0, overrideCalls = 0, vu0Calls = 0, ioAccesses = 0, ioUnknown = 0;
        uint64_t frames = 0;
        std::map<std::string, uint64_t> unanswered; // calls with no native answer (by name), which end the run
        // (guest PC, device address) -> {reads, writes}; populated only when profileIo is enabled.
        std::map<std::pair<uint32_t, uint32_t>, std::array<uint64_t, 2>> ioSites;
    };

    class FreeHost final : public ee::Host, public ee::KernelObserver, public port::Host
    {
    public:
        explicit FreeHost(const FreeOptions &options);
        ~FreeHost();

        // Loads the ELF into guest memory and binds every external of the generated code (`externals`) by the ELF's
        // symbol at its address: a kernel call, a port override (`registry`), a native library function. False with
        // `error` when the ELF cannot be read.
        bool setUp(ee::Dispatcher &d, const BozoExternalEntry *externals, size_t count, const port::Registry *registry,
                   std::string &error);
        // Runs until the frame limit, a fault or the end of every thread. Returns the frames ended.
        uint64_t run();

        const FreeStats &stats() const { return stats_; }
        // Called at each game frame's end (after the frame's record), with the frame's number from 0 and the context
        // the frame ended in (its ra: who waited for the frame's end).
        std::function<void(uint64_t frame, const R5900Context *ctx)> onFrame;
        // Called at each game frame's end (before onFrame) with the stereo 16-bit 48 kHz samples the mixer made since the
        // last one (FreeOptions::sound), on the game's thread.
        std::function<void(uint64_t frame, const int16_t *samples, size_t count)> onAudio;
        // The ELF function holding `pc` (its symbol, "?" if none), and a symbol's address (0 if none).
        std::string functionAt(uint32_t pc) const;
        uint32_t symbol(const std::string &name) const;
        // Out-calls per port override address.
        const std::map<uint32_t, uint64_t> &overrideCalls() const { return perOverride_; }
        std::string endReason() const { return endReason_; }
        // Any thread: the run ends at the next game frame's end (or the watchdog's next look).
        void requestStop() { stopRequested_.store(true, std::memory_order_relaxed); }
        // Any thread: the game frames ended so far.
        uint64_t framesEnded() const { return framesEnded_.load(std::memory_order_relaxed); }
        libs::Libraries &libraries() override { return libs_; }

        // ee::Host
        void libraryCall(ee::Dispatcher &, R5900Context *ctx, uint32_t target) override;
        void vu0Microprogram(ee::Dispatcher &, R5900Context *ctx, uint32_t address) override;
        void ioRead(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, uint64_t value[2]) override;
        void ioWrite(ee::Dispatcher &, R5900Context *ctx, uint32_t address, uint32_t size, const uint64_t value[2]) override;
        bool cop0Condition(ee::Dispatcher &, R5900Context *) override;
        void waitCompleted(ee::Dispatcher &, ee::Thread &t) override;
        void handlerCompleted(ee::Dispatcher &, ee::Thread &t, const ee::Frame &f) override;
        void fault(ee::Dispatcher &, R5900Context *, const std::string &what) override;
        void idle(ee::Dispatcher &, uint64_t, uint64_t) override { watchdog(nullptr); }
        void resuming(ee::Dispatcher &, ee::Thread &, ee::Frame &f, uint32_t) override { watchdog(&f.ctx); }
        void kernelCallBegin(ee::Dispatcher &, R5900Context *ctx, uint32_t target, uint32_t kind) override;
        void kernelCallEnd(ee::Dispatcher &, R5900Context *, uint32_t target, bool) override;
        bool librarySyscall(ee::Dispatcher &, R5900Context *ctx, const char *name) override;

        // ee::KernelObserver
        bool dmacStatusSet(uint32_t cause) override;

        // port::Host
        void frameEnd(Runtime *rt, R5900Context *ctx) override;
        bool callLibrary(Runtime *rt, R5900Context *ctx, uint32_t address) override;
        bool deterministic() const override { return !opt_.realTime; }
        bool realTime() const override { return opt_.realTime; }
        bool nativeGraphics() const override { return instructionHandler_ != nullptr; }
        bool nativeInstruction(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t pc) override
        {
            return instructionHandler_ && instructionHandler_(ram, ctx, rt, pc);
        }

    private:
        port::InstructionHandler instructionHandler_ = nullptr;
        void runOverride(R5900Context *ctx, uint32_t target, port::Function fn);
        // Ends the run when no frame ended for opt_.stallFields fields (a wait that never ends), saying where.
        void watchdog(R5900Context *ctx);
        uint64_t lastFrameCycle_ = 0;
        void endFrame(R5900Context *ctx);
        void frameSound(uint64_t &hash, uint32_t &count);
        std::vector<int16_t> audio_;
        // Real time with sound (libraries.md 15.13e): when no frame has ended for a while (a load, a stall), the sound
        // is still made, on the game's thread between two out-calls (never inside a command): a watcher thread only
        // raises soundDue_, the watchdog's next look brings the engine up to the host's clock and hands the samples to
        // onAudio. The next frame's end takes them as made (the engine's clock only moves forward).
        void stallSound(const R5900Context *ctx);
        void startSoundWatcher();
        void stopSoundWatcher();
        std::atomic<bool> soundDue_{false};
        std::atomic<int64_t> frameEndNs_{0};
        std::atomic<bool> watcherStop_{false};
        std::thread watcher_;

        FreeOptions opt_;
        ee::Dispatcher *d_ = nullptr;
        libs::Libraries libs_;
        // Looked up at every out-call (a few thousand a frame in a match): hashed.
        std::unordered_map<uint32_t, port::Function> portAt_;
        std::unordered_map<uint32_t, const libs::Function *> nativeAt_;
        std::unordered_set<uint32_t> flushCacheAt_; // the kernel's FlushCache entries (kernelCallEnd)
        bool flushCacheSyscall_ = false;            // the syscall being answered is FlushCache's
        std::map<uint32_t, std::string> names_;
        std::map<uint32_t, uint64_t> perOverride_;
        uint32_t insideDepth_ = 0;
        FreeStats stats_;
        reference::Writer writer_;
        bool writing_ = false;
        std::string endReason_;
        libs::ScriptInput padScript_;
        std::atomic<bool> stopRequested_{false};
        std::atomic<uint64_t> framesEnded_{0};
    };
}
