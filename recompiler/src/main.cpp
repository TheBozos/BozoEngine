// bozo-recomp: recompile a PS2 executable's EE code to C++.
//
//   bozo-recomp <elf> <out dir> [options]
//   bozo-recomp <elf> --census           only decode the code and count each instruction
//
// Options: --prefix <name> (file and table names, default "bozo"), --max-file-bytes <n> (split size of the generated
// .cpp files, default 1 MiB), --no-comments (no disassembly comment per instruction), --verbose (list the
// unresolved jr instructions and the extra entry points).
#include "analysis.h"
#include "decoder.h"
#include "elf.h"
#include "emitter.h"
#include "program.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace bozo;

namespace
{
    int usage()
    {
        std::fprintf(stderr, "usage: bozo-recomp <elf> <out dir> [--prefix name] [--max-file-bytes n] [--no-comments]\n"
                             "       bozo-recomp <elf> --census\n");
        return 2;
    }

    // Decode every word of the code and count each operation. Returns the number of unknown words.
    uint64_t census(const Program &p, bool print)
    {
        std::vector<uint64_t> counts(kOpCount, 0);
        std::map<uint32_t, uint32_t> unknown; // word -> count
        uint64_t total = 0;
        for (const auto &r : p.code)
            for (uint32_t a = r.start; a + 4 <= r.end; a += 4)
            {
                const uint32_t w = p.read32(a);
                const Op op = decode(w);
                ++counts[static_cast<unsigned>(op)];
                ++total;
                if (op == Op::Unknown)
                    ++unknown[w];
            }
        uint64_t distinct = 0;
        for (unsigned k = 1; k < kOpCount; ++k)
            distinct += counts[k] != 0;
        std::printf("decode: %llu instruction words in", static_cast<unsigned long long>(total));
        for (const auto &r : p.code)
            std::printf(" %s [0x%x, 0x%x)", r.name.c_str(), r.start, r.end);
        std::printf("\ndecode: %llu distinct operations, %llu unknown words\n", static_cast<unsigned long long>(distinct),
                    static_cast<unsigned long long>(counts[0]));
        if (print)
        {
            std::vector<unsigned> order;
            for (unsigned k = 1; k < kOpCount; ++k)
                if (counts[k])
                    order.push_back(k);
            std::sort(order.begin(), order.end(), [&](unsigned x, unsigned y) { return counts[x] > counts[y]; });
            for (unsigned k : order)
                std::printf("  %-10s %8llu  (op %u)\n", opName(static_cast<Op>(k)), static_cast<unsigned long long>(counts[k]), k);
        }
        for (const auto &[w, n] : unknown)
            std::printf("  unknown word 0x%08x x%u\n", w, n);
        return counts[0];
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    const std::string elfPath = argv[1];
    const bool censusOnly = std::strcmp(argv[2], "--census") == 0;
    const std::string outDir = censusOnly ? std::string() : argv[2];
    EmitOptions options;
    bool verbose = false;
    for (int k = 3; k < argc; ++k)
    {
        const std::string a = argv[k];
        if (a == "--prefix" && k + 1 < argc)
            options.prefix = argv[++k];
        else if (a == "--max-file-bytes" && k + 1 < argc)
            options.maxBytesPerFile = std::strtoull(argv[++k], nullptr, 0);
        else if (a == "--verbose")
            verbose = true;
        else if (a == "--no-comments")
            options.comments = false;
        else
            return usage();
    }

    const auto t0 = std::chrono::steady_clock::now();
    ElfFile elf;
    if (!elf.load(elfPath))
    {
        std::fprintf(stderr, "bozo-recomp: %s\n", elf.error().c_str());
        return 1;
    }
    if (elf.machine() != 8)
        std::fprintf(stderr, "bozo-recomp: warning: ELF machine %u is not MIPS (8)\n", elf.machine());
    const Program program = programFromElf(elf);
    const uint64_t unknown = census(program, censusOnly);
    if (censusOnly)
        return unknown == 0 ? 0 : 1;

    Analysis analysis(program);
    analysis.run();
    const AnalysisStats &as = analysis.stats();
    std::printf("functions: %zu (%u from ELF function symbols, %u carved from code no symbol covers), %u extra entries\n",
                analysis.functions().size(), as.symbolFunctions, as.gapFunctions, as.extraEntries);
    std::printf("calls: %u distinct call targets; code pointers: %u in data, %u built by lui/addiu\n", as.callTargets,
                as.dataCodePointers, as.codePointerConstants);
    std::printf("jump tables: %u (%u entries); returns through a copy of ra: %u; jr through an unknown register value: %u; "
                "branches in delay slots: %u\n",
                as.jumpTables, as.jumpTableTargets, as.returnJr, as.unresolvedJr, as.branchesInDelaySlots);
    for (const auto &w : as.warnings)
        std::printf("  warning: %s\n", w.c_str());
    if (verbose)
        for (const Function &f : analysis.functions())
        {
            for (uint32_t a : f.unresolvedJr)
                std::printf("  jr through a register at 0x%x in %s\n", a, cppName(f.symbol, f.start).c_str());
            for (uint32_t e : f.extraEntries)
                std::printf("  extra entry 0x%x in %s\n", e, cppName(f.symbol, f.start).c_str());
        }

    Emitter emitter(analysis, options);
    std::string error;
    if (!emitter.emitAll(outDir, error))
    {
        std::fprintf(stderr, "bozo-recomp: %s\n", error.c_str());
        return 1;
    }
    const EmitStats &es = emitter.stats();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("emitted: %u functions, %u table entries, %u instructions, %u files, %.1f MB, in %.2f s\n", es.functions,
                es.entries, es.instructions, es.files, es.bytes / 1e6, seconds);
    uint32_t untranslated = 0;
    for (unsigned k = 0; k < kOpCount; ++k)
        if (es.untranslated[k])
        {
            std::printf("  untranslated: %s x%u\n", opName(static_cast<Op>(k)), es.untranslated[k]);
            untranslated += es.untranslated[k];
        }
    std::printf("untranslated instructions: %u\n", untranslated);
    return 0;
}
