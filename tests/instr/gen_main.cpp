// bozo-test-gen: recompiles every instruction test case's guest code into C++ (prefix "instr"), through the same
// analysis and emitter as bozo-recomp.
//
//   bozo-test-gen <out dir>
#include "cases.h"

#include "analysis.h"
#include "emitter.h"
#include "program.h"

#include <cstdio>
#include <cstring>

using namespace itest;

namespace
{
    void put32(bozo::Program &p, uint32_t addr, uint32_t v)
    {
        std::memcpy(p.image.data() + (addr - p.base), &v, 4);
    }

    // Writes a function's words (with the "jr ra; nop" every case ends with) and its symbol.
    void putFunction(bozo::Program &p, uint32_t addr, std::vector<uint32_t> words, const std::string &name)
    {
        words.push_back(tasm::jr(tasm::ra));
        words.push_back(tasm::nop);
        for (size_t k = 0; k < words.size(); ++k)
            put32(p, addr + 4u * static_cast<uint32_t>(k), words[k]);
        const uint32_t size = 4u * static_cast<uint32_t>(words.size());
        p.code.push_back({addr, addr + size, name});
        p.symbols.push_back({name, addr, size, true});
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: bozo-test-gen <out dir>\n");
        return 2;
    }
    const auto &cases = allCases();
    bozo::Program p;
    p.base = kCodeBase;
    p.entry = kCodeBase;
    const uint32_t dataEnd = kDataBase + static_cast<uint32_t>(cases.size()) * kDataStride;
    p.image.assign(dataEnd - kCodeBase, 0);
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const Env e = envFor(i);
        putFunction(p, e.base, cases[i].code, "t" + std::to_string(i));
        if (!cases[i].callee.empty())
            putFunction(p, e.callee, cases[i].callee, "t" + std::to_string(i) + "_callee");
        for (const auto &[addr, word] : cases[i].data)
            put32(p, addr, word);
    }
    p.data.push_back({kDataBase, dataEnd, "data"});

    bozo::Analysis analysis(p);
    analysis.run();
    for (const auto &w : analysis.stats().warnings)
        std::printf("bozo-test-gen: warning: %s\n", w.c_str());

    bozo::EmitOptions options;
    options.prefix = "instr";
    for (size_t i = 0; i < cases.size(); ++i)
        for (uint32_t offset : cases[i].nativeOffsets)
        {
            options.nativePcs.insert(envFor(i).base + offset);
            options.storePcs.insert(envFor(i).base + offset);
        }
    for (size_t i = 0; i < cases.size(); ++i)
        if (cases[i].calleeExternal)
            options.externals[envFor(i).callee] =
                cases[i].calleeExternal == 1 ? bozo::ExternalKind::Stub : bozo::ExternalKind::Override;
    options.maxBytesPerFile = ~size_t(0); // one file: instr_000.cpp
    bozo::Emitter emitter(analysis, options);
    std::string error;
    if (!emitter.emitAll(argv[1], error))
    {
        std::fprintf(stderr, "bozo-test-gen: %s\n", error.c_str());
        return 1;
    }
    std::printf("bozo-test-gen: %zu cases, %u functions\n", cases.size(), emitter.stats().functions);
    return 0;
}
