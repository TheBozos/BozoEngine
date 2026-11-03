// What a native library function works on: the calling context (arguments, results) and guest memory. Every write
// can be journaled (the bytes it replaced), so a harness can compare a native call's effect with a recorded one and
// undo it. Spec: docs/specs/libraries.md section 1.
#pragma once

#include "bozo/runtime.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bozo::libs
{
    // The memory a native call wrote, with the bytes each write replaced (in write order).
    struct Journal
    {
        struct Entry
        {
            uint32_t flat = 0;   // flat offset (runtime.h)
            uint32_t length = 0;
            size_t oldAt = 0;    // where its old bytes start in `old`
        };
        std::vector<Entry> entries;
        std::vector<uint8_t> old;
        void clear()
        {
            entries.clear();
            old.clear();
        }
    };

    // Thrown by a native function on an access outside guest memory (a bad pointer): the caller reports it.
    struct BadAccess
    {
        uint32_t address = 0, bytes = 0;
    };

    class Guest
    {
    public:
        Guest(Runtime &rt, R5900Context *ctx, Journal *journal = nullptr) : rt_(rt), ctx_(ctx), journal_(journal) {}

        Runtime &runtime() { return rt_; }
        R5900Context *context() { return ctx_; }

        // ---- Arguments (abi.md: a0-a3, t0-t3, then 64-bit stack slots from sp) and results ----
        uint32_t arg(int i) const { return static_cast<uint32_t>(arg64(i)); }
        int32_t sarg(int i) const { return static_cast<int32_t>(arg(i)); }
        uint64_t arg64(int i) const;
        uint32_t floatArg(int i) const { return ctx_->fu[12 + i]; } // f12, f13, ... as bits
        // v0 as the reference's library calls leave it: the value sign-extended to 64 bits, the upper 64 bits cleared.
        void ret(int32_t v)
        {
            ctx_->r[2].sd[0] = v;
            ctx_->r[2].ud[1] = 0;
        }
        void retU(uint32_t v) { ret(static_cast<int32_t>(v)); }
        void retFloat(uint32_t bits) { ctx_->fu[0] = bits; }
        // The EE cycles the call takes (a timed call's; 0 for the others).
        uint64_t cycles = 0;

        // ---- Memory ----
        // A host pointer to `bytes` bytes of guest memory at `address`; throws BadAccess when there is none.
        const uint8_t *in(uint32_t address, uint32_t bytes) const;
        uint8_t read8(uint32_t address) const { return *in(address, 1); }
        uint16_t read16(uint32_t address) const;
        uint32_t read32(uint32_t address) const;
        // A NUL-terminated string (at most `limit` bytes are read; the terminator is not part of it).
        std::string string(uint32_t address, uint32_t limit = 0x10000) const;
        // The length of the string at `address` (strlen), reading through the guest's memory.
        uint32_t stringLength(uint32_t address, uint32_t limit = 0x1000000) const;

        void write(uint32_t address, const void *data, uint32_t bytes);
        void fill(uint32_t address, uint8_t value, uint32_t bytes);
        // Copies inside guest memory (overlap allowed: memmove semantics).
        void copy(uint32_t to, uint32_t from, uint32_t bytes);
        void write8(uint32_t address, uint8_t v) { write(address, &v, 1); }
        void write16(uint32_t address, uint16_t v) { write(address, &v, 2); }
        void write32(uint32_t address, uint32_t v) { write(address, &v, 4); }

    private:
        uint8_t *out(uint32_t address, uint32_t bytes); // journals the bytes about to be written

        Runtime &rt_;
        R5900Context *ctx_;
        Journal *journal_;
    };
}
