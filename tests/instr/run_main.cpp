// bozo-instr-tests: runs every instruction test case's recompiled code on a fresh context and guest RAM, and checks
// the results. Exit code 0 when all pass.
//
//   bozo-instr-tests [substring]   (only the cases whose name contains it)
#include "cases.h"

#include "instr_functions.h"

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace itest;

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;
    const auto &cases = allCases();
    std::unique_ptr<uint8_t[]> ram(new uint8_t[bozo::kRamSize]);
    auto rt = std::make_unique<Runtime>();
    int passed = 0, failed = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const Case &c = cases[i];
        if (filter && c.name.find(filter) == std::string::npos)
            continue;
        const Env e = envFor(i);
        std::fill(ram.get(), ram.get() + bozo::kRamSize, uint8_t(0));
        *rt = Runtime();
        rt->table = instr_function_table;
        rt->tableSize = instr_function_count;
        auto ctx = std::make_unique<R5900Context>();
        for (const auto &[addr, word] : c.data)
            wr32(ram.get(), addr, word);
        ctx->r[31].ud[0] = kReturnSentinel;
        c.setup(*ctx, ram.get(), *rt);

        const BozoFunctionEntry *end = instr_function_table + instr_function_count;
        const BozoFunctionEntry *fn = std::lower_bound(instr_function_table, end, e.base,
                                                       [](const BozoFunctionEntry &x, uint32_t a) { return x.address < a; });
        Checker k(c.name);
        if (fn == end || fn->address != e.base)
            k.eq("function in the table", 0, 1);
        else
        {
            fn->function(ram.get(), ctx.get(), rt.get());
            c.check(k, *ctx, ram.get(), *rt);
            for (const auto &ev : rt->events)
                if (ev.rfind("unimplemented", 0) == 0 || ev.rfind("unknown-target", 0) == 0)
                    k.eq(("unexpected runtime event: " + ev).c_str(), 0, 1);
            k.eq("r0", ctx->r[0].ud[0] | ctx->r[0].ud[1], 0);
        }
        if (k.failures() == 0)
            ++passed;
        else
            ++failed;
    }
    std::printf("instruction tests: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
