// The per-instruction tests: each case is a tiny guest program (raw instruction words), the state it starts from and
// the state it must end in. The same list is used twice: by bozo-test-gen, which recompiles every case's code into
// C++, and by bozo-instr-tests, which runs that C++ and checks the results. Expected values come from
// docs/specs/r5900.md, worked out by hand.
#pragma once

#include "asm.h"
#include "stub_runtime.h"

#include "bozo/context.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace itest
{
    // Where case i lives in the guest address space.
    constexpr uint32_t kCodeBase = 0x00100000u;
    constexpr uint32_t kCaseStride = 0x1000u;
    constexpr uint32_t kCalleeOffset = 0x800u; // a second function, for calls and tail calls
    constexpr uint32_t kDataBase = 0x00400000u; // case i's data at kDataBase + i * kDataStride
    constexpr uint32_t kDataStride = 0x400u;
    constexpr uint32_t kReturnSentinel = 0x0BAD0000u; // ra when a case is called

    struct Env
    {
        uint32_t base;   // the case's main function
        uint32_t callee; // its second function
        uint32_t data;   // its data area (guest RAM, also part of the program image the recompiler sees)
    };

    class Checker
    {
    public:
        explicit Checker(std::string name) : name_(std::move(name)) {}
        void eq(const char *what, uint64_t got, uint64_t want);
        void events(const Runtime &rt, const std::vector<std::string> &want);
        int failures() const { return failures_; }

    private:
        std::string name_;
        int failures_ = 0;
    };

    struct Case
    {
        std::string name;
        std::vector<uint32_t> code;   // the main function (a "jr ra; nop" is appended)
        std::vector<uint32_t> callee; // the second function (empty: none)
        std::vector<std::pair<uint32_t, uint32_t>> data; // (address, word) put in guest RAM before the run
        int calleeExternal = 0; // 1: the callee's address is a "stub" external, 2: an "override" (EmitOptions::externals)
        std::function<void(R5900Context &, uint8_t *, Runtime &)> setup;
        std::function<void(Checker &, R5900Context &, uint8_t *, Runtime &)> check;
    };

    // Every case, case i at kCodeBase + i * kCaseStride.
    const std::vector<Case> &allCases();

    inline Env envFor(size_t index)
    {
        const uint32_t base = kCodeBase + static_cast<uint32_t>(index) * kCaseStride;
        return {base, base + kCalleeOffset, kDataBase + static_cast<uint32_t>(index) * kDataStride};
    }

    // Helpers for setups and checks.
    inline uint32_t fbits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    inline uint64_t sx(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
    inline uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + a, 4);
        return v;
    }
    inline uint64_t rd64(const uint8_t *ram, uint32_t a)
    {
        uint64_t v;
        std::memcpy(&v, ram + a, 8);
        return v;
    }
    inline void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); }
    inline void wr64(uint8_t *ram, uint32_t a, uint64_t v) { std::memcpy(ram + a, &v, 8); }
}
