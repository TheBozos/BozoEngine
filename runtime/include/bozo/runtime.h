// BozoEngine's runtime core: what recompiled code runs on (guest memory, the function table and overrides, COP0's
// Status) and the interface through which the runtime's services (the kernel, the native libraries, VU0's native
// microprograms, I/O) answer the generated code's requests.
//
// This is the start of phase 2's runtime (docs/PLAN.md). The replay harness (tools/replay, docs/specs/replay.md) is
// its first user: it plugs in services that answer every request from a capture's log. The real runtime will plug in
// the kernel and native libraries behind the same interface.
#pragma once

#include "bozo/runtime_api.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bozo
{
    // ---- Guest memory --------------------------------------------------------------------------------------------
    // One host block holding main RAM (32 MB), the scratchpad (16 KB) and VU0's data memory (4 KB), in that order: the
    // "flat space" of docs/specs/capture.md section 3. Every other guest address has no memory behind it (it goes to
    // the services as I/O).
    constexpr uint32_t kFlatRamOffset = 0x0000000u;
    constexpr uint32_t kFlatScratchpadOffset = 0x2000000u;
    constexpr uint32_t kFlatVu0Offset = 0x2004000u;
    constexpr uint32_t kFlatBytes = 0x2005000u;
    constexpr uint32_t kScratchpadBase = 0x70000000u;
    constexpr uint32_t kScratchpadBytes = 0x4000u;
    constexpr uint32_t kVu0DataBase = 0x11004000u; // VU0 data memory as the EE sees it
    constexpr uint32_t kVu0DataBytes = 0x1000u;

    class GuestMemory
    {
    public:
        GuestMemory();
        uint8_t *flat() { return flat_.get(); }
        const uint8_t *flat() const { return flat_.get(); }
        uint8_t *ram() { return flat_.get() + kFlatRamOffset; }
        uint8_t *scratchpad() { return flat_.get() + kFlatScratchpadOffset; }
        uint8_t *vu0Data() { return flat_.get() + kFlatVu0Offset; }

        // The flat offset of a guest address with `size` bytes of memory behind it, or -1 (I/O, nothing there).
        // RAM through KSEG0/KSEG1 and the EE's uncached views; the scratchpad; VU0 data memory.
        static int64_t flatOffset(uint32_t address, uint32_t size);
        uint8_t *hostPointer(uint32_t address, uint32_t size)
        {
            const int64_t o = flatOffset(address, size);
            return o < 0 ? nullptr : flat_.get() + o;
        }

    private:
        std::unique_ptr<uint8_t[]> flat_;
    };

    // ---- Functions -------------------------------------------------------------------------------------------------
    // The generated function table (every guest function and extra entry by address, sorted) and the overrides: guest
    // addresses bound to native functions (the port's replacements and the native library calls, abi.md section 5).
    // Overrides are looked up first. Before an override runs, ctx->pc holds the address it was reached at.
    class FunctionTable
    {
    public:
        void setGenerated(const BozoFunctionEntry *table, size_t count)
        {
            table_ = table;
            count_ = count;
        }
        RecompiledFunction generated(uint32_t address) const; // nullptr if none
        void setOverride(uint32_t address, RecompiledFunction fn);
        RecompiledFunction override(uint32_t address) const; // nullptr if none
        void clearOverrides() { overrides_.clear(); }
        size_t overrideCount() const { return overrides_.size(); }

    private:
        const BozoFunctionEntry *table_ = nullptr;
        size_t count_ = 0;
        std::vector<BozoFunctionEntry> overrides_; // sorted by address
    };

    // ---- Services --------------------------------------------------------------------------------------------------
    // What the runtime core passes on. Every request recompiled code makes outside memory and the function table.
    class RuntimeServices
    {
    public:
        virtual ~RuntimeServices() = default;
        // A transfer to an address with neither an override nor generated code.
        virtual void unknownTarget(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t target) = 0;
        virtual void yield(uint8_t *rdram, R5900Context *ctx, Runtime *rt) = 0;
        virtual void syscall(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t code) = 0;
        virtual void breakpoint(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t code) = 0;
        virtual void trap(uint8_t *rdram, R5900Context *ctx, Runtime *rt) = 0;
        virtual void unimplemented(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t word) = 0;
        virtual void eret(uint8_t *rdram, R5900Context *ctx, Runtime *rt) = 0;
        // COP0 registers other than Status (12), which the core holds.
        virtual uint32_t cop0Read(R5900Context *ctx, Runtime *rt, uint32_t reg) = 0;
        virtual void cop0Write(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t value) = 0;
        virtual bool cop0Condition(R5900Context *ctx, Runtime *rt) = 0;
        // ei / di, after the core changed Status.EIE: the scheduler's chance to run pending interrupt handlers.
        virtual void interruptsChanged(uint8_t *, R5900Context *, Runtime *, bool) {}
        virtual void vu0CallMicroprogram(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t address) = 0;
        virtual uint32_t vu0ControlRead(R5900Context *ctx, Runtime *rt, uint32_t reg) = 0;
        virtual void vu0ControlWrite(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t value) = 0;
        // Loads and stores to addresses with no memory behind them (size 1, 2, 4, 8 or 16; value[0] is the low half).
        virtual void ioRead(R5900Context *ctx, Runtime *rt, uint32_t address, uint32_t size, uint64_t value[2]) = 0;
        virtual void ioWrite(R5900Context *ctx, Runtime *rt, uint32_t address, uint32_t size, const uint64_t value[2]) = 0;
    };

    constexpr uint32_t kCop0Status = 12;
    constexpr uint32_t kStatusEIE = 1u << 16; // the EE's master interrupt enable (ei sets it, di clears it)
}

// The runtime recompiled code receives as `rt` (opaque to it).
struct Runtime
{
    bozo::GuestMemory memory;
    bozo::FunctionTable functions;
    bozo::RuntimeServices *services = nullptr;
    uint32_t cop0Status = 0;
};
