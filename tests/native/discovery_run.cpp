// Runs the code generated from test_discovery.py's program without symbols: what discovery kept must still execute.
#include "bozo/runtime.h"
#include "discovery_functions.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "addresses.h"

static void check(bool value, const char *what)
{
    if (!value)
        throw std::runtime_error(what);
}

namespace
{
    struct NoServices : bozo::RuntimeServices
    {
        void unknownTarget(uint8_t *, R5900Context *, Runtime *, uint32_t) override { throw std::runtime_error("unknown target"); }
        void yield(uint8_t *, R5900Context *, Runtime *) override {}
        void syscall(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        void breakpoint(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        void trap(uint8_t *, R5900Context *, Runtime *) override {}
        void unimplemented(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        void eret(uint8_t *, R5900Context *, Runtime *) override {}
        uint32_t cop0Read(R5900Context *, Runtime *, uint32_t) override { return 0; }
        void cop0Write(R5900Context *, Runtime *, uint32_t, uint32_t) override {}
        bool cop0Condition(R5900Context *, Runtime *) override { return false; }
        void vu0CallMicroprogram(uint8_t *, R5900Context *, Runtime *, uint32_t) override {}
        uint32_t vu0ControlRead(R5900Context *, Runtime *, uint32_t) override { return 0; }
        void vu0ControlWrite(R5900Context *, Runtime *, uint32_t, uint32_t) override {}
        void ioRead(R5900Context *, Runtime *, uint32_t, uint32_t, uint64_t[2]) override {}
        void ioWrite(R5900Context *, Runtime *, uint32_t, uint32_t, const uint64_t[2]) override {}
    };
}

int main(int argc, char **argv)
{
    try
    {
        check(argc == 2, "usage");
        std::ifstream in(argv[1], std::ios::binary);
        const std::vector<char> elf{std::istreambuf_iterator<char>(in), {}};
        Runtime rt;
        NoServices services;
        rt.services = &services;
        const uint32_t codeBytes = kCodeEnd - k_main;
        std::memcpy(rt.memory.ram() + k_main, elf.data() + kFileCode, codeBytes);
        std::memcpy(rt.memory.ram() + kData, elf.data() + kFileCode + codeBytes, kDataWords * 4);
        rt.functions.setGenerated(discovery_function_table, discovery_function_count);
        rt.functions.setResume(discovery_resume_table, discovery_resume_count);

        R5900Context ctx;
        const uint32_t returnTo = 0x200000;
        const auto call = [&](uint32_t target, uint32_t a0) {
            ctx.r[31].uw[0] = returnTo;
            ctx.r[4].uw[0] = a0;
            bozo_call_indirect(rt.memory.flat(), &ctx, &rt, target);
            check(ctx.pc == returnTo, "returned");
            return static_cast<int32_t>(ctx.r[2].uw[0]);
        };
        check(call(k_switcher, 0) == 10, "switch case 0");
        check(call(k_switcher, 1) == 11, "switch case 1");
        check(call(k_switcher, 2) == 12, "switch case 2");
        check(call(k_switcher, 3) == -1, "switch default");
        check(call(k_slots, 0) == 20, "code slot 0");
        check(call(k_slots, 1) == 21, "code slot 1");
        check(call(k_by_pointer, 0) == 7, "function only a data word points at");
        check(call(k_through_register, kPointerWord) == 7, "tail call through a register");
        check(call(k_tail_to_next, 0) == 5, "tail call to the next function");
        check(call(k_early_return, 1) == 1 && call(k_early_return, 0) == 2, "two returns in a function");
        check(call(k_backward, 0) == 3, "a branch back over a return");
        check(rt.functions.generated(k_switcher + 4) != nullptr, "a data word into a body is an entry");
        std::puts("discovery: switches, code slots, pointers and tail calls run");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
