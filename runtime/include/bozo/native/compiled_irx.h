// SPDX-License-Identifier: MIT
// Team-authored generated-code ABI moved after a per-line authorship audit (docs/native-irx.md).
// Register values and plain library buffers for statically compiled routines. No CPU or device emulator.
#pragma once
#include <cstddef>
#include <cstdint>

namespace bozo::native::irx
{
    // Register values retained by generated native functions across cooperative yields.
    struct NativeCpu
    {
        uint32_t gpr[32];
        uint32_t hi;
        uint32_t lo;
        uint32_t pc;
        uint32_t cop0[32];
    };

    class Env
    {
      public:
        explicit Env(NativeCpu &cpu) : cpu(cpu) {}
        virtual ~Env() = default;

        NativeCpu &cpu;

        // Reads as the guest sees them: the image/data buffer directly; addresses outside it must be rejected by the
        // host.
        uint8_t r8(uint32_t address)
        {
            const uint32_t physical = address & 0x1FFFFFFFu;
            return physical < ramSize ? ram[physical] : read8(address);
        }
        uint16_t r16(uint32_t address)
        {
            const uint32_t physical = address & 0x1FFFFFFFu;
            if (physical + 1u < ramSize)
                return static_cast<uint16_t>(ram[physical] | (ram[physical + 1u] << 8));
            return read16(address);
        }
        uint32_t r32(uint32_t address)
        {
            const uint32_t physical = address & 0x1FFFFFFFu;
            if (physical + 3u < ramSize)
                return static_cast<uint32_t>(ram[physical]) | (static_cast<uint32_t>(ram[physical + 1u]) << 8) |
                       (static_cast<uint32_t>(ram[physical + 2u]) << 16) |
                       (static_cast<uint32_t>(ram[physical + 3u]) << 24);
            return read32(address);
        }
        // The native library image/data buffer (including streams refilled in place).
        const uint8_t *ramBase() const { return ram; }
        uint32_t ramBytes() const { return ramSize; }

        // Writes are bounds-checked by the native host. There are no device-register effects.
        void w8(uint32_t address, uint8_t value) { write8(address, value); }
        void w16(uint32_t address, uint16_t value) { write16(address, value); }
        void w32(uint32_t address, uint32_t value) { write32(address, value); }

        // Work accounting for cooperative native callbacks.
        void tick(uint32_t instructions) { executed += instructions; }
        // The time slice is spent: a loop stops at its head (cpu.pc) and returns for the host to run other native
        // callbacks.
        bool expired() const { return executed >= budget; }

        // A call (JAL / JALR, cpu.gpr[31] already set to ret): runs target and what it leads to, through statically
        // linked functions and native SDK bindings, until the guest is back at ret. False when it stopped before (a
        // blocking call, the end of the slice, a yielded callback): cpu.pc is where it goes on, and the caller returns
        // at once.
        virtual bool call(uint32_t target, uint32_t ret) = 0;

        uint64_t executed = 0;
        uint64_t budget = 0;

      protected:
        uint8_t *ram = nullptr;
        uint32_t ramSize = 0;
        virtual uint8_t read8(uint32_t address) = 0;
        virtual uint16_t read16(uint32_t address) = 0;
        virtual uint32_t read32(uint32_t address) = 0;
        virtual void write8(uint32_t address, uint8_t value) = 0;
        virtual void write16(uint32_t address, uint16_t value) = 0;
        virtual void write32(uint32_t address, uint32_t value) = 0;
    };

    using Function = void (*)(Env &);

    struct Entry
    {
        uint32_t pc;
        Function function;
    };

    // Generated code: one per module, at namespace scope (static initialization).
    struct Registration
    {
        Registration(const char *name, uint64_t fileHash, uint32_t base, uint32_t size, const Entry *entries,
                     size_t count);
    };

    // R3000 integer division semantics, emitted as native arithmetic.
    inline void divide(NativeCpu &c, uint32_t rs, uint32_t rt)
    {
        const int32_t lhs = static_cast<int32_t>(rs), rhs = static_cast<int32_t>(rt);
        if (rhs == 0)
        {
            c.lo = lhs >= 0 ? 0xFFFFFFFFu : 1u;
            c.hi = static_cast<uint32_t>(lhs);
        }
        else if (lhs == INT32_MIN && rhs == -1)
        {
            c.lo = static_cast<uint32_t>(lhs);
            c.hi = 0u;
        }
        else
        {
            c.lo = static_cast<uint32_t>(lhs / rhs);
            c.hi = static_cast<uint32_t>(lhs % rhs);
        }
    }
    inline void divideUnsigned(NativeCpu &c, uint32_t rs, uint32_t rt)
    {
        if (rt == 0u)
        {
            c.lo = 0xFFFFFFFFu;
            c.hi = rs;
        }
        else
        {
            c.lo = rs / rt;
            c.hi = rs % rt;
        }
    }
    inline uint32_t loadLeft(Env &e, uint32_t address, uint32_t old)
    {
        static constexpr uint32_t masks[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
        static constexpr uint32_t shifts[4] = {24u, 16u, 8u, 0u};
        return (old & masks[address & 3u]) | (e.r32(address & ~3u) << shifts[address & 3u]);
    }
    inline uint32_t loadRight(Env &e, uint32_t address, uint32_t old)
    {
        static constexpr uint32_t masks[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
        static constexpr uint32_t shifts[4] = {0u, 8u, 16u, 24u};
        return (old & masks[address & 3u]) | (e.r32(address & ~3u) >> shifts[address & 3u]);
    }
    inline void storeLeft(Env &e, uint32_t address, uint32_t value)
    {
        const uint32_t aligned = address & ~3u, old = e.r32(aligned);
        static constexpr uint32_t masks[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u};
        static constexpr uint32_t shifts[4] = {24u, 16u, 8u, 0u};
        e.w32(aligned, (old & masks[address & 3u]) | (value >> shifts[address & 3u]));
    }
    inline void storeRight(Env &e, uint32_t address, uint32_t value)
    {
        const uint32_t aligned = address & ~3u, old = e.r32(aligned);
        static constexpr uint32_t masks[4] = {0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};
        static constexpr uint32_t shifts[4] = {0u, 8u, 16u, 24u};
        e.w32(aligned, (old & masks[address & 3u]) | (value << shifts[address & 3u]));
    }
} // namespace bozo::native::irx
