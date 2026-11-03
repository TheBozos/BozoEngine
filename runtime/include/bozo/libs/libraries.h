// BozoEngine's native libraries (phase 3): the game's library calls answered by native code, at the level of the call
// (no hardware model below it). Each function is known by the game's symbol name; a runtime binds it at the address
// the game's ELF gives that name. Spec: docs/specs/libraries.md.
#pragma once

#include "bozo/libs/gs.h"
#include "bozo/libs/guest.h"
#include "bozo/libs/mc.h"
#include "bozo/libs/pad.h"

#include <cstdint>
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
        kAllLibraries = (1u << 5) - 1,
    };
    const char *libraryName(Library l);
    // "libc,libm,gs,pad,mc", "all" or "none" -> a mask; false on an unknown name.
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

        // ---- State ----
        Heap heap;
        Rand rand;
        GsState gs;
        PadState pad;
        McState mc;
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
    };

    // Each library's functions (libc.cpp, libm.cpp, gs.cpp, pad.cpp, mc.cpp).
    void addLibc(std::vector<Function> &out);
    void addLibm(std::vector<Function> &out);
    void addGs(std::vector<Function> &out);
    void addPad(std::vector<Function> &out);
    void addMc(std::vector<Function> &out);

    // printf-style formatting as the reference's sprintf: the format at `format`, the arguments from argument slot
    // `first` on (64-bit doubles for %f/%e/%g). Returns the text without its terminator.
    std::string formatGuest(Guest &g, uint32_t format, int first);
}
