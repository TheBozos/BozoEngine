// BozoEngine's native libraries (phase 3): the game's library calls answered by native code, at the level of the call
// (no hardware model below it). Each function is known by the game's symbol name; a runtime binds it at the address
// the game's ELF gives that name. Spec: docs/specs/libraries.md.
#pragma once

#include "bozo/audio/port_stream.h"
#include "bozo/ee/kernel.h"
#include "bozo/libs/cd.h"
#include "bozo/libs/guest.h"
#include "bozo/libs/io.h"
#include "bozo/libs/mc.h"
#include "bozo/libs/mpeg.h"
#include "bozo/libs/pad.h"
#include "bozo/libs/sif.h"
#include "bozo/libs/snd.h"
#include "bozo/libs/timer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bozo::ee
{
    class Dispatcher;
}

namespace bozo::libs
{
    // The libraries, for per-library switches (a bit each).
    enum Library : uint32_t
    {
        kLibc = 1u << 0, // memcpy, memset, the string functions, sprintf/printf, malloc/free, rand/srand
        kLibm = 1u << 1, // sqrt and the float sine/cosine kernels
        kGs = 1u << 2,   // the GS library (display and draw environments, uploads, syncs)
        kPad = 1u << 3,  // libpad
        kMc = 1u << 4,   // libmc
        kCd = 1u << 5,   // libcdvd: sectors and the stream from the disc image
        kVu0 = 1u << 6,  // TMB's VU0 microprograms (VCALLMS)
        kIo = 1u << 7,   // the hardware registers the game loads and stores (DMA, timer 0, the GS's privileged ones)
        kMpeg = 1u << 8, // libmpeg: the movies (needs FFmpeg)
        kSif = 1u << 9,  // the SIF layer: RPC, the IOP heap, module loading, the SIF registers and DMA
        kSnd = 1u << 10, // the 989SND sound driver behind the SIF RPC
        kTimer = 1u << 11, // libkernel's timer library and DelayThread
        kAllLibraries = (1u << 12) - 1,
    };
    constexpr unsigned kLibraryCount = 12;

    // Which microcode VU0's code memory holds: the image of the VIF0 chain the game kicked last (libraries.md
    // section 10). TMB's two chains are static data in its ELF.
    enum class Vu0Image : uint8_t
    {
        None,
        Hierarchy, // hier__Fi and main: the scene traversal's programs
        TwoD,      // drawInit (debug draws only)
    };
    struct Vu0State
    {
        static constexpr uint32_t kHierarchyChain = 0x234060u, k2dChain = 0x2337D0u;
        static constexpr uint32_t k2dImageEnd = 230u * 8u; // the 2D image's MPG covers code below this
        // The hierarchy image until a kick of the 2D chain is seen: main and hier__Fi send it inside calls the logs
        // answer whole (no VIF0 store of theirs is an out-call of its own), and the 2D image is debug draws' only.
        Vu0Image image = Vu0Image::Hierarchy;
        bool hierarchyHigh = true; // the hierarchy image was loaded once: its code above k2dImageEnd stays
        // A VIF0 DMA kick (CHCR written with STR): the chain it sends, by its tag address.
        void kick(uint32_t chcr, uint32_t tadr);
    };
    const char *libraryName(Library l);
    // "libc,libm,gs,pad,mc,cd,vu0,io,mpeg", "all" or "none" -> a mask; false on an unknown name.
    bool parseLibraries(const std::string &list, uint32_t &mask);

    class Libraries;
    using Handler = void (*)(Libraries &, Guest &);

    struct Function
    {
        const char *name; // the game's symbol (a leading "ps2_" in a log's name is ignored)
        Library library;
        Handler handler;
        // The call ends the game's frame and waits for the vblank (sceGsSyncV): its time and its frame end are the
        // runtime's frame boundary, not the library's (in a pinned run, the log's).
        bool frameBoundary = false;
        // The call takes EE time or may make its thread wait (a disc read, a stream read): what it decides about time
        // follows Libraries::pin when a pinned run sets it (the reference's points), else the library's own model.
        bool timed = false;
        // A pinned run gives the call the log's reply words (Pin::logWord): the 989SND driver behind sceSifCallRpc takes
        // from them what the reference's IOP time decided (libraries.md section 15.5).
        bool logReply = false;
    };

    // A pinned run's points for the timed call being made (kernel.md section 4: what the reference decided about
    // time, taken from its log): the EE cycles the call takes and whether it makes its thread wait, until when.
    struct Pin
    {
        bool active = false;
        uint64_t cycles = 0;
        // The call's EE time is the library's own (a call made inside a port's override: the log has no record of its
        // own for it, only the wait it made: kernel.md section 3b).
        bool nativeCycles = false;
        bool waits = false;
        uint64_t token = 0; // the wait's token (a vblank wait: the vblank count it waits past)
        // The end (flat address + 1) of the memory the log's call wrote, 0 if it wrote none: how much of a stream read
        // had arrived when it was made (the ring's state at that point).
        uint32_t writtenEnd = 0;
        // The reference's reply word at an EE address (a pinned run's log), for the decisions its IOP's time made (the
        // 989SND driver's sound lifetimes): set for a call whose function has `logReply`.
        SndDriver::LogWord logWord;
    };

    // The game's heap as the reference runtime keeps it: blocks carved from guest memory from the heap's start
    // (SetupHeap's result), sizes rounded up to 16 bytes, first fit by address, a freed block merged with its free
    // neighbours. No bookkeeping in guest memory (libraries.md section 2.2).
    class Heap
    {
    public:
        void reset(uint32_t start, uint32_t end);
        bool ready() const { return end_ != 0; }
        uint32_t allocate(uint32_t bytes); // 0 when no free block is large enough
        bool release(uint32_t address);    // false when `address` is not a block in use
        bool contains(uint32_t address,uint32_t bytes) const;
        uint32_t largestFree() const;
        uint32_t totalFree() const;
        size_t blocks() const { return used_.size(); }

    private:
        std::map<uint32_t, uint32_t> free_; // address -> bytes
        std::unordered_map<uint32_t, uint32_t> used_;
        uint32_t end_ = 0;
    };

    // rand/srand as the reference answers them: a 64-bit linear congruential generator (multiplier
    // 6364136223846793005, increment 1, state 1 until srand), rand returning bits 32-62 (libraries.md 2.3).
    struct Rand
    {
        uint64_t state = 1;
        void seed(uint32_t s) { state = s; }
        int32_t next()
        {
            state = state * 6364136223846793005ull + 1ull;
            return static_cast<int32_t>((state >> 32) & 0x7FFFFFFFu);
        }
    };

    class Libraries
    {
    public:
        Libraries();

        // The runtime the libraries serve (the kernel's operations, the heap's start).
        void attach(ee::Dispatcher &d) { d_ = &d; }
        ee::Dispatcher *dispatcher() { return d_; }

        // The native function for a symbol name, or nullptr.
        const Function *find(const std::string &name) const;
        const std::vector<Function> &functions() const { return functions_; }

        // Native state follows the calls the native runtime does not make itself (calls answered from a log, and
        // the calls game code makes inside them). False with `why` when the call's recorded result disagrees with
        // the native state.
        bool observeTraced(const std::string &name, const uint32_t a[4], uint32_t v0, std::string &why);
        // Whether a traced call to `name` touches native state (rand, srand, malloc, free).
        static bool follows(const std::string &name);
        // A call answered from a log, after its effects were applied (`g` holds its context and memory).
        bool observeAnswered(const std::string &name, Guest &g, std::string &why);

        // ---- Time and waits ----
        // The EE clock of the call being made (the dispatcher's).
        uint64_t now(Guest &g) const;
        // The EE cycles a timed call takes: the pinned point, else the library's own `native` answer.
        uint64_t callCycles(uint64_t native) const { return pin.active && !pin.nativeCycles ? pin.cycles : native; }
        // Whether a timed call makes its thread wait, and the wait's token: the pinned point, else the library's own.
        bool waitPoint(bool nativeWaits, uint64_t nativeToken, uint64_t &token) const
        {
            token = pin.active ? pin.token : nativeToken;
            return pin.active ? pin.waits : nativeWaits;
        }
        // The vblank count to wait past so a thread waiting from now resumes at the first vblank at or after `cycle`.
        uint64_t vblankTokenFor(uint64_t cycle) const;
        // The running thread waits for a vblank (the kernel's wait: `fixedResult` its v0, -1 the vblank's parity); with
        // `completion`, `then` runs as it resumes (a library continuation, calllog.md section 5, origin resume).
        void waitVblank(uint64_t token, int32_t fixedResult, std::function<void(Libraries &, Guest &)> then = {});
        // A library continuation of a thread's wait, run by the runtime as the thread resumes (null if none).
        std::function<void(Libraries &, Guest &)> takeContinuation(int32_t thread);
        bool hasContinuation(int32_t thread) const { return continuations_.count(thread) != 0; }
        // The running thread waits (any reason) with a completion: `then` runs as it resumes.
        void waitFor(uint32_t reason, uint32_t object, uint64_t token, int32_t fixedResult,
                     std::function<void(Libraries &, Guest &)> then);
        // A handler queued by a library; `end` (if any) is its completion, run as it returns (origin handler-end).
        void queueHandler(const ee::QueuedHandler &h, std::function<void(Libraries &, Guest &)> end);
        std::function<void(Libraries &, Guest &)> takeHandlerEnd(uint64_t sequence);
        Pin pin;

        // ---- VU0 microprograms ----
        // The native program at `pc` of the image VU0 holds (null if none): it runs on the calling context's VU0 state
        // and VU0's data memory.
        const Function *vu0Program(uint32_t pc) const;

        // ---- State ----
        Heap heap;
        Rand rand;
        PadState pad;
        McState mc;
        CdState cd;
        Vu0State vu0;
        IoState io;
        // Opaque libdma SDK configuration returned by GetEnv. Native services do not
        // turn it into DMA controller registers or execute a hardware environment.
        std::array<uint8_t, 20> dmaEnvironment{};
        MpegState mpeg;
        SifState sif;
        TimerState timers;
        SndDriver snd;
        // Whether the 989SND driver answers the RPC (else its replies come from the log and it follows them).
        bool sndNative = true;
        // Optional native audio implementation. Advances to EE time and returns its sample queue.
        // Unset: the built-in 989SND source. Its lifetime belongs to the port.
        std::function<audio::Mixer *(uint64_t)> audioProvider;
        MovieSoundSink *movieSound = nullptr; // the movie's sound (null: dropped)
        audio::PortStream portStream;
        IoAccess ioAccess;
        // The I/O library as a function: makes the access in `ioAccess`.
        static const Function &ioFunction();
        // Game frames ended so far: the runtime's count at its frame boundary (sceGsSyncV's result and the pad
        // script follow it).
        uint32_t framesEnded = 0;
        // printf's output (the game's debug text) goes to the host's stdout unless off.
        bool printOutput = true;

        // The heap, set up from SetupHeap's result on first use.
        Heap &gameHeap();

    private:
        ee::Dispatcher *d_ = nullptr;
        std::vector<Function> functions_;
        std::unordered_map<std::string, size_t> byName_;
        std::map<int32_t, std::function<void(Libraries &, Guest &)>> continuations_;
        std::map<uint64_t, std::function<void(Libraries &, Guest &)>> handlerEnds_;
    };

    // Each library's functions (libc.cpp, libm.cpp, gs.cpp, pad.cpp, mc.cpp).
    void addLibc(std::vector<Function> &out);
    void addLibm(std::vector<Function> &out);
    void addGs(std::vector<Function> &out);
    void addDma(std::vector<Function> &out);
    void addPad(std::vector<Function> &out);
    void addMc(std::vector<Function> &out);
    void addCd(std::vector<Function> &out);
    void addMpeg(std::vector<Function> &out); // nothing without a video decoder (FFmpeg)
    void addSif(std::vector<Function> &out);
    void addTimer(std::vector<Function> &out);
    // Sectors of the game's disc (the CD library's image, opened on first use): false when there is none.
    bool discRead(Libraries &l, uint32_t lbn, uint32_t count, uint8_t *out);

    // printf-style formatting as the reference's sprintf: the format at `format`, the arguments from argument slot
    // `first` on (64-bit doubles for %f/%e/%g). Returns the text without its terminator.
    std::string formatGuest(Guest &g, uint32_t format, int first);
}
