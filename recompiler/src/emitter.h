// The C++ emitter: one C++ function per guest function (abi.md section 1), a header declaring them all and the
// function table.
#pragma once

#include "analysis.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bozo
{
    struct EmitOptions
    {
        std::string prefix = "bozo";          // file names: <prefix>_functions.h, <prefix>_table.cpp, <prefix>_NNN.cpp
        size_t maxBytesPerFile = 1u << 20;    // start a new .cpp past this size (parallel builds, compiler memory)
        bool comments = true;                 // a comment with each instruction's address and disassembly
    };

    struct EmitStats
    {
        uint32_t files = 0;
        uint32_t functions = 0;
        uint32_t entries = 0;                 // table entries: functions + extra entries
        uint32_t instructions = 0;
        uint64_t bytes = 0;                   // generated source size
        std::array<uint32_t, kOpCount> untranslated{}; // decoded but not translated (emitted as bozo_unimplemented)
    };

    class Emitter
    {
    public:
        Emitter(const Analysis &analysis, EmitOptions options) : a_(analysis), opt_(std::move(options)) {}

        // The definition of one function (and of its extra entries' wrappers).
        void emitFunction(const Function &f, std::string &out);
        // The header declaring every function, and the table.
        void emitHeader(std::string &out) const;
        void emitTable(std::string &out) const;
        // Everything into a directory. Returns false (with a message) on a write error.
        bool emitAll(const std::string &dir, std::string &error);

        const EmitStats &stats() const { return stats_; }

    private:
        std::string translate(const Insn &i);
        std::string callTo(uint32_t target) const;
        std::string entryName(const Function &f, uint32_t addr) const;

        const Analysis &a_;
        EmitOptions opt_;
        EmitStats stats_;
    };
}
