// What a game port builds on (phase 4: TMB_Recomp's runtime/): its overrides registered by guest address, the services
// they use while they run (the kernel, the EE clock, the hardware registers, the frame boundary), and the host behind
// them (bozo-run's log host for the pinned check, the free host for the real runtime). Spec: docs/specs/abi.md
// section 5 and docs/specs/kernel.md section 3b.
//
// An override is a RecompiledFunction bound to a guest address: calls to that address reach it instead of the guest
// code there. It may call generated functions by name (`foo_0x1234(rdram, ctx, rt)`); while it runs, the guest code it
// calls is never stopped at a checkpoint, and a stop it calls for (a call that blocks, a kernel call that gives way)
// is deferred to the override's return (kernel.md section 3b). Every override address must be an external of the
// generated code (bozo-recomp --externals): checkRegistrations says which are not.
#pragma once

#include "bozo/runtime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bozo::ee
{
    class Dispatcher;
    class Kernel;
}

namespace bozo::libs
{
    class Libraries;
}

namespace bozo::audio
{
    class PortStream;
}

namespace bozo::port
{
    using Function = RecompiledFunction;
    using InstructionHandler = bool (*)(uint8_t *, R5900Context *, Runtime *, uint32_t pc);

    // The port's overrides, as its install function registers them.
    class Registry
    {
    public:
        // The fork's name, kept so a port's registration lines move unchanged.
        void registerFunction(uint32_t address, Function fn);
        const std::vector<BozoFunctionEntry> &functions() const { return functions_; }
        Function find(uint32_t address) const;
        // Native instruction replacements run inline, including in delay slots. They must not block or change pc/ra.
        void nativeGraphics(InstructionHandler handler) { instructionHandler_ = handler; }
        InstructionHandler instructionHandler() const { return instructionHandler_; }
        // A native VU0 microprogram, by its entry in VU0's code memory (VCALLMS and VCALLMSR). A port with any replaces
        // the built-in table entirely; a later one for an entry replaces it. Runs inline: must not block or change pc/ra.
        void vu0Program(uint32_t entry, Function fn);
        const std::vector<BozoFunctionEntry> &vu0Programs() const { return vu0Programs_; }
        // A port with no transfer left in flight whatever the game's registers say: BC0F and BC0T answer "done" in free
        // mode, also after a FlushCache and with no channel in D_PCR, where the condition's TMB rule says "not done".
        void transfersAlwaysDone() { transfersAlwaysDone_ = true; }
        bool transfersAreAlwaysDone() const { return transfersAlwaysDone_; }
        // The sync-V callback runs at each vblank's start, as libgraph's vblank handler calls it, instead of at the
        // native frame boundary: for a game whose frame waits on a semaphore the callback signals, so the threads below
        // the waiting one run until the vblank.
        void syncVCallbackAtVblank() { syncVAtVblank_ = true; }
        bool syncVCallbackIsAtVblank() const { return syncVAtVblank_; }

    private:
        std::vector<BozoFunctionEntry> functions_; // in registration order; a later one for an address replaces it
        std::vector<BozoFunctionEntry> vu0Programs_;
        InstructionHandler instructionHandler_ = nullptr;
        bool transfersAlwaysDone_ = false;
        bool syncVAtVblank_ = false;
    };

    // A port: its game and how to install it. One per executable (BOZO_PORT).
    struct Port
    {
        const char *title = "";
        const char *elfName = "";   // the executable it is for (SCUS_971.01)
        uint32_t entry = 0;         // its entry point, to tell it apart from another release
        void (*install)(Registry &) = nullptr;
    };
    void setPort(const Port *port);
    const Port *port(); // null when the executable has none

    // The registered overrides that are not externals of the generated code (bozo-recomp --externals:
    // <prefix>_external_table): direct calls would bypass each of them silently.
    std::vector<uint32_t> undeclared(const Registry &registry, const BozoExternalEntry *table, size_t count);

    // The host behind the overrides: what only it knows or decides.
    class Host
    {
    public:
        virtual ~Host() = default;
        // The native libraries (their state: pads, the movie decoder, the heap...).
        virtual libs::Libraries &libraries() = 0;
        // The game frame ends here, inside an override (the port's frame boundary: native frames). The host takes the
        // frame's record (bozo-run) or hands the frame over (the free host); the EE clock is where the override left it.
        virtual void frameEnd(Runtime *rt, R5900Context *ctx) = 0;
        // Whether nothing the host does may change the run: the pinned check, headless runs (the port then keeps
        // everything that depends on host timing out: determinism.h).
        virtual bool deterministic() const { return true; }
        // Whether the game runs on the host's clock (a window, sound): its frames are held to their rate. A headless or
        // checked run goes as fast as the host makes frames.
        virtual bool realTime() const { return false; }
        virtual bool nativeGraphics() const { return false; }
        virtual bool nativeInstruction(uint8_t *, R5900Context *, Runtime *, uint32_t) { return false; }
        // The library function BozoEngine answers at `address` (a stub of the generated code), made for an override
        // that calls the original (calling the guest code there would run the game's own library, which the
        // reference never did). False when nothing native answers there.
        virtual bool callLibrary(Runtime *rt, R5900Context *ctx, uint32_t address) = 0;
    };

    // ---- What overrides use while they run --------------------------------------------------------------------------
    ee::Dispatcher &dispatcher(Runtime *rt);
    ee::Kernel &kernel(Runtime *rt);
    Host &host(Runtime *rt);
    libs::Libraries &libraries(Runtime *rt);
    // The port's PCM into the mixer (a movie's soundtrack): 48 kHz, added after the voices. Thread-safe.
    audio::PortStream &audioStream(Runtime *rt);
    uint8_t *scratchpad(Runtime *rt);
    // The EE clock (cycles), and charging it from inside an override (the events it passes are taken after it).
    uint64_t now(Runtime *rt);
    void accountCycles(Runtime *rt, R5900Context *ctx, uint64_t cycles);
    constexpr uint64_t kEeClockHz = 294912000ull;
    // The vblanks started so far.
    uint64_t vblankCount(Runtime *rt);
    // The running thread's id and its current priority (0: none).
    int32_t currentThread(Runtime *rt);
    uint32_t currentPriority(Runtime *rt);
    // The kernel's ChangeThreadPriority / RotateThreadReadyQueue (their results), made by the override.
    int32_t changePriority(Runtime *rt, int32_t thread, uint32_t priority, uint32_t &old);
    int32_t rotateReadyQueue(Runtime *rt, uint32_t priority);
    // A switch the kernel wants happens now (as at a kernel call's end); when the running thread gives way, the
    // override's stop is deferred to its return, the thread going on at ctx->pc. Returns whether it gave way.
    bool transfer(Runtime *rt, R5900Context *ctx);
    // A hardware register (T0's count, ...) as guest code would read or write it, through the I/O library.
    uint32_t ioRead32(Runtime *rt, R5900Context *ctx, uint32_t address);
    void ioWrite32(Runtime *rt, R5900Context *ctx, uint32_t address, uint32_t value);
    // The game's frame ends here (Host::frameEnd).
    void frameEnd(Runtime *rt, R5900Context *ctx);
    // The library function at a stub's address, made natively (Host::callLibrary): an override's call of the
    // original where the original is a library BozoEngine implements (sceGsSetDefLoadImage, sceGsSyncV...).
    void callLibrary(Runtime *rt, R5900Context *ctx, uint32_t address);
    bool deterministic(Runtime *rt);
    bool realTime(Runtime *rt);
    inline bool nativeGraphics(Runtime *rt) { return rt->port && rt->port->nativeGraphics(); }

    // ---- Hooks a port sets ------------------------------------------------------------------------------------------
    // The game's stores to hardware registers: true for a store whose effect is not to be made (as if the instruction
    // were patched out: a DMA kick of something the port draws itself). The store is still an out-call (the reference
    // logged it); the I/O library leaves it without effect. `pc` is the store's instruction when the generated code set
    // it (a "store 0xADDR" line of the externals).
    using StorePatch = bool (*)(uint32_t pc, uint32_t address, uint32_t value);
    void setStorePatch(StorePatch patch);
    StorePatch storePatch();
}

// A port's registration, at namespace scope in one of its files:
//   BOZO_PORT("Twisted Metal - Black (USA)", "SCUS_971.01", 0x100008u, &install)
#define BOZO_PORT_CAT2(a, b) a##b
#define BOZO_PORT_CAT(a, b) BOZO_PORT_CAT2(a, b)
#define BOZO_PORT(title, elf, entry, install)                                                                          \
    namespace                                                                                                          \
    {                                                                                                                  \
        const ::bozo::port::Port BOZO_PORT_CAT(s_bozoPort, __LINE__){title, elf, entry, install};                      \
        const bool BOZO_PORT_CAT(s_bozoPortSet, __LINE__) =                                                            \
            (::bozo::port::setPort(&BOZO_PORT_CAT(s_bozoPort, __LINE__)), true);                                       \
    }
